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
#define MAX_MESSAGE_LENGTH					65535 // Maximum size of a message a user can send (pre-serialization)
#define MAX_TF_PER_MESSAGE					65 // Most transfer frames we will ever use for one message

// Transfer Frame Data Lengths in bytes
#define MAX_SECURITY_HEADER_LENGTH 			20
#define MAX_SECURITY_TRAILER_LENGTH 		20
#define MAX_DATA_FIELD_LENGTH				1014 // MAX_TRANSFER_FRAME_LENGTH - mandatory bytes = 1014
#define MAX_DATA_ZONE_LENGTH				1014 // MAX_TRANSFER_FRAME_LENGTH - mandatory bytes = 1014
#define MAX_INSERT_ZONE_LENGTH				1024 // Apparently specified to worst case take maximum TF size
#define OCF_DATA_LENGTH 					4
#define FECF_DATA_LENGTH 					4
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
	uint32_t VCFrameCountField;
};

//Complete black box
struct TFInsertZone {
	BitBuffer<MAX_INSERT_ZONE_LENGTH> TFIZData;
};

struct TFDFHeader {
	uint16_t TFDZConstructionRules;
	uint16_t USLPProtocolIdentifier;
	uint16_t FirstHeaderLastValidOctetPointer; //Optional
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
	TFInsertZone TFIZ; //tf is this
	TFDataField TFDF;
	OperationalControlField OCF;
	FrameErrorControlField FECF;
};

enum MessageType {
	COMMAND = 1, 
	BITMAP = 2
};