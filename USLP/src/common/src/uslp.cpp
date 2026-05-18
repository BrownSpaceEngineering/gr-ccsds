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
#include <common/uslp.h>
#include <common/packing.h>
#include "CRC.h"

TFPrimaryHeader USLP::GetPrimaryHeader(uint8_t VCID) {
	TFPrimaryHeader tfph;
	tfph.protocolCommandControlFlag = false; //Until we work on COP
	tfph.TFVN = managedParams.physical.TFVN; // Just carries the current version
	tfph.sourceOrDestinationID = 1; // 0 is more important for multi-recipient systems
	tfph.MAPID = 0; // We do not need MAP, not so many pieces of data to transfer
	tfph.endTFPrimaryHeaderFlag; // Decided possibly if we need super fast transfer of packet
	tfph.bypassSequenceControlFlag;
	tfph.protocolCommandControlFlag;
	tfph.operationalControlFieldFlag; //Seems like a good safeguard to have
	tfph.VCFrameCountLength;
	tfph.VCFrameCountField;

	//tfph.TFLength = 0; // To be decided later (measured in octets)
	//tfph.spare = 0b00; //Decide what to do with this later
	//tfph.SCID = 0; // Constant we decide on when the mission launches

	// Test packing a Transfer Frame Primary Header (TO BE REMOVED)
	tfph.TFVN = 4;
	tfph.SCID = 2;
	tfph.sourceOrDestinationID = 1;
	tfph.VCID = 2;
	tfph.MAPID = 0;
	tfph.endTFPrimaryHeaderFlag = false;
	tfph.TFLength = 512;
	tfph.bypassSequenceControlFlag = false;
	tfph.protocolCommandControlFlag = false;
	tfph.spare = 0b00;
	tfph.operationalControlFieldFlag = false;
	tfph.VCFrameCountLength = 2;
	tfph.VCFrameCountField = 5;

	return tfph;
}

TFInsertZone USLP::GetInsertZone() {
	TFInsertZone insertZone {};

	if (managedParams.physical.insertZonePresent) {
		BitBuffer<MAX_INSERT_ZONE_LENGTH> data;
		data.data = {2, 3};
		data.length = managedParams.physical.insertZoneLength;
		insertZone.TFIZData = data;
	}
	
	return insertZone;
}

// Current assumption is no SDLS (will edit later)
BitBuffer<MAX_SECURITY_HEADER_LENGTH> USLP::GetSecurityHeader() {
	std::array<uint8_t, MAX_SECURITY_HEADER_LENGTH> data {};
	
	BitBuffer<MAX_SECURITY_HEADER_LENGTH> header;
	header.data = data;
	header.length = 0;

	return header;
};

// Current assumption is no SDLS (will edit later)
BitBuffer<MAX_SECURITY_TRAILER_LENGTH> USLP::GetSecurityTrailer() {
	std::array<uint8_t, MAX_SECURITY_TRAILER_LENGTH> data {};
	
	BitBuffer<MAX_SECURITY_TRAILER_LENGTH> trailer;
	trailer.data = data;
	trailer.length = 0;

	return trailer;
};

TFDataField USLP::GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data) {
	TFDataField tfdf;
	//tfdf.securityHeader = GetSecurityHeader();

	// Placeholder values
	TFDFHeader header {6, 0, 2};
	tfdf.header = header;
	tfdf.TFDZ = data;
	//tfdf.securityTrailer = GetSecurityTrailer();
	//std::cout << "security trailer length on init: " << tfdf.securityTrailer.length << std::endl;

	return tfdf;
}

OperationalControlField USLP::GetOperationalControlField(USLPContext context) {
	OperationalControlField ocf {};

	if (managedParams.virtualChannels[context.currentVCID].COPInEffect != USLPConfig::COPType::NONE) {
		// Default fields to be filled in logically later
		ocf.SDUType = 0;
		ocf.OCFData = 0;
	}

	return ocf;
}

FrameErrorControlField USLP::GetFrameErrorControlField() {
	FrameErrorControlField fecf {};

	if (managedParams.physical.FECFPresent) {
		BitBuffer<FECF_DATA_LENGTH> FECFData;
		FECFData.data = CRCGenerator();
		FECFData.length = FECF_DATA_LENGTH;
		fecf.FECFData = FECFData;
	}

	return fecf;
}

// Converts higher level input data into a Transfer Frame ready for transmission
TransferFrame USLP::DataToTransferFrame(uint8_t VCID, BitBuffer<MAX_DATA_FIELD_LENGTH> message, USLPContext context) {
	TFPrimaryHeader tfph = GetPrimaryHeader(VCID);
	TFInsertZone tfiz = GetInsertZone();
	TFDataField tfdf = GetDataField(message);
	OperationalControlField ocf = GetOperationalControlField(context);
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

// (To be implemented) Determines how many bytes of the message should be sent in the next transfer frame
uint16_t USLP::TFDataSize(uint8_t vcid) {
	uint16_t num_bytes = MAX_TRANSFER_FRAME_LENGTH - PRIMARY_HEADER_LENGTH;
	std::cout << "1" << std::endl;
	if (managedParams.physical.FECFPresent) {
		if (managedParams.physical.isCRC32) {
			num_bytes -= FECF_DATA_LENGTH;
		} else {
			num_bytes -= CRC16_DATA_LENGTH;
		}
	}
	std::cout << "2" << std::endl;
	if (managedParams.physical.insertZonePresent) {
		num_bytes -= managedParams.physical.insertZoneLength;
	}
	std::cout << "3" << std::endl;
	// This feels like it should be a constant for the physical link; I do not quite understand
	if (managedParams.virtualChannels[vcid].COPInEffect != USLPConfig::COPType::NONE) {
		num_bytes -= OCF_DATA_LENGTH;
	}
	std::cout << "4" << std::endl;
	return num_bytes;
	//return MAX_DATA_FIELD_LENGTH;
}

// Converts part of message into the data that will be wrapped in a transfer frame
BitBuffer<MAX_DATA_FIELD_LENGTH> USLP::GetTFDataZone(uint16_t &messagePtr, BitBuffer<MAX_MESSAGE_LENGTH> message, USLPContext context) {
	BitBuffer<MAX_DATA_FIELD_LENGTH> chunk;
	uint16_t maxTFDataSize = maxDataZoneLengths[context.currentVCID];
	uint16_t numBytesCopy = std::min(maxTFDataSize, static_cast<uint16_t>(message.length - messagePtr));
	chunk.length = numBytesCopy;
	std::memcpy(&chunk.data[0], &message.data[messagePtr], numBytesCopy);
	messagePtr += numBytesCopy;
	
	return chunk;
}

// Converts higher level input data into a stream of bytes for the physical layer
BitBuffer<MAX_DATA_SIZE> USLP::DataToStream(MessageType type, BitBuffer<MAX_MESSAGE_LENGTH> message) {
	uint16_t messagePtr = 0; // with current data size limit of 65535 uint16_t works
	uint32_t serializedDataPtr = 0;
	BitBuffer<MAX_DATA_SIZE> serializedData;

	while (messagePtr < message.length) {
		uint8_t VCID = static_cast<uint8_t>(type);
		USLPContext context {VCID};
		std::cout << "getting data zone" << std::endl;
		BitBuffer<MAX_DATA_FIELD_LENGTH> chunk = GetTFDataZone(messagePtr, message, context);
		std::cout << "getting tf" << std::endl;
		TransferFrame tf = DataToTransferFrame(VCID, chunk, context);
		std::cout << "packing tf" << std::endl;
		BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packedFrame = packer.packTransferFrame(tf, context);
		
		for (int i = 0; i < 8; i++) {
			std::bitset<8> b{packedFrame.data[i]};
			std::cout << b << " ";
		}
		std::cout << "\n";

		assert(serializedDataPtr + packedFrame.length <= MAX_DATA_SIZE);
		std::memcpy(&serializedData.data[serializedDataPtr], &packedFrame.data[0], packedFrame.length);
		std::cout << "ptr " << serializedDataPtr << std::endl;
		std::cout << "ptr ";
		std::cout << "\n";
		serializedDataPtr += packedFrame.length;
	}

	serializedData.length = serializedDataPtr;

	return serializedData;
}

USLP::USLP(uint8_t numVirtualChannels) : packer(managedParams) {
	for (int i = 0; i < numVirtualChannels; i++) {
		maxDataZoneLengths[i] = TFDataSize(i);
	}
}

int main(int argc, char* argv[]) {
	std::array<uint8_t, TEST_ARRAY_SIZE> message = GenerateRandomBytes();
	WriteBytes(message); // Writes raw bytes of message to bytes.txt
	
	//BitBuffer<PRIMARY_HEADER_LENGTH> packedHeader = packPrimaryHeader(tfph);
	//std::bitset<8> b2{packedHeader.data[7]};
	//std::cout << "First byte: " << b2 << std::endl;
	//std::cout << "Packed Header: " << std::bitset<64>(packedHeader) << std::endl;

	enum MessageType type = BITMAP;
	std::array<uint8_t, MAX_MESSAGE_LENGTH> messageContainer {};
	std::memcpy(&messageContainer[0], &message[0], message.size());

	uint8_t numVC = 2;
	USLP uslp(numVC);
	std::cout << "cons" << std::endl;

	BitBuffer<MAX_MESSAGE_LENGTH> messageBuffer {messageContainer, TEST_ARRAY_SIZE};
	BitBuffer<MAX_DATA_SIZE> serializedTransferFrames = uslp.DataToStream(type, messageBuffer);

	std::ofstream out("Transfer Frames.txt", std::ios::trunc);
	int lastTFIndex = 0;

	// Writes transfer frame bytes wrapping message
	for (int i = 0; i < TEST_ARRAY_SIZE; i++) {
		//std::bitset<8> b2{message[i]};
		//std::cout << b2 << std::endl;
		if (i % 1024 == 0) {
			lastTFIndex = i;

			out << "\n";
			out << "\n";
			out << "------ NEW TRANSFER FRAME ------";
			out << "\n";
			out << "\n";
		}

		if ((i - lastTFIndex) % 32 == 0) {
			out << "\n";
		}

		out << std::setw(3) << static_cast<int>(serializedTransferFrames.data[i]) << "  ";
	}

	return 0;
}
