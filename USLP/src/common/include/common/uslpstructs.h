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
        uint16_t transferFrameLength = 1024;       // USLP-115
        uint8_t TFVN = 0b1100;                     // USLP-116
        uint8_t MCMultiplexingScheme;              // USLP-117
        bool insertZonePresent = false;            // USLP-118
        uint16_t insertZoneLength = 0;             // USLP-119 (length 1-65514 (set to 0 since not using insert service))
        bool FECFPresent = true;                   // USLP-120
        uint32_t maxTFPerDataUnit;                 // USLP-122 (Maximum Number of Transfer Frames Given to the Coding and Synchronization Sublayer as a single data unit)
        uint32_t maxRepetitions;                   // USLP-123 (Maximum Value of the Repetitions Parameter to the Coding and Synchronization Sublayer)
    
        bool isCRC32 = true;                       // Custom param determining length of FECF (2 bytes for CRC-16, 4 bytes for CRC-32)
    };

    struct MasterChannel {
        FrameType frameType;                       // USLP-124 (Can either be fixed or variable in length)
        uint16_t SCID = 0;                         // USLP-125
        std::vector<uint8_t> VCIDs;                // USLP-126 (0–62 of them)
        uint8_t VCMultiplexingScheme;              // USLP-127
    };

    struct VirtualChannelConfig {
        FrameType frameType = FrameType::FIXED;    // USLP-128 (Can either be fixed or variable in length)
        uint8_t VCID;                              // USLP-129

        uint8_t seqControlCountLength;             // USLP-130 (<= 56 bits) (VC Count Length for Sequence Control QoS)
        uint8_t expeditedCountLength = 4;          // USLP-131 (<= 56 bits) (VC Count Length for Expedited QoS)

        COPType COPInEffect = COPType::NONE;       // USLP-132
        uint8_t CLCWVersion = 1;                   // USLP-133
        uint32_t CLCWReportingRate;                // USLP-134

        std::vector<uint8_t> MAPIDs;               // USLP-135 (0–15 of them)
        uint8_t MAPMultiplexingScheme;             // USLP-136 (optional)

        uint16_t truncatedFrameLength;             // USLP-137 (I do not understand when the frame would be truncated)
        SDUType SDUType = SDUType::CCSDS_PACKET;   // USLP-138 (Data field content, which can carry our desired messages or other services)

        bool OCFAllowed;                           // USLP-139 (variable-length TF)
        bool OCFRequired;                          // USLP-140 (fixed-length TF)

        uint32_t seqControlledRepetitions;         // USLP-141 (No idea about this)
        uint32_t copControlRepetitions;            // USLP-142 (No idea about this)

        uint32_t TFDFCompletionTimeoutMs;          // USLP-143 (Max milliseconds from start to releasing)
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

    PhysicalChannel physical;
    MasterChannel master;
    std::array<VirtualChannelConfig, NUM_ACTIVE_CHANNELS> virtualChannelConfigs;
    std::vector<MAPChannel> mapChannels;
    PacketTransfer packet;
};

// Struct used to store the context of a specific packet being processed
struct USLPContext {
    uint8_t currentVCID;
};
