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
        std::lock_guard<std::mutex> lock(m_bufferMtx);
        
        m_lastPacketTime = std::chrono::steady_clock::now();
        m_accumulationBuffer.insert(&packetBytes.data[0], packetBytes.length);
    }
};

class VirtualChannel {
private:
    void initializeMask(uint8_t vcFrameCountLength) {
        // Clamp length between 1 and 7 per USLP spec safety
        vcFrameCountLength = std::clamp<uint8_t>(vcFrameCountLength, 1, 7);
        
        // Calculate the maximum bitmask for the specified byte width
        // e.g., 1 byte -> 0xFF, 2 bytes -> 0xFFFF, 4 bytes -> 0xFFFFFFFF
        if (vcFrameCountLength == 8) {
            vcFrameCountMask = ~0ULL; // Full 64-bit mask
        } else {
            vcFrameCountMask = (1ULL << (vcFrameCountLength * 8)) - 1;
        }
    }
public:
    VirtualChannel(uint8_t vcFrameCountLength = VC_FRAME_COUNT_LENGTH, 
        size_t fixedTfdfSize = MAX_DATA_FIELD_LENGTH)
        : accumulator(fixedTfdfSize)
    {
        initializeMask(vcFrameCountLength);
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

class USLP {
public:
    USLP(USLPConfig& managedParams) : packer(managedParams) {
    };

    TFPrimaryHeader GetPrimaryHeader(uint8_t VCID);
    TFInsertZone GetInsertZone();
    BitBuffer<MAX_SECURITY_HEADER_LENGTH> GetSecurityHeader();
    BitBuffer<MAX_SECURITY_TRAILER_LENGTH> GetSecurityTrailer();
    TFDataField GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data, uint32_t GVCID);
    OperationalControlField GetOperationalControlField(USLPContext context);
    FrameErrorControlField GetFrameErrorControlField();
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
        const uint8_t& VCID, 
        const uint16_t& fhp);
    TransferFrame VCGeneration(TFDataField& tfdf, const uint8_t& VCID);
    void VCPacketThread(); // Processes incoming packet streams and creates transfer frames
    void PrepareTransferFrame(
        BitBuffer<MAX_DATA_ZONE_LENGTH>& data,
        const uint8_t& VCID,
        const uint16_t& fhp);
private:
    USLPPacker packer;
    USLPConfig managedParams;
    std::array<VirtualChannel, VC_COUNT> virtualChannels;
    ThreadSafeQueue<TransferFrame> m_frameMultiplexerQueue;

    bool m_running = true;
};