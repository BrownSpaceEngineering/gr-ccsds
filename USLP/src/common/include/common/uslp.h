#pragma once

#include <stdint.h> 
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
#include <set>
#include <cstdint>
#include <chrono>
#include <mutex>

bool m_enableLogs = true;

template <typename... Args>
void log(Args&&... args) {
    if (!m_enableLogs) return;

    // C++17 Fold-expression: unpacks and prints every parameter sequentially
    (std::cout << ... << std::forward<Args>(args)) << "\n";
}

class VirtualChannelAccumulator {
private:
    uint8_t m_vcid;
    
    std::chrono::steady_clock::time_point m_lastPacketTime; // Last time a packet was accumulated
    std::mutex m_bufferMtx;

    // Tracker for the next frame's First Header Pointer (FHP)
    uint16_t m_nextFhp = 0; 

public:
    AccumulationBuffer m_accumulationBuffer;
    size_t m_fixedTfdfSize; // e.g., 1024 bytes
    std::chrono::steady_clock::time_point m_lastFrameTime; // Last time a frame was transmitted

    VirtualChannelAccumulator(size_t tfdfSize = MAX_DATA_FIELD_LENGTH) 
        : m_fixedTfdfSize(tfdfSize) {
        m_lastPacketTime = std::chrono::steady_clock::now();
        m_lastFrameTime = std::chrono::steady_clock::now();
    }

    // Handled by VCP.request
    void processIncomingPacket(const BitBuffer<MAX_MESSAGE_LENGTH>& packetBytes, uint32_t GVCID) {
        std::cout << "VCPRequest\n";
        std::lock_guard<std::mutex> lock(m_bufferMtx);
        
        m_lastPacketTime = std::chrono::steady_clock::now();
        m_accumulationBuffer.insert(&packetBytes.data[0], packetBytes.length);
    }
};

class VirtualChannel {
public:
    VirtualChannel(uint8_t vcFrameCountLength = VC_FRAME_COUNT_LENGTH, 
        size_t fixedTfdfSize = MAX_DATA_FIELD_LENGTH)
        : accumulator(fixedTfdfSize)
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
    std::chrono::milliseconds expirationTime{50}; // time between transfer frames
    
    uint64_t vcFrameCountMask = 0xFFFFFFFF;
    uint64_t vcFrameCount = 0;
};

static constexpr int maxFinishedTransferFrames = 1024;

struct TFAllFormats {
    TransferFrame tf;
    BitBuffer<maxFinishedTransferFrames> serializedBytes;
};

class USLP {
public:
    USLP(USLPConfig& managedParams) 
        : packer(managedParams), 
          managedParams(managedParams),
          m_running(true) 
    {
        for (size_t vc = 0; vc < VC_COUNT; ++vc) {
            // Read parameters calculated above
            uint8_t byteWidth = managedParams.virtualChannelConfigs[vc].expeditedCountLength;
            
            virtualChannels[vc].initializeMask(byteWidth);
            virtualChannels[vc].expirationTime = std::chrono::milliseconds(managedParams.virtualChannelConfigs[vc].TFDFCompletionTimeoutMs);
        }

        virtualChannels[IDLE_VCID].initializeMask(6);
        virtualChannels[IDLE_VCID].expirationTime = std::chrono::milliseconds(50);

        // Spawning threads at the VERY END of constructor to ensure all members 
        // (packer, queues, virtualChannels) are fully initialized in memory first.
        m_packetThread = std::thread(&USLP::VCPacketThread, this);
        m_multiplexerThread = std::thread(&USLP::VCMultiplexer, this);
    }

    ~USLP() {
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
    }

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
    USLPPacker packer;
    USLPConfig managedParams;
    std::array<VirtualChannel, MAX_VC_COUNT> virtualChannels{};
    ThreadSafeMultiplexerQueue<TransferFrame> m_frameMultiplexerQueue;
    std::array<TFAllFormats, maxFinishedTransferFrames> m_finishedTransferFrames;
    uint64_t m_finishedTransferFramesIdx = 0;

    bool m_running = true;

    // Thread handles
    std::thread m_packetThread;
    std::thread m_multiplexerThread;
};