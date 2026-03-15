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

TFPrimaryHeader GetPrimaryHeader(int VCID) {
	TFPrimaryHeader tfph;
	tfph.protocolCommandControlFlag = false; //Until we work on COP
	tfph.TFVN = 4; // Just carries the current version
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
	tfph.operationalControlFieldFlag = true;
	tfph.VCFrameCountLength = 2;
	tfph.VCFrameCountField = 5;

	return tfph;
}

TFInsertZone GetInsertZone() {
	TFInsertZone insertZone;
	BitBuffer<MAX_INSERT_ZONE_LENGTH> data;
	data.data = {2, 3};
	data.length = 2;
	insertZone.TFIZData = data;
	
	return insertZone;
}

// Current assumption is no SDLS (will edit later)
BitBuffer<MAX_SECURITY_HEADER_LENGTH> GetSecurityHeader() {
	std::array<uint8_t, MAX_SECURITY_HEADER_LENGTH> data {};
	
	BitBuffer<MAX_SECURITY_HEADER_LENGTH> header;
	header.data = data;
	header.length = 0;

	return header;
};

// Current assumption is no SDLS (will edit later)
BitBuffer<MAX_SECURITY_TRAILER_LENGTH> GetSecurityTrailer() {
	std::array<uint8_t, MAX_SECURITY_TRAILER_LENGTH> data {};
	
	BitBuffer<MAX_SECURITY_TRAILER_LENGTH> trailer;
	trailer.data = data;
	trailer.length = 0;

	return trailer;
};

TFDataField GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data) {
	TFDataField tfdf;
	tfdf.securityHeader = GetSecurityHeader();

	// Placeholder values
	TFDFHeader header {6, 0, 2};
	tfdf.header = header;
	tfdf.TFDZ = data;
	tfdf.securityTrailer = GetSecurityTrailer();
	//std::cout << "security trailer length on init: " << tfdf.securityTrailer.length << std::endl;

	return tfdf;
}

OperationalControlField GetOperationalControlField() {
	OperationalControlField ocf;
	ocf.SDUType = 0;
	ocf.OCFData = 0;

	return ocf;
}

FrameErrorControlField GetFrameErrorControlField() {
	BitBuffer<FECF_DATA_LENGTH> FECFData;
	FECFData.data = CRCGenerator();
	FECFData.length = 4;

	FrameErrorControlField fecf {FECFData};

	return fecf;
}

// Converts higher level input data into a Transfer Frame ready for transmission
TransferFrame DataToTransferFrame(MessageType type, BitBuffer<MAX_DATA_FIELD_LENGTH> message) {
	int VCID = type; //either 1 or 2 depending on command or bitmap

	TFPrimaryHeader tfph = GetPrimaryHeader(VCID);
	TFInsertZone tfiz = GetInsertZone();
	TFDataField tfdf = GetDataField(message);
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

// (To be implemented) Determines how many bytes of the message should be sent in the next transfer frame
uint16_t TFDataSize() {
	return 1003;
	//return MAX_DATA_FIELD_LENGTH;
}

// Converts part of message into the data that will be wrapped in a transfer frame
BitBuffer<MAX_DATA_FIELD_LENGTH> GetTFDataZone(uint16_t &messagePtr, BitBuffer<MAX_MESSAGE_LENGTH> message) {
	BitBuffer<MAX_DATA_FIELD_LENGTH> chunk;
	uint16_t maxTFDataSize = TFDataSize();
	uint16_t numBytesCopy = std::min(maxTFDataSize, static_cast<uint16_t>(message.length - messagePtr));
	chunk.length = numBytesCopy;
	std::memcpy(&chunk.data[0], &message.data[messagePtr], numBytesCopy);
	messagePtr += numBytesCopy;
	
	return chunk;
}

// Converts higher level input data into a stream of bytes for the physical layer
BitBuffer<MAX_DATA_SIZE> DataToStream(MessageType type, BitBuffer<MAX_MESSAGE_LENGTH> message) {
	uint16_t messagePtr = 0; // with current data size limit of 65535 uint16_t works
	uint32_t serializedDataPtr = 0;
	BitBuffer<MAX_DATA_SIZE> serializedData;

	while (messagePtr < message.length) {
		//std::cout << "getting DZ" << std::endl;
		BitBuffer<MAX_DATA_FIELD_LENGTH> chunk = GetTFDataZone(messagePtr, message);
		//std::cout << "making TF" << std::endl;
		TransferFrame tf = DataToTransferFrame(type, chunk);
		//std::cout << "pack" << std::endl;
		BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packedFrame = packTransferFrame(tf);
		//std::cout << "packed" << std::endl;
		for (int i = 0; i < 8; i++) {
			std::bitset<8> b{packedFrame.data[i]};
			std::cout << b << " ";
		}
		std::cout << "\n";

		assert(serializedDataPtr + packedFrame.length <= MAX_DATA_SIZE);
		std::memcpy(&serializedData.data[serializedDataPtr], &packedFrame.data[0], packedFrame.length);
		serializedDataPtr += packedFrame.length;
	}

	serializedData.length = serializedDataPtr;

	return serializedData;
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

	BitBuffer<MAX_MESSAGE_LENGTH> messageBuffer {messageContainer, TEST_ARRAY_SIZE};
	BitBuffer<MAX_DATA_SIZE> serializedTransferFrames = DataToStream(type, messageBuffer);

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
