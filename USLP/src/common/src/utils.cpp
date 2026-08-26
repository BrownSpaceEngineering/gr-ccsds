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
//#include "CRC.h"

int8_t GetChannelByVCID(uint8_t vcid, std::array<int8_t, MAX_VC_COUNT> &mapping) {
	if (vcid >= MAX_VC_COUNT) {
		std::cerr << "Invalid vcid\n";
		return -1; // Out of bounds safety
	}
	
	return mapping[vcid];
};

void printBytes(uint64_t value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (int i = sizeof(uint64_t) - 1; i >= 0; --i) {
        std::cout << static_cast<int>(bytes[i]) << " ";
    }
    std::cout << "\n";
}

std::array<uint8_t, 4> CRCGenerator() {
	//To be implemented later
	std::array<uint8_t, 4> CRC{4, 0, 5, 0};

	return CRC;
}

// Generates random bytes enough to fill 3 transfer frames with maximum capacity
std::array<uint8_t, TEST_ARRAY_SIZE> GenerateRandomBytes() {
	std::array<uint8_t, TEST_ARRAY_SIZE> message;

	/*std::generate(message.begin(), message.end(), [] {
		static std::mt19937 gen(std::random_device{}());
		static std::uniform_int_distribution<int> dist(0,255);
		return dist(gen);
	});*/

	std::mt19937 gen(12345);
	std::uniform_int_distribution<uint8_t> dist(0, 255);
	
	for (auto& b: message) {
		b = static_cast<uint8_t>(dist(gen));
	}

	return message;
}

void ClearFile() {
	std::ofstream file("bytes.txt", std::ios::trunc);
}

// Writes Bytes to .txt assuming maximum transfer frame capacity
void WriteBytes(BitBuffer<MAX_CCS_SL_FRAME_LENGTH> &serializedBytes) {
	std::ofstream out("TestOutput.txt", std::ios::app);
	int lastTFIndex = 0;
	out << "\n";
	out << "\n";
	out << "------ NEW TRANSFER FRAME ------";
	out << "\n";
	out << "\n";

	for (int i = 0; i < serializedBytes.length; i++) {
		if ((i - lastTFIndex) % 32 == 0) {
			out << "\n";
		}

		out << std::setw(3) << static_cast<int>(serializedBytes.data[i]) << "  ";
	}
}

// Helper function to calculate CRC-16 or CRC-32 over the frame bytes
uint32_t ComputeCRC(const uint8_t* data, size_t length, bool isCRC32) {
    if (isCRC32) {
        // Standard CRC-32 (IEEE 802.3) polynomial representation
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                } else {
                    crc >>= 1;
                }
            }
        }
        return ~crc;
    } else {
        // Standard CRC-16-CCITT polynomial representation (0x1021)
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= (static_cast<uint16_t>(data[i]) << 8);
            for (int j = 0; j < 8; ++j) {
                if (crc & 0x8000) {
                    crc = (crc << 1) ^ 0x1021;
                } else {
                    crc <<= 1;
                }
            }
        }
        return crc;
    }
}

void PrintPrimaryHeader(const TFPrimaryHeader& tfph) {
    if (true) return;

    std::cout << "=============================================\n";
    std::cout << "       USLP TRANSFER FRAME PRIMARY HEADER    \n";
    std::cout << "=============================================\n";
    
    // Casting uint8_t and narrow integer fields ensures they print as numbers
    std::cout << "  Transfer Frame Version (TFVN)     : " << static_cast<uint32_t>(tfph.TFVN) << "\n";
    std::cout << "  Spacecraft Identifier (SCID)      : " << tfph.SCID << "\n";
    
    std::cout << "  Source/Destination ID (SDID)      : " 
              << (tfph.sourceOrDestinationID ? "1 (Destination)" : "0 (Source)") << "\n";
              
    std::cout << "  Virtual Channel ID (VCID)         : " << static_cast<uint32_t>(tfph.VCID) << "\n";
    std::cout << "  Multiplexer Access Point (MAPID)  : " << static_cast<uint32_t>(tfph.MAPID) << "\n";
    
    std::cout << "  End of Header Flag                : " 
              << (tfph.endTFPrimaryHeaderFlag ? "1 (Present)" : "0 (Absent)") << "\n";
              
    std::cout << "  Transfer Frame Length             : " << tfph.TFLength << " bytes\n";
    
    std::cout << "  Bypass/Seq Control (QoS Type)     : " 
              << (tfph.bypassSequenceControlFlag ? "1 (Expedited / Type-B)" : "0 (Sequence-Controlled / Type-A)") << "\n";
              
    std::cout << "  Protocol Command/Control Flag     : " 
              << (tfph.protocolCommandControlFlag ? "1 (Protocol Control)" : "0 (User Data)") << "\n";
              
    // Standard bitset helps visualize spare bit fields
    std::cout << "  Spare Bits                        : 0b" << std::bitset<2>(tfph.spare) << "\n";
    
    std::cout << "  Operational Control Field (OCF)   : " 
              << (tfph.operationalControlFieldFlag ? "1 (Required/Present)" : "0 (Absent)") << "\n";
              
    std::cout << "  VC Frame Count Length             : " << static_cast<uint32_t>(tfph.VCFrameCountLength) << " bytes\n";
    std::cout << "  VC Frame Count                    : " << tfph.VCFrameCount << "\n";
    std::cout << "=============================================\n" << std::endl;
}