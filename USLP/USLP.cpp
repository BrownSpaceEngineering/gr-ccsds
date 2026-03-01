#include <stdint.h> 
#include <iostream>
#include <bitset>
#include <vector>
#include <array>
#include <cstdint>
#include "CRCpp/inc/CRC.h"

// Transfer Frame Data Lengths in bytes
#define MAX_SECURITY_HEADER_LENGTH 			20
#define MAX_SECURITY_TRAILER_LENGTH 		20
#define MAX_DATA_FIELD_LENGTH				1014 // Max Transfer Frame size - mandatory bytes
#define MAX_DATA_ZONE_LENGTH				1014 // Max Transfer Frame size - mandatory bytes
#define MAX_INSERT_ZONE_LENGTH				1024 // Apparently specified to worst case take maximum TF size
#define OCF_DATA_LENGTH 					4
#define FECF_DATA_LENGTH 					4
#define MAX_TRANSFER_FRAME_LENGTH 			1024
#define PRIMARY_HEADER_LENGTH				8
#define DATA_FIELD_HEADER_LENGTH			3

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
#define TFDZ_CONSTRUCTION_RULES_POS			29  // 3 bits
#define USLP_PROTOCOL_ID_POS				24  // 5 bits
#define FIRST_HEADER_LAST_VALID_OCTET_POS	8  // 16 bits

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

void appendVector(std::vector<uint8_t>& dest, const std::vector<uint8_t>& src) {
    dest.insert(dest.end(), src.begin(), src.end());
}

template <size_t Capacity> BitBuffer<Capacity> packInteger(uint64_t value, size_t numBytes) {
    BitBuffer<Capacity> buffer;
    buffer.length = numBytes;

    for (size_t i = 0; i < numBytes; i++) {
        buffer.data[numBytes - 1 - i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }

    return buffer;
}

void printBytes(uint64_t value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (int i = sizeof(uint64_t) - 1; i >= 0; --i) {
        std::cout << static_cast<int>(bytes[i]) << " ";
    }
    std::cout << "\n";
}

BitBuffer<PRIMARY_HEADER_LENGTH> packPrimaryHeader(TFPrimaryHeader tfph) {
    uint64_t packedHeader = 0;

    packedHeader |= ((uint64_t)(tfph.TFVN))                   		<< TFVN_POS;
	packedHeader |= ((uint64_t)(tfph.SCID))                   		<< SCID_POS;
    packedHeader |= ((uint64_t)(tfph.sourceOrDestinationID))  		<< SRC_DST_ID_POS;
    packedHeader |= ((uint64_t)(tfph.VCID))                   		<< VCID_POS;
    packedHeader |= ((uint64_t)(tfph.MAPID))                  		<< MAPID_POS;
    packedHeader |= ((uint64_t)(tfph.endTFPrimaryHeaderFlag)) 		<< END_TF_PRIMARY_HEADER_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.TFLength))                		<< TF_LENGTH_POS;
    packedHeader |= ((uint64_t)(tfph.bypassSequenceControlFlag)) 	<< BYPASS_SEQ_CTRL_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.protocolCommandControlFlag)) 	<< PROTOCOL_CMD_CTRL_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.spare))                  		<< SPARE_POS;
    packedHeader |= ((uint64_t)(tfph.operationalControlFieldFlag)) 	<< OCF_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.VCFrameCountLength))    		<< VC_FRAME_COUNT_LENGTH_POS;
	packedHeader |= ((uint64_t)(tfph.VCFrameCountField))    		<< VC_FRAME_COUNT_POS;
	printBytes(packedHeader);
	int numBytes = (tfph.endTFPrimaryHeaderFlag == 1) ? 4 : 8;

    return packInteger<PRIMARY_HEADER_LENGTH>(packedHeader, numBytes);
}

BitBuffer<MAX_INSERT_ZONE_LENGTH> packInsertZone(TFInsertZone tfiz) {
    return tfiz.TFIZData;
};

BitBuffer<DATA_FIELD_HEADER_LENGTH> packDataFieldHeader(TFDFHeader tfdfh) {
    uint32_t packed = 0;

    packed |= ((uint32_t)(tfdfh.TFDZConstructionRules))             << TFDZ_CONSTRUCTION_RULES_POS;
    packed |= ((uint32_t)(tfdfh.USLPProtocolIdentifier))            << USLP_PROTOCOL_ID_POS;
    packed |= ((uint32_t)(tfdfh.FirstHeaderLastValidOctetPointer))  << FIRST_HEADER_LAST_VALID_OCTET_POS;

	int numBytes = DATA_FIELD_HEADER_LENGTH;

    return packInteger<DATA_FIELD_HEADER_LENGTH>(packed, numBytes);
}

template <size_t M, size_t N> void append(const BitBuffer<M>& src, BitBuffer<N>& dest, size_t& offset) {
	std::copy(src.data.begin(),
			  src.data.begin() + src.length,
			  dest.data.begin() + offset);
	offset += src.length;
};

BitBuffer<MAX_DATA_FIELD_LENGTH> packDataField(TFDataField tfdf) {
	BitBuffer<MAX_DATA_FIELD_LENGTH> packed;
    BitBuffer<3> packedHeader = packDataFieldHeader(tfdf.header);
    size_t offset = 0;

    append(tfdf.securityHeader, packed, offset);
    append(packedHeader, packed, offset);
    append(tfdf.TFDZ, packed, offset);
    append(tfdf.securityTrailer, packed, offset);

    packed.length = offset;

    return packed;
}

BitBuffer<OCF_DATA_LENGTH> packOperationalControlField(OperationalControlField ocf) {
	uint32_t packed = 0;

    packed |= ((uint32_t)(ocf.SDUType)) << 29;
    packed |= ((uint32_t)(ocf.OCFData));
	int numBytes = OCF_DATA_LENGTH;

    return packInteger<OCF_DATA_LENGTH>(packed, numBytes);
}

BitBuffer<FECF_DATA_LENGTH> packFrameErrorControlField(FrameErrorControlField fecf) {
    return fecf.FECFData;
}

BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packTransferFrame(TransferFrame tf) {
	BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packed;
	BitBuffer<PRIMARY_HEADER_LENGTH> packedPrimaryHeader = packPrimaryHeader(tf.TFPH);
	BitBuffer<MAX_INSERT_ZONE_LENGTH> packedInsertZone = packInsertZone(tf.TFIZ);
	BitBuffer<MAX_DATA_FIELD_LENGTH> packedDataField = packDataField(tf.TFDF);
	BitBuffer<OCF_DATA_LENGTH> packedOperationalControlField = packOperationalControlField(tf.OCF);
	BitBuffer<FECF_DATA_LENGTH> packedFrameErrorControlField = packFrameErrorControlField(tf.FECF);

	size_t offset = 0;

	append(packedPrimaryHeader, packed, offset);
	append(packedInsertZone, packed, offset);
	append(packedDataField, packed, offset);
	append(packedOperationalControlField, packed, offset);
	append(packedFrameErrorControlField, packed, offset);

	packed.length = offset;

	return packed;
}

enum MessageType {
	COMMAND = 1, 
	BITMAP = 2
};

std::array<uint8_t, 4> CRCGenerator() {
	//To be implemented later
	std::array<uint8_t, 4> CRC{};

	return CRC;
}

TFPrimaryHeader GetPrimaryHeader(int VCID) {
	TFPrimaryHeader tfph;
	tfph.protocolCommandControlFlag = false; //Until we work on COP
	tfph.TFVN = 4; // Just carries the current version
	tfph.sourceOrDestinationID = 1; // 0 is more important for multi-recipient systems
	tfph.MAPID = 0; // We do not need MAP, not so many pieces of data to transfer
	tfph.endTFPrimaryHeaderFlag;
	tfph.bypassSequenceControlFlag;
	tfph.protocolCommandControlFlag;
	tfph.operationalControlFieldFlag; //Seems like a good safeguard to have
	tfph.VCFrameCountLength;
	tfph.VCFrameCountField;

	//tfph.TFLength = 0; // To be decided later (measured in octets)
	//tfph.spare = 0b00; //Decide what to do with this later
	//tfph.SCID = 0; // Constant we decide on when the mission launches

	return tfph;
}

TFInsertZone GetInsertZone() {
	TFInsertZone insertZone;
	
	return insertZone;
}

TFDataField GetDataField(std::vector<uint8_t> data) {
	TFDataField tfdf;

	return tfdf;
}

OperationalControlField GetOperationalControlField() {
	OperationalControlField ocf;

	return ocf;
}

FrameErrorControlField GetFrameErrorControlField() {
	FrameErrorControlField fecf {CRCGenerator()};

	return fecf;
}

// Converts higher level input data into a Transfer Frame ready for transmission
TransferFrame DataToTransferFrame(MessageType type, std::vector<uint8_t> data) {
	int VCID = type; //either 1 or 2 depending on command or bitmap

	TFPrimaryHeader tfph = GetPrimaryHeader(VCID);
	TFInsertZone tfiz = GetInsertZone();
	TFDataField tfdf = GetDataField(data);
	OperationalControlField ocf = GetOperationalControlField();
	FrameErrorControlField fecf = GetFrameErrorControlField();

	// Assemble the Transfer Frame
	TransferFrame tf;
	tf.TFPH = tfph;
	tf.TFIZ = tfiz;
	tf.TFDF = tfdf;
	tf.OCF = ocf;
	tf.FECF = fecf;

	return tf;
}

// Converts higher level input data into a stream of bytes for the physical layer
template <size_t M, size_t N> std::array<uint8_t, N> DataToStream(MessageType type, std::array<uint8_t, M> data) {
	TransferFrame tf = DataToTransferFrame(type, data);
	BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packedFrame = packTransferFrame(tf);
	//std::array<uint8_t, packedFrame.length> serializedBytes;
	//std::copy(packedFrame.data.begin(), packedFrame.data.begin() + packedFrame.length, serializedBytes);

	return packedFrame;
}

int main(int argc, char* argv[]) {
	// Test packing a Transfer Frame Primary Header
	TFPrimaryHeader tfph;
	tfph.TFVN = 4;
	tfph.SCID = 2;
	tfph.sourceOrDestinationID = 1;
	tfph.VCID = 2;
	tfph.MAPID = 0;
	tfph.endTFPrimaryHeaderFlag = true;
	tfph.TFLength = 512;
	tfph.bypassSequenceControlFlag = false;
	tfph.protocolCommandControlFlag = false;
	tfph.spare = 0b00;
	tfph.operationalControlFieldFlag = true;
	tfph.VCFrameCountLength = 2;
	tfph.VCFrameCountField = 5;

	BitBuffer<PRIMARY_HEADER_LENGTH> packedHeader = packPrimaryHeader(tfph);
	std::bitset<8> b2{packedHeader.data[7]};
	std::cout << "First byte: " << b2 << std::endl;
	//std::cout << "Packed Header: " << std::bitset<64>(packedHeader) << std::endl;

	return 0;
}
