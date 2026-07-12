#pragma once

#include <stdint.h>
#include <thread> 
#include <iostream>
#include <bitset>
#include <vector>
#include <array>
#include <cstdint>
#include <cassert>
#include <random>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <common/data.h>
#include <common/packing.h>
#include <common/utils.h>
#include <common/uslpstructs.h>
#include <string>
#include <string_view>
#include <set>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static constexpr bool enableLogs = false;

template <typename... Args>
void log(Args&&... args) {
    if (!enableLogs) return;

    // C++17 Fold-expression: unpacks and prints every parameter sequentially
    (std::cout << ... << std::forward<Args>(args)) << "\n";
}

class VirtualChannelAccumulator {
private:
    uint8_t m_vcid;
    std::chrono::steady_clock::time_point m_lastPacketTime; // Last time a packet was accumulated
    uint16_t m_nextFhp = 0; // Tracker for the next frame's First Header Pointer (FHP)

public:
    AccumulationBuffer m_accumulationBuffer;
    size_t m_fixedTfdzSize; // e.g., 1024 bytes
    std::chrono::steady_clock::time_point m_lastFrameTime; // Last time a frame was transmitted
    std::chrono::steady_clock::time_point m_accumulationStartTime; // USLP-143 State tracking
    std::mutex m_bufferMtx;

    VirtualChannelAccumulator(size_t tfdzSize = MAX_DATA_ZONE_LENGTH) 
        : m_fixedTfdzSize(tfdzSize) {
        m_lastPacketTime = std::chrono::steady_clock::now();
        m_lastFrameTime = std::chrono::steady_clock::now();
    }

    // Handled by VCP.request
    bool processIncomingPacket(const BitBuffer<MAX_MESSAGE_LENGTH>& packetBytes, uint32_t GVCID) {
        std::lock_guard<std::mutex> lock(m_bufferMtx);
        
        m_lastPacketTime = std::chrono::steady_clock::now();
        return m_accumulationBuffer.insert(&packetBytes.data[0], packetBytes.length);
    }
};

class VirtualChannel {
public:
    VirtualChannel(uint8_t vcFrameCountLength = VC_FRAME_COUNT_LENGTH, 
        size_t fixedTfdzSize = MAX_DATA_ZONE_LENGTH)
        : accumulator(fixedTfdzSize)
    {
        initializeMask(vcFrameCountLength);
    }

    void initializeMask(uint8_t vcFrameCountLength) {
        // Clamp length between 1 and 7 per USLP spec safety
        //vcFrameCountLength = std::clamp<uint8_t>(vcFrameCountLength, 1, 7);
        if (vcFrameCountLength < 0 || vcFrameCountLength > 8) {
            std::cerr << "Invalid vcFrameCountLenth value; must be between 1 and 7\n";
            return;
        }

        // Calculate the maximum bitmask for the specified byte width
        // e.g., 1 byte -> 0xFF, 2 bytes -> 0xFFFF, 4 bytes -> 0xFFFFFFFF
        if (vcFrameCountLength == 8) {
            vcFrameCountMask = ~0ULL; // Full 64-bit mask
        } else {
            vcFrameCountMask = (1ULL << (vcFrameCountLength * 8)) - 1;
        }
    }

    void incrementFrameCount() {
        vcFrameCount++;
        vcFrameCount &= vcFrameCountMask;
    }

    VirtualChannelAccumulator accumulator; // keeps track of current bytes to send in TFDF

    std::chrono::milliseconds expirationTime{50}; // USLP-143 (TFDFCompletionTimeoutMs)
    std::chrono::milliseconds interFrameDelay{50}; // USLP-144 (interFrameDelayMs)
    
    uint64_t vcFrameCountMask = 0xFFFFFFFF;
    uint64_t vcFrameCount = 0;
};

static constexpr int maxFinishedTransferFrames = 128;

struct TFAllFormats {
    TransferFrame tf;
    BitBuffer<MAX_TRANSFER_FRAME_LENGTH> serializedBytes;
};

// Helper utility to convert the COP enum class into a string_view
inline std::string_view ToString(USLPConfig::COPType type) {
    switch (type) {
        case USLPConfig::COPType::COP_1: return "COP_1";
        case USLPConfig::COPType::COP_P: return "COP_P";
        case USLPConfig::COPType::NONE:  return "NONE";
        default:                         return "UNKNOWN";
    }
}

class USLP {
public:
    USLP(USLPConfig& managedParams) 
        : packer(managedParams, m_vcidToIndex), 
          managedParams(managedParams),
          m_running(true) 
    {
        PrintVirtualChannelConfigs();

        m_vcidToIndex.fill(-1);

        for (size_t i = 0; i < VC_COUNT; i++) {
            uint8_t targetVCID = managedParams.virtualChannelConfigs[i].VCID; // e.g., 0, 1, 2
        
            // Map the VCID to this exact index 'i' in our dense array
            m_vcidToIndex[targetVCID] = static_cast<int8_t>(i);

            // Read parameters calculated above
            uint8_t byteWidth = managedParams.virtualChannelConfigs[i].expeditedCountLength;
            
            m_virtualChannels[i].initializeMask(byteWidth);

            // Map both temporal specifications to their corresponding channels
            m_virtualChannels[i].expirationTime = std::chrono::milliseconds(
                managedParams.virtualChannelConfigs[i].TFDFCompletionTimeoutMs
            );
            m_virtualChannels[i].interFrameDelay = std::chrono::milliseconds(
                managedParams.virtualChannelConfigs[i].interFrameDelayMs
            );
        }

        // 3. Setup the IDLE channel in the final slot
        size_t idleIndex = NUM_ACTIVE_CHANNELS - 1;
        m_vcidToIndex[IDLE_VCID] = static_cast<int8_t>(idleIndex);
        
        m_virtualChannels[idleIndex].initializeMask(6);
        m_virtualChannels[idleIndex].expirationTime = std::chrono::milliseconds(50);

        PrintVCIDMapping();
        InitNetworkSocket();

        // Spawning threads at the VERY END of constructor to ensure all members 
        // (packer, queues, virtualChannels) are fully initialized in memory first.
        m_packetThread = std::thread(&USLP::VCPacketThread, this);
        m_multiplexerThread = std::thread(&USLP::VCMultiplexer, this);
    }

    ~USLP() {
        CleanupNetworkSocket();
        terminateThreads();
    }

    void terminateThreads() {
        // 1. Signal threads to exit their main while(m_running) loops
        m_running = false;

        // Note: If m_frameMultiplexerQueue uses a blocking wait condition variable,
        // you should call a wake/unblock function here (e.g., m_frameMultiplexerQueue.unblock())
        // so VCMultiplexer wakes up immediately to see m_running == false.

        // 2. Safely join threads before memory cleanup occurs
        if (m_packetThread.joinable()) {
            m_packetThread.join();
        }
        if (m_multiplexerThread.joinable()) {
            m_multiplexerThread.join();
        }
    };
    void PrintVCIDMapping();
    void PrintVirtualChannelConfigs() const {
        std::cout << "\n=============================================\n";
        std::cout << "       USLP VIRTUAL CHANNEL CONFIGURATIONS    \n";
        std::cout << "=============================================\n";

        // Grab the configuration array directly via your class reference member
        const auto& configs = managedParams.virtualChannelConfigs;

        for (size_t i = 0; i < configs.size(); ++i) {
            const auto& config = configs[i];
            
            std::cout << "  Config Index [" << i << "]:"
                    << "  VCID = " << static_cast<int>(config.VCID)
                    << "  |  COP Type = " << ToString(config.COPInEffect) 
                    << "\n";
        }

        std::cout << "=============================================\n\n";
    };



    TFPrimaryHeader GetPrimaryHeader(uint8_t VCID);
    TFInsertZone GetInsertZone();
    BitBuffer<MAX_SECURITY_HEADER_LENGTH> GetSecurityHeader();
    BitBuffer<MAX_SECURITY_TRAILER_LENGTH> GetSecurityTrailer();
    TFDataField GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data, uint32_t GVCID);
    OperationalControlField GetOperationalControlField(USLPContext context);
    FrameErrorControlField GetFrameErrorControlField(TransferFrame& tf);
    TransferFrame DataToTransferFrame(
        uint8_t VCID, 
        BitBuffer<MAX_DATA_FIELD_LENGTH> message,
        USLPContext context); // Converts higher level input data into a Transfer Frame ready for transmission
    uint16_t TFDataSize(uint8_t vcid); // Determines how many bytes of the message should be sent in the next transfer frame
    BitBuffer<MAX_DATA_FIELD_LENGTH> GetTFDataZone(
        uint16_t &messagePtr, 
        BitBuffer<MAX_MESSAGE_LENGTH> message, 
        USLPContext context); // Converts part of message into the data that will be wrapped in a transfer frame
    BitBuffer<MAX_DATA_SIZE> DataToStream(MessageType type, BitBuffer<MAX_MESSAGE_LENGTH> message); // Converts higher level input data into a stream of bytes for the physical layer


    void VCPRequest(
        BitBuffer<MAX_MESSAGE_LENGTH> packet, 
        uint32_t GVCID,
        uint8_t PVN,
        uint32_t SDU_ID,
        ServiceType serviceType);
    bool IsValidPVN(uint8_t PVN);
    TFDataField VCPacketProcessing(
        BitBuffer<MAX_DATA_ZONE_LENGTH>& data,
        uint8_t VCID, 
        uint16_t fhp,
        uint8_t UPID);
    TransferFrame VCGeneration(TFDataField& tfdf, uint8_t VCID);
    void VCMultiplexer();
    void VCPacketThread(); // Processes incoming packet streams and creates transfer frames
    void PrepareTransferFrame(
        BitBuffer<MAX_DATA_ZONE_LENGTH>& data,
        uint8_t VCID,
        uint16_t fhp,
        uint8_t UPID);
    void AllFramesGenerationFunction(TransferFrame& tf);

    //int8_t GetChannelByVCID(uint8_t vcid); // Returns the channel index of the VCID, -1 if invalid
    uint64_t GetFinishedTransferFramesIndex() {
        return m_finishedTransferFramesIdx;
    };
    TFAllFormats GetFinishedFrame(int i) {
        if (i < 0 || i >= m_finishedTransferFramesIdx) {
            std::cerr << "[ERROR] USLP::GetFinishedFrame - Index (" << i 
                    << ") is out of bounds. Most recent valid index is " 
                    << (m_finishedTransferFramesIdx - 1) << "\n";
            
            // 2. Return a safe, empty/default object instead of reading random memory
            return TFAllFormats{}; 
        }
        
        // 3. Safe, verified in-bounds read
        return m_finishedTransferFrames[i];
    };
private:
    void InitNetworkSocket();
    void CleanupNetworkSocket();
    void SendToGNURadio(const BitBuffer<MAX_TRANSFER_FRAME_LENGTH>& serializedBytes);

    int m_socketFd = -1;
    sockaddr_in m_gnuRadioAddr{};

    std::array<int8_t, MAX_VC_COUNT> m_vcidToIndex{5};
    USLPPacker packer;
    USLPConfig managedParams;
    std::array<VirtualChannel, NUM_ACTIVE_CHANNELS> m_virtualChannels{};
    ThreadSafeMultiplexerQueue<TransferFrame> m_frameMultiplexerQueue;
    std::array<TFAllFormats, maxFinishedTransferFrames> m_finishedTransferFrames;
    uint64_t m_finishedTransferFramesIdx = 0;

    bool m_running = true;
    std::mutex m_multiplexerMtx;

    // Thread handles
    std::thread m_packetThread;
    std::thread m_multiplexerThread;
};