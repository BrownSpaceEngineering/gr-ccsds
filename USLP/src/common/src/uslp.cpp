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
//#include "CRC.h"

TFPrimaryHeader USLP::GetPrimaryHeader(uint8_t VCID) {
	TFPrimaryHeader tfph;
	tfph.TFVN = managedParams.physical.TFVN; // Just carries the current version
	tfph.SCID = managedParams.master.SCID; // Constant we decide on when the mission launches
	tfph.sourceOrDestinationID = 1; // 0 is more important for multi-recipient systems
	tfph.VCID = VCID;
	tfph.MAPID = 0; // We do not need MAP, not so many pieces of data to transfer
	tfph.endTFPrimaryHeaderFlag = 0; // Decided possibly if we need super fast transfer of packet
	tfph.TFLength = 0; // To be decided later (measured in octets)
	tfph.bypassSequenceControlFlag = 1; // Expedited Frame, no COP
	tfph.protocolCommandControlFlag = 0; // Generally sending CFDP packets, user data
	tfph.spare = 0b00; //Decide what to do with this later
	tfph.operationalControlFieldFlag = 0; // No OCF because no COP
	tfph.VCFrameCountLength = managedParams.virtualChannels[VCID].expeditedCountLength;
	tfph.VCFrameCountField = virtualChannels[VCID].vcFrameCount;

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

TFDataField USLP::GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data, uint32_t GVCID) {
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

FrameErrorControlField USLP::GetFrameErrorControlField(TransferFrame& tf) {
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
/*TransferFrame USLP::DataToTransferFrame(uint8_t VCID, BitBuffer<MAX_DATA_FIELD_LENGTH> message, USLPContext context) {
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
}*/

// (To be implemented) Determines how many bytes of the message should be sent in the next transfer frame
/*uint16_t USLP::TFDataSize(uint8_t vcid) {
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
}*/

// Converts part of message into the data that will be wrapped in a transfer frame
/*
BitBuffer<MAX_DATA_FIELD_LENGTH> USLP::GetTFDataZone(uint16_t &messagePtr, BitBuffer<MAX_MESSAGE_LENGTH> message, USLPContext context) {
	BitBuffer<MAX_DATA_FIELD_LENGTH> chunk;
	uint16_t maxTFDataSize = maxDataZoneLengths[context.currentVCID];
	uint16_t numBytesCopy = std::min(maxTFDataSize, static_cast<uint16_t>(message.length - messagePtr));
	chunk.length = numBytesCopy;
	std::memcpy(&chunk.data[0], &message.data[messagePtr], numBytesCopy);
	messagePtr += numBytesCopy;
	
	return chunk;
}*/

// TO-DO: Checks with SANA registry
bool USLP::IsValidPVN(uint8_t PVN) {
	return true;
}

void USLP::VCPRequest(
        BitBuffer<MAX_MESSAGE_LENGTH> packet, 
        uint32_t GVCID,
        uint8_t PVN,
        uint32_t SDU_ID,
        ServiceType serviceType) {
	if (!IsValidPVN(PVN)) return;

	uint8_t VCID = static_cast<uint8_t>(VC_BITMASK & GVCID);

	if (VCID < VC_COUNT) {
		VirtualChannelAccumulator& accumulator = virtualChannels[VCID].accumulator;
		accumulator.processIncomingPacket(packet, GVCID);
	}
}

// Thread for multiplexing VCs
void USLP::VCMultiplexer() {
	constexpr auto TICK_RATE = std::chrono::milliseconds(20);

	while (m_running) {
		TransferFrame frameToProcess;
        
		bool receivedRealFrame = m_frameMultiplexerQueue.pop_with_timeout(frameToProcess, TICK_RATE);
        // This will block safely without burning CPU cycles until 
        // PrepareTransferFrame notifies the condition variable.
        if (receivedRealFrame) {
			AllFramesGenerationFunction(frameToProcess);
        } else {
            // CASE B: 20ms passed with absolutely zero activity. Generate OID.
            BitBuffer<MAX_DATA_ZONE_LENGTH> idlePayload;
            idlePayload.fill(0, IDLE_PATTERN, MAX_DATA_ZONE_LENGTH);
            
			PrepareTransferFrame(idlePayload, IDLE_VCID, DEFAULT_FHP, IDLE_UPID);
        }
	}
}

// Handles both wrapping packets in Transfer Frames and Multiplexing finished Transfer Frames
void USLP::VCPacketThread() {
	constexpr auto TICK_RATE = std::chrono::milliseconds(20);

	while (m_running) {
		auto nextTick = std::chrono::steady_clock::now() + TICK_RATE;
		bool frameGeneratedThisTick = false;

		for (size_t vc = 0; vc < VC_COUNT; vc++) {
			VirtualChannelAccumulator& acc = virtualChannels[vc].accumulator;
			auto timeSinceFrame = std::chrono::steady_clock::now() - acc.m_lastFrameTime;
			bool bufferHasData = acc.m_accumulationBuffer.payloadBuffer.length > 0;
			bool transferFrameDue = timeSinceFrame > virtualChannels[vc].expirationTime;
			bool bufferReady = bufferHasData && (acc.m_accumulationBuffer.payloadBuffer.length >= acc.m_fixedTfdfSize);

			if (bufferReady || transferFrameDue) {
				// We use a while loop to drain the buffer if multiple frames are ready
				while (acc.m_accumulationBuffer.payloadBuffer.length > 0 || transferFrameDue) {
					BitBuffer<MAX_ACCUMULATOR_LENGTH>& payloadBuffer = acc.m_accumulationBuffer.payloadBuffer;
					
					size_t numBytesToWrap = std::min(acc.m_fixedTfdfSize, payloadBuffer.length);
					size_t paddingNeeded = acc.m_fixedTfdfSize - numBytesToWrap;

					// 1. Calculate FHP and shift the metadata indices BEFORE erasing data
					uint16_t fhp = DEFAULT_FHP; // Default: No new packet starts in this frame
					size_t keptIndicesCount = 0;

					PacketPtrBuffer& headerIndices = acc.m_accumulationBuffer.packetPointers;
					size_t newTailPointer = headerIndices.m_queueTail;

					for (size_t count = headerIndices.m_queueTail; count < headerIndices.m_queueSize; count++) {
						size_t i = (headerIndices.m_queueTail + count) % MAX_INCOMING_PACKETS;
						size_t startIndex = headerIndices.m_packetStartIndices[i];

						if (startIndex < numBytesToWrap) {
							// This packet starts inside our current frame window
							if (fhp == 2047) {
								fhp = static_cast<uint16_t>(startIndex); // Grab the very first one
							}

							newTailPointer++;
							// We consume this index, so we do NOT copy it to the 'kept' count.
						} else {
							// This packet starts in a future frame. Shift its offset back!
							headerIndices.m_packetStartIndices[i] = startIndex - numBytesToWrap;
						}
					}
					
					// Update the queue size to drop the consumed indices
					headerIndices.m_queueTail = newTailPointer;
					// (Note: if using a custom ring buffer, adjust your head/tail pointers instead of resize)

					// 2. Construct the Transfer Frame Payload
					BitBuffer<MAX_DATA_FIELD_LENGTH> tfdfPayload(&payloadBuffer.data[0], numBytesToWrap);
					if (paddingNeeded > 0) {
						tfdfPayload.fill(tfdfPayload.length, 0x00, paddingNeeded);
					}

					// 3. NOW it is safe to erase the bytes from the accumulator
					payloadBuffer.eraseFromStart(numBytesToWrap);

					// 4. Reset the temporal trigger and dispatch
					acc.m_lastFrameTime = std::chrono::steady_clock::now();
					transferFrameDue = false;

					PrepareTransferFrame(tfdfPayload, vc, fhp, DEFAULT_UPID);
					virtualChannels[vc].incrementFrameCount();
					frameGeneratedThisTick = true;
					
					// Break the while loop if we don't have enough data for another full frame
					if (payloadBuffer.length < acc.m_fixedTfdfSize) {
						break;
					}
				}
				
			}
		}

		std::this_thread::sleep_until(nextTick);
	}

	return;
}

void USLP::PrepareTransferFrame(
	BitBuffer<MAX_DATA_ZONE_LENGTH>& data, 
	uint8_t VCID,
	uint16_t fhp,
    uint8_t UPID) {
	TFDataField tfdf = VCPacketProcessing(data, VCID, fhp, UPID);
	TransferFrame tf = VCGeneration(tfdf, VCID);

	if (UPID == DEFAULT_UPID) {
		m_frameMultiplexerQueue.push(std::move(tf));
	} else if (UPID == IDLE_UPID) {
		AllFramesGenerationFunction(tf);
	}
}

TFDataField USLP::VCPacketProcessing(BitBuffer<MAX_DATA_ZONE_LENGTH>& data, uint8_t VCID, uint16_t fhp, uint8_t UPID) {
	TFDataField tfdf;
	//tfdf.securityHeader = GetSecurityHeader();

	tfdf.header.TFDZConstructionRules = DEFAULT_CONSTRUCTION_RULE;
	tfdf.header.USLPProtocolIdentifier = UPID;
	tfdf.header.FirstHeaderLastValidOctetPointer = fhp;
	tfdf.TFDZ = data;
	//tfdf.securityTrailer = GetSecurityTrailer();
	//std::cout << "security trailer length on init: " << tfdf.securityTrailer.length << std::endl;

	return tfdf;
}

TransferFrame USLP::VCGeneration(TFDataField& tfdf, uint8_t VCID) {
	// Frame Init Procedure
	TransferFrame tf {};
	tf.TFPH = GetPrimaryHeader(VCID);
	tf.TFDF = tfdf;

	return tf;
}

TransferFrame USLP::AllFramesGenerationFunction(TransferFrame& tf) {
	tf.FECF = GetFrameErrorControlField(tf);
	BitBuffer<MAX_TRANSFER_FRAME_LENGTH> serializedBytes = packer.packTransferFrame(tf);

	return tf;
}

// Converts higher level input data into a stream of bytes for the physical layer
/*BitBuffer<MAX_DATA_SIZE> USLP::DataToStream(MessageType type, BitBuffer<MAX_MESSAGE_LENGTH> message) {
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
}*/

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

	USLPConfig managedParams {};
	USLP uslp(managedParams);
	std::cout << "cons" << std::endl;

	BitBuffer<MAX_MESSAGE_LENGTH> messageBuffer(&messageContainer[0], TEST_ARRAY_SIZE);
	//BitBuffer<MAX_DATA_SIZE> serializedTransferFrames = uslp.DataToStream(type, messageBuffer);

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

		//out << std::setw(3) << static_cast<int>(serializedTransferFrames.data[i]) << "  ";
	}

	return 0;
}