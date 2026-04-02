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
#include <common/utils.h>
#include <string>
#include <set>

struct USLPConfig {
    enum class FrameType {
        FIXED,
        VARIABLE
    };

    enum class COPType {
        COP_1,
        COP_P,
        NONE
    };

    enum class SDUType {
        CCSDS_PACKET,
        VCA_SDU,
        MAPA_SDU,
        OCTET_STREAM
    };

    struct PhysicalChannel {
        std::string physicalChannelName;		   // USLP-113
        FrameType frameType;                       // USLP-114
        uint16_t transferFrameLength;              // USLP-115
        uint8_t TFVN = 0b1100;                     // USLP-116
        uint8_t MCMultiplexingScheme;              // USLP-117
        bool insertZonePresent;                    // USLP-118
        uint16_t insertZoneLength;                 // USLP-119 (length 1-65514)
        bool fecfPresent = true;                   // USLP-120
        uint32_t maxTFPerDataUnit;                 // USLP-122 (Maximum Number of Transfer Frames Given to the Coding and Synchronization Sublayer as a single data unit)
        uint32_t maxRepetitions;                   // USLP-123 (Maximum Value of the Repetitions Parameter to the Coding and Synchronization Sublayer)
    };

    struct MasterChannel {
        FrameType frameType;                       // USLP-124 (Can either be fixed or variable in length)
        uint16_t SCID;                             // USLP-125
        std::vector<uint8_t> VCIDs;                // USLP-126 (0–62 of them)
        uint8_t VCMultiplexingScheme;              // USLP-127 (I do not understand this)
    };

    struct VirtualChannel {
        FrameType frameType;                       // USLP-128 (Can either be fixed or variable in length)
        uint8_t VCID;                              // USLP-129

        uint8_t seqControlCountLength;             // USLP-130 (<= 56 bits) (VC Count Length for Sequence Control QoS)
        uint8_t expeditedCountLength;              // USLP-131 (<= 56 bits) (VC Count Length for Expedited QoS)

        COPType COPInEffect;                       // USLP-132
        uint8_t CLCWVersion = 1;                   // USLP-133
        uint32_t CLCWReportingRate;                // USLP-134

        std::vector<uint8_t> MAPIDs;               // USLP-135 (0–15 of them)
        uint8_t MAPMultiplexingScheme;             // USLP-136 (optional)

        uint16_t truncatedFrameLength;             // USLP-137 (I do not understand what is being truncated)
        SDUType SDUType;                           // USLP-138 (Data field content, which can carry our desired messages or other services)

        bool ocfAllowed;                           // USLP-139 (variable-length TF)
        bool ocfRequired;                          // USLP-140 (fixed-length TF)

        uint32_t seqControlledRepetitions;         // USLP-141 (No idea about this)
        uint32_t copControlRepetitions;            // USLP-142 (No idea about this)

        uint32_t tfdfCompletionTimeoutMs;          // USLP-143 (Max milliseconds from start to releasing)
        uint32_t interFrameDelayMs;                // USLP-144 (Max milliseconds between releases of successive frames)
    };

    struct MAPChannel {
        uint8_t MAPID;                             // USLP-145
        SDUType SDUType;                           // USLP-146
        uint32_t UPID;                             // USLP-147
    };

    struct PacketTransfer {
        std::set<uint8_t> validPVNs;               // USLP-148
        uint16_t maxPacketLength;                  // USLP-149 (in octets)
        bool deliverIncompletePackets;             // USLP-150
    };

    // ---------- Instances ----------
    PhysicalChannel physical;
    MasterChannel master;
    std::vector<VirtualChannel> virtualChannels;
    std::vector<MAPChannel> mapChannels;
    PacketTransfer packet;
};

TFPrimaryHeader GetPrimaryHeader(int VCID);
TFInsertZone GetInsertZone();
TFDataField GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data);
OperationalControlField GetOperationalControlField();
FrameErrorControlField GetFrameErrorControlField();
// Converts higher level input data into a Transfer Frame ready for transmission
TransferFrame DataToTransferFrame(MessageType type, BitBuffer<MAX_DATA_FIELD_LENGTH> message);
// (To be implemented) Determines how many bytes of the message should be sent in the next transfer frame
uint16_t TFDataSize();
// Converts part of message into the data that will be wrapped in a transfer frame
BitBuffer<MAX_DATA_FIELD_LENGTH> GetTFDataZone(uint16_t &messagePtr, BitBuffer<MAX_MESSAGE_LENGTH> message);
// Converts higher level input data into a stream of bytes for the physical layer
BitBuffer<MAX_DATA_SIZE> DataToStream(MessageType type, BitBuffer<MAX_MESSAGE_LENGTH> message);