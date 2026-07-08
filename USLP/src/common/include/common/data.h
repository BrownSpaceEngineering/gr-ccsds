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

// Upper Level Macros
#define MAX_TF_PER_MESSAGE						65 				// Most transfer frames we will ever use for one message
constexpr uint16_t MAX_MESSAGE_LENGTH = 		UINT16_MAX; 	// Maximum size of a message a user can send (pre-serialization)
constexpr uint8_t  VC_BITMASK = 				(1 << 6) - 1;
constexpr uint32_t MAX_ACCUMULATOR_LENGTH = 	UINT16_MAX; 	// Maximum size of the transfer frame creation queue
constexpr uint16_t MAX_INCOMING_PACKETS =   	UINT16_MAX; 	// Max upper layer packets that can wait to be wrapped
constexpr uint8_t  VC_COUNT = 					3; 				// Total number of VCs (only considering uplink 1. bitmaps 2. commands)
constexpr uint8_t  NUM_ACTIVE_CHANNELS = 		VC_COUNT + 1;	// VCs and the OID channel
constexpr uint8_t  IDLE_VCID = 					63; 			// CCSDS standard
constexpr uint16_t MAX_VC_COUNT = 				64; 			// 6 bit field in transfer frame so 2^6
constexpr uint8_t  DEFAULT_UPID = 				0b00000; 		// Indicates CFDP packets
constexpr uint8_t  IDLE_UPID = 					0b11111; 		// Indicates Idle data
constexpr uint8_t  IDLE_PATTERN = 				0x55;			// For OID frames
constexpr uint8_t  DEFAULT_CONSTRUCTION_RULE = 	0; 				// For TFDF
constexpr uint8_t  VC_FRAME_COUNT_LENGTH = 		4; 				// 4 bytes of sequence number per frame
constexpr uint16_t DEFAULT_FHP =				2047;			// Default First Header Pointer

// Transfer Frame Data Lengths in bytes
#define ZERO								0
#define MAX_SECURITY_HEADER_LENGTH 			20
#define MAX_SECURITY_TRAILER_LENGTH 		20
constexpr uint16_t MAX_DATA_FIELD_LENGTH = 1014; // MAX_TRANSFER_FRAME_LENGTH - mandatory bytes = 1014
constexpr uint16_t MAX_DATA_ZONE_LENGTH = 1014; // MAX_TRANSFER_FRAME_LENGTH - mandatory bytes = 1014
#define MAX_INSERT_ZONE_LENGTH				1024 // Apparently specified to worst case take maximum TF size
#define OCF_DATA_LENGTH 					4
#define CRC16_DATA_LENGTH 					2
#define CRC32_DATA_LENGTH 					4
#define FECF_DATA_LENGTH 					4 // Default FECF using CRC32
#define MAX_TRANSFER_FRAME_LENGTH 			1024
#define PRIMARY_HEADER_LENGTH				8
#define DATA_FIELD_HEADER_LENGTH			3

constexpr int MAX_DATA_SIZE = MAX_TRANSFER_FRAME_LENGTH * MAX_TF_PER_MESSAGE;

// Transfer Frame Primary Header field bit positions
#define TFVN_POS                        	60   // 4  bits
#define SCID_POS                        	44   // 16 bits
#define SRC_DST_ID_POS                  	43   // 1  bit 
#define VCID_POS                        	37   // 6  bits
#define MAPID_POS                       	33   // 4  bits
#define END_TF_PRIMARY_HEADER_FLAG_POS  	32   // 1  bit 
#define TF_LENGTH_POS                   	16   // 16 bits
#define BYPASS_SEQ_CTRL_FLAG_POS        	15   // 1  bit 
#define PROTOCOL_CMD_CTRL_FLAG_POS      	14   // 1  bit 
#define SPARE_POS                       	12   // 2  bits
#define OCF_FLAG_POS                    	11   // 1  bit 
#define VC_FRAME_COUNT_LENGTH_POS       	 8   // 3  bits 
#define VC_FRAME_COUNT_POS              	 0   // 8  bits
// NOTE THAT WE ARE NOT CURRENTLY SET UP	 TO CONTAIN MORE THAN 255 FRAMES IN A VC

// TF Primary Header structure
#define TFDZ_CONSTRUCTION_RULES_POS			21  // 3 bits
#define USLP_PROTOCOL_ID_POS				16  // 5 bits
#define FIRST_HEADER_LAST_VALID_OCTET_POS	0  // 16 bits

// Fixed-size buffer structure
template <size_t Capacity>

// Replacement of std::vector for fixed-size bit buffers
struct BitBuffer {
	std::array<uint8_t, Capacity> data; // Arbitrary size, can be adjusted as needed
	size_t length = 0; // Actual length of data in bytes

	BitBuffer() = default;
	BitBuffer(const uint8_t* source, size_t count) {
        insert(0, source, count);
    }

	/**
     * @brief Inserts a chunk of bytes at a specific index.
     * * @param index  The position in the buffer where insertion begins.
     * @param source Pointer to the raw byte array to copy from.
     * @param count  The number of bytes to insert.
     * @return true if insertion succeeded, false if it violates bounds or capacity.
     */
    bool insert(size_t index, const uint8_t* source, size_t count) {
        if (index > length || length + count > Capacity || source == nullptr) {
            return false; 
        }
        if (count == 0) return true;

        // Shift existing trailing data to the right to clear space. Unsure if necessary but good to support
        // Must use copy_backward because the source and destination ranges overlap!
        std::copy_backward(data.begin() + index, data.begin() + length, data.begin() + length + count);

        // Copy the new chunk into the newly vacated spot
        std::copy(source, source + count, data.begin() + index);

        length += count;
        return true;
    }

	bool insertAtEnd(const uint8_t* source, size_t count) {
		return insert(length, source, count);
	}

	/**
	 * @brief Fills a segment of the buffer with a repeating byte value.
	 * @param index The starting position.
	 * @param value The byte pattern to fill with (e.g., 0x00 or 0x55).
	 * @param count The number of bytes to fill.
	 */
	bool fill(size_t index, uint8_t value, size_t count) {
		if (index > length || length + count > Capacity) {
			return false;
		}
		if (count == 0) return true;

		// Shift existing trailing elements right if we are inserting into the middle
		std::copy_backward(data.begin() + index, data.begin() + length, data.begin() + length + count);

		// Fill the newly allocated segment with your repeating value
		std::fill(data.begin() + index, data.begin() + index + count, value);

		length += count;
		return true;
	}

	/**
     * @brief Erases a chunk of bytes starting from a specific index.
     * * @param index The position in the buffer where deletion begins.
     * @param count The number of bytes to remove.
     * @return true if deletion succeeded, false if the range is out of bounds.
     */
    bool erase(size_t index, size_t count) {
        // Enforce bounds checking
        if (index >= length || index + count > length) {
            return false; 
        }
        if (count == 0) return true;

        // Shift trailing data to the left to close the gap and overwrite the chunk.
        // Standard std::copy safely moves left when the destination is behind the source.
        std::copy(data.begin() + index + count, data.begin() + length, data.begin() + index);

        length -= count;
        return true;
    }

	bool eraseFromEnd(size_t count) {
		return erase(length - count, count);
	}

	bool eraseFromStart(size_t count) {
		return erase(0, count);
	}

	bool empty() {
		return length == 0;
	}

	void clear() {
        length = 0;
    }
};

// Simple ring buffer for tracking packet header indices
struct PacketPtrBuffer {
	std::array<size_t, MAX_INCOMING_PACKETS> m_packetStartIndices;
	size_t m_queueHead = 0;
	size_t m_queueTail = 0;
	size_t m_queueSize = 0;

	bool push(size_t index) {
		if (m_queueSize >= MAX_INCOMING_PACKETS) return false;
		m_packetStartIndices[m_queueHead] = index;
		m_queueHead = (m_queueHead + 1) % MAX_INCOMING_PACKETS;
		m_queueSize++;

		return true;
	}

	void pop() {
		if (m_queueSize > 0) {
			m_queueSize -= 1;
			m_queueTail = (m_queueTail + 1) % MAX_INCOMING_PACKETS;
		}
	}

	bool isFull() {
		if (m_queueSize <= MAX_INCOMING_PACKETS) {
			return false;
		} else {
			return true;
		}
	}
};

struct AccumulationBuffer {
	BitBuffer<MAX_ACCUMULATOR_LENGTH> payloadBuffer; // Stores raw packet contents
	PacketPtrBuffer packetPointers; // Stores the start of each packet header
	uint16_t numIncomingPackets = 0;

	bool insert(const uint8_t* packet, size_t length) {
		if (length == 0) return true;

		if (packetPointers.isFull()) {
			return false; 
		}

		size_t rawStartLocation = payloadBuffer.length;
		bool status = payloadBuffer.insertAtEnd(packet, length);
	
		if (status) {
			packetPointers.push(rawStartLocation);
		}

		return status;
	}

	bool erase(size_t length) {
		numIncomingPackets--;
		return payloadBuffer.eraseFromStart(length);
	}

	bool empty() {
		if (payloadBuffer.length == 0) {
			return true;
		} else {
			return false;
		}
	}
};


struct TFPrimaryHeader {
	uint16_t TFVN = 4;
	uint16_t SCID;
	bool sourceOrDestinationID;
	uint16_t VCID;
	uint16_t MAPID;
	bool endTFPrimaryHeaderFlag;
	uint16_t TFLength;
	bool bypassSequenceControlFlag;
	bool protocolCommandControlFlag;
	uint16_t spare;
	bool operationalControlFieldFlag;
	uint16_t VCFrameCountLength;
	uint32_t VCFrameCount;
};

//Complete black box
struct TFInsertZone {
	BitBuffer<MAX_INSERT_ZONE_LENGTH> TFIZData;
};

struct TFDFHeader {
	uint8_t TFDZConstructionRules;
	uint8_t USLPProtocolIdentifier;
	uint16_t firstHeaderLastValidOctetPointer;
};

struct TFDataField {
	BitBuffer<MAX_SECURITY_HEADER_LENGTH> securityHeader;
	TFDFHeader header;
	BitBuffer<MAX_DATA_ZONE_LENGTH> TFDZ; //ALL THE DATA YOU WANT TO SEND GOES HERE
	BitBuffer<MAX_SECURITY_TRAILER_LENGTH> securityTrailer;
};

struct OperationalControlField {
	uint8_t SDUType; // 3 bits
	uint32_t OCFData; // 29 bits
};

struct FrameErrorControlField {
	BitBuffer<FECF_DATA_LENGTH> FECFData; //Generated using 32-bit CRC
};

struct TransferFrame {
	TFPrimaryHeader TFPH;
	TFInsertZone TFIZ;
	TFDataField TFDF;
	OperationalControlField OCF;
	FrameErrorControlField FECF;
};

enum MessageType {
	COMMAND = 0, 
	BITMAP = 1
};

enum ServiceType {
	SEQUENCE_CONTROLLED = 0,
	EXPEDITED = 1
};