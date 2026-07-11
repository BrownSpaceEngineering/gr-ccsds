#pragma once

#include <common/data.h>
#include <common/packing.h>
#include <common/utils.h>
#include <common/uslpstructs.h>
#include <common/uslp.h>

// Structure to log exactly when a packet entered the USLP stack via VCPRequest
struct PacketInjectionRecord {
    std::chrono::steady_clock::time_point timestamp;
    uint8_t vcid;
    uint32_t sduId;
    size_t lengthBytes;
};

void RunVCPRequestMultiplexingTest(USLP& uslpStack);
void RunUslpTemporalStandardsTest();
BitBuffer<MAX_MESSAGE_LENGTH> CreateDummyPacket(uint8_t fillByte, size_t numBytes);