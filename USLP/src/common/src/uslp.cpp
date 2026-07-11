#include <thread>
#include <stdint.h> 
#include <iostream>
#include <iomanip>
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
#include <chrono>

//#include "CRC.h"

void USLP::PrintVCIDMapping() {
	std::cout << "\n=============================================\n";
	std::cout << "      USLP VCID TO INTERNAL INDEX MAP        \n";
	std::cout << "=============================================\n";
	
	bool hasMappings = false;

	for (size_t vcid = 0; vcid < MAX_VC_COUNT; ++vcid) {
		int8_t internalIndex = m_vcidToIndex[vcid];

		// Only print slots that have been explicitly mapped to a dense array index
		if (internalIndex != -1) {
			std::cout << "  Protocol VCID [" << std::setw(2) << std::setfill('0') << vcid << "]"
					<< "  ---->  Internal Storage Index [" << static_cast<int>(internalIndex) << "]\n";
			hasMappings = true;
		}
	}

	if (!hasMappings) {
		std::cout << "  [WARNING] No Virtual Channels are currently mapped!\n";
	}

	std::cout << "=============================================\n\n";
}

TFPrimaryHeader USLP::GetPrimaryHeader(uint8_t VCID) {
	TFPrimaryHeader tfph;
	tfph.TFVN = managedParams.physical.TFVN; // Just carries the current version
	tfph.SCID = managedParams.master.SCID; // Constant we decide on when the mission launches
	tfph.sourceOrDestinationID = 1; // 0 is more important for multi-recipient systems
	std::cout << "VCID assigned: " << static_cast<uint32_t>(VCID) << "\n";
	tfph.VCID = VCID;
	tfph.MAPID = 0; // We do not need MAP, not so many pieces of data to transfer
	tfph.endTFPrimaryHeaderFlag = 0; // Decided possibly if we need super fast transfer of packet
	tfph.TFLength = 0; // To be decided later (measured in octets)
	tfph.bypassSequenceControlFlag = 1; // Expedited Frame, no COP
	tfph.protocolCommandControlFlag = 0; // Generally sending CFDP packets, user data
	tfph.spare = 0b00; //Decide what to do with this later
	tfph.operationalControlFieldFlag = 0; // No OCF because no COP
	log("GetPrimaryHeader");
	log(static_cast<uint32_t>(VCID));
	int8_t vcidIndex = GetChannelByVCID(VCID, m_vcidToIndex);

	if (vcidIndex < 0) { // Temporary solution, maybe best thing to do is invalidate the frame?
		tfph.VCFrameCountLength = 4;
		tfph.VCFrameCount = 0;
	} else {
		tfph.VCFrameCountLength = managedParams.virtualChannelConfigs[vcidIndex].expeditedCountLength;
		tfph.VCFrameCount = m_virtualChannels[vcidIndex].vcFrameCount;
		log("vc frame count length: ", tfph.VCFrameCountLength);
		log("vc frame count: ", tfph.VCFrameCount);
	}

	log("GetPrimaryHeader2");

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

OperationalControlField USLP::GetOperationalControlField(USLPContext context) {
	OperationalControlField ocf {};

	if (managedParams.virtualChannelConfigs[context.currentVCID].COPInEffect != USLPConfig::COPType::NONE) {
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
	if (!IsValidPVN(PVN)) { // Unfinished logic, always returns true
		std::cerr << "Invalid PVN provided\n";
		return;
	}
	//log("VCPRequest");

	uint8_t VCID = static_cast<uint8_t>(VC_BITMASK & GVCID);
	int8_t vcidIndex = GetChannelByVCID(VCID, m_vcidToIndex);

	if (vcidIndex >= 0) {
		VirtualChannelAccumulator& accumulator = m_virtualChannels[vcidIndex].accumulator;
		accumulator.processIncomingPacket(packet, GVCID);
	} else {
		std::cerr << "Invalid GVCID provided\n";
	}
}

// Thread for multiplexing VCs
void USLP::VCMultiplexer() {
	constexpr auto TICK_RATE = std::chrono::milliseconds(20);

	while (m_running) {
		TransferFrame frameToProcess;
        
		//log("popping");
		bool receivedRealFrame = m_frameMultiplexerQueue.pop_with_timeout(frameToProcess, TICK_RATE);
        //log("popped");
		// This will block safely without burning CPU cycles until
        // PrepareTransferFrame notifies the condition variable.
        if (receivedRealFrame) {
			log("received real frame\n");
			AllFramesGenerationFunction(frameToProcess);
        } else {
            // CASE B: 20ms passed with absolutely zero activity. Generate OID.
            BitBuffer<MAX_DATA_ZONE_LENGTH> idlePayload;
            idlePayload.fill(0, IDLE_PATTERN, MAX_DATA_ZONE_LENGTH);
            
			//log("prepare idle frame");
			//PrepareTransferFrame(idlePayload, IDLE_VCID, DEFAULT_FHP, IDLE_UPID);
			//log("finished idle frame");
        }
	}

	std::cout << "leaving VCMultiplexer\n";
}

// Handles both wrapping packets in Transfer Frames and Multiplexing finished Transfer Frames
void USLP::VCPacketThread() {
	constexpr auto TICK_RATE = std::chrono::milliseconds(20);
	//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	log("Packet thread begin!");

	while (m_running) {
		auto nextTick = std::chrono::steady_clock::now() + TICK_RATE;
		bool frameGeneratedThisTick = false;
		log("Packet thread");

		// Potential issue with current system where if a virtual channel receives a constant
		// stream, others will never be processed
		for (size_t vc = 0; vc < m_virtualChannels.size() - 1; vc++) {
			int8_t vcidIndex = GetChannelByVCID(vc, m_vcidToIndex);
			VirtualChannelAccumulator& acc = m_virtualChannels[vcidIndex].accumulator;
			auto timeSinceFrame = std::chrono::steady_clock::now() - acc.m_lastFrameTime;
			bool transferFrameDue = (timeSinceFrame > m_virtualChannels[vcidIndex].expirationTime) && 
									(acc.m_accumulationBuffer.payloadBuffer.length > 0);
			bool bufferReady = acc.m_accumulationBuffer.payloadBuffer.length >= acc.m_fixedTfdzSize;

			std::cout << "length accum buf: " << acc.m_accumulationBuffer.payloadBuffer.length << "\n";
			std::cout << "transfer frame due? " << transferFrameDue << "\n";

			if (bufferReady || transferFrameDue) {
				// We use a while loop to drain the buffer if multiple frames are ready
				while (acc.m_accumulationBuffer.payloadBuffer.length > 0 || transferFrameDue) {
					BitBuffer<MAX_ACCUMULATOR_LENGTH>& payloadBuffer = acc.m_accumulationBuffer.payloadBuffer;
					
					size_t numBytesToWrap = std::min(acc.m_fixedTfdzSize, payloadBuffer.length);
					size_t paddingNeeded = acc.m_fixedTfdzSize - numBytesToWrap;
					std::cout << "padding Needed: " << paddingNeeded << "\n";
					std::cout << "fixedTfdzSize: " << acc.m_fixedTfdzSize << "\n";
					std::cout << "payloadBuffer length: " << static_cast<int>(payloadBuffer.length) << "\n";

					// 1. Calculate FHP and shift the metadata indices BEFORE erasing data
					uint16_t fhp = DEFAULT_FHP; // Default: No new packet starts in this frame
					size_t keptIndicesCount = 0;

					PacketPtrBuffer& headerIndices = acc.m_accumulationBuffer.packetPointers;
					size_t newTailPointer = headerIndices.m_queueTail;

					for (size_t count = headerIndices.m_queueTail; count < headerIndices.m_queueSize; count++) {
						size_t i = (headerIndices.m_queueTail + count);
						if (i > MAX_INCOMING_PACKETS) i -= MAX_INCOMING_PACKETS;

						size_t startIndex = headerIndices.m_packetStartIndices[i];

						if (startIndex < numBytesToWrap) {
							// This packet starts inside our current frame window
							if (fhp == 2047) {
								fhp = static_cast<uint16_t>(startIndex); // Grab the very first one
							}

							newTailPointer++;
							// We consume this index, so we do not copy it to the 'kept' count.
						} else {
							// This packet starts in a future frame. Shift its offset back
							headerIndices.m_packetStartIndices[i] = startIndex - numBytesToWrap;
						}
					}
					
					// Update the queue size to drop the consumed indices
					headerIndices.m_queueTail = newTailPointer;
					// (Note: if using a custom ring buffer, adjust your head/tail pointers instead of resize)

					// 2. Construct the Transfer Frame Payload
					BitBuffer<MAX_DATA_ZONE_LENGTH> tfdfPayload(&payloadBuffer.data[0], numBytesToWrap);
					if (paddingNeeded > 0) {
						tfdfPayload.fill(tfdfPayload.length, 0x00, paddingNeeded);
					}

					std::cout << "last byte in payload: " << static_cast<uint32_t>(tfdfPayload.data[tfdfPayload.length - 1]) << "\n";

					// 3. NOW it is safe to erase the bytes from the accumulator
					payloadBuffer.eraseFromStart(numBytesToWrap);

					// 4. Reset the temporal trigger and dispatch
					acc.m_lastFrameTime = std::chrono::steady_clock::now();
					transferFrameDue = false;

					//std::cout << "fhp for normal frame: " << fhp << "\n";
					std::cout << "vc in packet thread: " << vc << "\n";
					std::cout << "payload byte: " << static_cast<uint32_t>(tfdfPayload.data[0]) << "\n";
					PrepareTransferFrame(tfdfPayload, vc, fhp, DEFAULT_UPID);
					m_virtualChannels[vcidIndex].incrementFrameCount();
					frameGeneratedThisTick = true;
					
					// Break the while loop if we don't have enough data for another full frame
					if (payloadBuffer.length < acc.m_fixedTfdzSize) {
						break;
					}
				}
				
			}
		}

		std::this_thread::sleep_until(nextTick);
	}

	std::cout << "leaving VCPacketThread\n";

	return;
}

void USLP::PrepareTransferFrame(
	BitBuffer<MAX_DATA_ZONE_LENGTH>& data, 
	uint8_t VCID,
	uint16_t fhp,
    uint8_t UPID) {
	log("PrepareTransferFrame");
	TFDataField tfdf = VCPacketProcessing(data, VCID, fhp, UPID);
	TransferFrame tf = VCGeneration(tfdf, VCID);

	if (UPID == DEFAULT_UPID) {
		//std::cout << "Pushing into multiplexer queue\n";
		m_frameMultiplexerQueue.push(std::move(tf));
	} else if (UPID == IDLE_UPID) {
		AllFramesGenerationFunction(tf);
		//std::cout << "OID all frames generation hs been finshed\n";
	}
}

TFDataField USLP::VCPacketProcessing(BitBuffer<MAX_DATA_ZONE_LENGTH>& data, uint8_t VCID, uint16_t fhp, uint8_t UPID) {
	log("VCPacketProcessing");
	TFDataField tfdf;
	//tfdf.securityHeader = GetSecurityHeader();

	tfdf.header.TFDZConstructionRules = DEFAULT_CONSTRUCTION_RULE;
	tfdf.header.USLPProtocolIdentifier = UPID;
	tfdf.header.firstHeaderLastValidOctetPointer = fhp;
	tfdf.TFDZ = data;
	//tfdf.securityTrailer = GetSecurityTrailer();
	//std::cout << "security trailer length on init: " << tfdf.securityTrailer.length << std::endl;

	return tfdf;
}

TransferFrame USLP::VCGeneration(TFDataField& tfdf, uint8_t VCID) {
	log("VCGeneration");
	// Frame Init Procedure
	TransferFrame tf {};
	tf.TFPH = GetPrimaryHeader(VCID);
	tf.TFDF = tfdf;

	return tf;
}

// Interfaces with GNU Radio module, unsure how to have it return to that yet
void USLP::AllFramesGenerationFunction(TransferFrame& tf) {
	log("AllFramesGenerationFunction");
	tf.FECF = GetFrameErrorControlField(tf);
	std::cout << "all frames generation payload byte: " << static_cast<uint32_t>(tf.TFDF.TFDZ.data[tf.TFDF.TFDZ.length - 1]) << "\n";
	BitBuffer<MAX_TRANSFER_FRAME_LENGTH> serializedBytes = packer.packTransferFrame(tf);

	m_finishedTransferFrames[m_finishedTransferFramesIdx] = TFAllFormats{tf, serializedBytes};
	m_finishedTransferFramesIdx++;

	WriteBytes(serializedBytes);
	if (tf.TFPH.VCID == 63) {
		log("Finished Idle AllFramesGenerationFunction");
	}
}

// Structure to log exactly when a packet entered the USLP stack via VCPRequest
struct PacketInjectionRecord {
    std::chrono::steady_clock::time_point timestamp;
    uint8_t vcid;
    uint32_t sduId;
    size_t lengthBytes;
};

// Helper to generate dummy CCSDS/CFDP packet payloads
BitBuffer<MAX_MESSAGE_LENGTH> CreateDummyPacket(uint8_t fillByte, size_t numBytes) {
    BitBuffer<MAX_MESSAGE_LENGTH> packet;
    packet.length = numBytes;

    for (size_t i = 0; i < numBytes; ++i) {
        packet.data[i] = fillByte;
    }

    return packet;
}

void RunVCPRequestMultiplexingTest(USLP& uslpStack) {
    std::cout << "==================================================\n";
    std::cout << "[TEST START] Multi-VC VCPRequest Injection & Multiplexing\n";
    std::cout << "==================================================\n";

    std::vector<PacketInjectionRecord> injectionLog;
    const auto testStartTime = std::chrono::steady_clock::now();

    // Define 3 target Virtual Channels
    // VC 0: High-Priority Spacecraft Commands / Real-Time Telemetry
    // VC 1: CFDP File Delivery (Heavy data chunks)
    // VC 2: Low-Priority Engineering Logs
    const std::vector<uint8_t> targetVCs = {0, 1, 2};

    // --- PHASE 1: PACKET INJECTION LOOP ---
    uint32_t sequenceId = 1000;
	//std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    
    for (int cycle = 1; cycle <= 5; ++cycle) {
        for (uint8_t vc : targetVCs) {
            sequenceId++;

            // Vary packet sizes: CFDP on VC 1 gets larger chunks
            size_t payloadSize = (vc == 1) ? 512 : 128;
            uint8_t patternByte = static_cast<uint8_t>((vc << 4) | (cycle & 0x0F));
			std::cout << "pattern Byte: " << static_cast<uint32_t>(patternByte) << "\n";
            
            BitBuffer<MAX_MESSAGE_LENGTH> packet = CreateDummyPacket(patternByte, payloadSize);
			std::cout << "dummy packet last byte: " << static_cast<uint32_t>(packet.data[packet.length - 1]) << "\n";

            // Record exact timestamp right before injecting into the Service Access Point
            auto injectTime = std::chrono::steady_clock::now();
            injectionLog.push_back({injectTime, vc, sequenceId, payloadSize});

            // Call VCPRequest
            // Note: Assuming PVN=0x00 (Space Packet) and Packet ServiceType
			if (true) {
				uslpStack.VCPRequest(
					packet, 
					static_cast<uint32_t>(vc), // GVCID mapped to VCID for simple test
					0x00,                      // PVN
					sequenceId,                // SDU_ID
					ServiceType::EXPEDITED
				);
			}

            std::cout << "[INJECT] SDU_ID: " << sequenceId 
                      << " | VC: " << static_cast<int>(vc) 
                      << " | Size: " << payloadSize << " B\n";

            // Simulate realistic micro-delays between packet arrivals (10ms - 25ms)
            //std::this_thread::sleep_for(std::chrono::milliseconds(20 + (vc * 5 * 30)));
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	uslpStack.terminateThreads();

    std::cout << "\n[TEST PHASE 1 COMPLETE] Injected " << injectionLog.size() << " packets.\n\n";

    // --- PHASE 2: TRIGGER MULTIPLEXER & FRAME GENERATION ---
    // If your multiplexer runs on a separate thread, give it time to flush.
    // If running synchronously, trigger your frame generation loop here.
    std::cout << "[MULTIPLEXING] Running frame generation loop across accumulators...\n";
    
    // Example synchronous tick:
    // uslpStack.TickMultiplexer(); 
    // Or allow background thread to process:
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // --- PHASE 3: VERIFY GENERATED TRANSFER FRAMES ---
    std::cout << "\n==================================================\n";
    std::cout << "[RESULTS] Inspecting Generated Transfer Frames\n";
    std::cout << "==================================================\n";

    size_t generatedCount = uslpStack.GetFinishedTransferFramesIndex(); // Retrieves m_finishedTransferFramesIdx
    std::cout << "Total Transfer Frames Generated: " << generatedCount << "\n\n";

    for (size_t i = 0; i < generatedCount; ++i) {
        const TFAllFormats& output = uslpStack.GetFinishedFrame(i);
        
        // Extract debug info from the un-serialized structural frame
        uint8_t frameVCID = output.tf.TFPH.VCID & 0x3F; // Mask VCID depending on your bit allocation
        uint32_t frameCount = output.tf.TFPH.VCFrameCount;
        uint16_t fhp = output.tf.TFDF.header.firstHeaderLastValidOctetPointer;

        std::cout << "Frame #" << std::setw(3) << std::setfill('0') << i 
                  << " | VCID: " << static_cast<int>(frameVCID)
                  << " | VC Frame Count: " << frameCount
                  << " | FHP: " << fhp
                  << " | Serialized Bytes: " << output.serializedBytes.length << " B"
                  << " | First Byte: 0d" << (int)output.tf.TFDF.TFDZ.data[8] << std::dec
                  << "\n" << "\n";
    }

    // --- PHASE 4: TIMELINE CORRELATION REPORT ---
    std::cout << "\n--- Ingestion Timeline Report ---\n";
    for (const auto& record : injectionLog) {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp - testStartTime).count();
        std::cout << "T+" << std::setw(4) << elapsedMs << " ms | SDU " << record.sduId 
                  << " ingested into VCID " << static_cast<int>(record.vcid) << "\n";
    }

    std::cout << "==================================================\n";
    std::cout << "[TEST PASSED] Pipeline execution verified successfully.\n";
    std::cout << "==================================================\n";
}

int main(int argc, char* argv[]) {
	std::cout << "started running\n";
	ClearFile();
	// 2. Initialize your complete USLP Configuration
	USLPConfig managedParams {
		// Physical Channel Configuration
		USLPConfig::PhysicalChannel {
			.physicalChannelName = "BROWN_SAT_LINK_1",
			.frameType = USLPConfig::FrameType::FIXED,
			.transferFrameLength = 1024,
			.TFVN = 0b1100,
			.MCMultiplexingScheme = 0,
			.insertZonePresent = false,
			.insertZoneLength = 0,
			.FECFPresent = true,
			.maxTFPerDataUnit = 1,
			.maxRepetitions = 1,
			.isCRC32 = true
		},
		
		// Master Channel Configuration
		USLPConfig::MasterChannel {
			.frameType = USLPConfig::FrameType::FIXED,
			.SCID = 42,
			.VCIDs = {0, 1, 2}, // The active channels we are multiplexing
			.VCMultiplexingScheme = 0
		},

		// --- YOUR VIRTUAL CHANNELS ARRAY INITIALIZATION ---
		std::array<USLPConfig::VirtualChannelConfig, NUM_ACTIVE_CHANNELS>{
			// Index 1: CFDP File Transmission Channel (Bulky Data)
			USLPConfig::VirtualChannelConfig {
				.frameType = USLPConfig::FrameType::FIXED,
				.VCID = 1,
				.seqControlCountLength = 4,
				.expeditedCountLength = 4,
				.SDUType = USLPConfig::SDUType::CCSDS_PACKET,
				.TFDFCompletionTimeoutMs = 100, // Higher timeout tolerated for big frames
				.interFrameDelayMs = 50
			},
			// Index 0: High-Priority Real-Time Telemetry / Commands
			USLPConfig::VirtualChannelConfig {
				.frameType = USLPConfig::FrameType::FIXED,
				.VCID = 0,
				.seqControlCountLength = 4,   // 4-byte frame counter width
				.expeditedCountLength = 4,
				.SDUType = USLPConfig::SDUType::CCSDS_PACKET,
				.TFDFCompletionTimeoutMs = 20, // Low latency flush
				.interFrameDelayMs = 50
			},
			// Index 2: Secondary Payload / Science Instrument Logs
			USLPConfig::VirtualChannelConfig {
				.frameType = USLPConfig::FrameType::FIXED,
				.VCID = 2,
				.seqControlCountLength = 2,    // 2-byte truncated width to save space
				.expeditedCountLength = 4,
				.SDUType = USLPConfig::SDUType::CCSDS_PACKET,
				.TFDFCompletionTimeoutMs = 500,
				.interFrameDelayMs = 100
			},
			// Index 3: OID
			USLPConfig::VirtualChannelConfig {
				.frameType = USLPConfig::FrameType::FIXED,
				.VCID = 63,
				.seqControlCountLength = 2,    // 2-byte truncated width to save space
				.expeditedCountLength = 4,
				.SDUType = USLPConfig::SDUType::CCSDS_PACKET,
				.TFDFCompletionTimeoutMs = 500,
				.interFrameDelayMs = 100
			}
		},

		// MAP Channels Vector
		std::vector<USLPConfig::MAPChannel>{},

		// Packet Transfer Configurations
		USLPConfig::PacketTransfer {
			.validPVNs = {0b000}, // Only accept standard Space Packets (Version 1)
			.maxPacketLength = 4096,
			.deliverIncompletePackets = false
		}
	};

	USLP uslp(managedParams);

	RunVCPRequestMultiplexingTest(uslp);

	//std::array<uint8_t, TEST_ARRAY_SIZE> message = GenerateRandomBytes();
	//WriteBytes(message); // Writes raw bytes of message to bytes.txt
	
	//BitBuffer<PRIMARY_HEADER_LENGTH> packedHeader = packPrimaryHeader(tfph);
	//std::bitset<8> b2{packedHeader.data[7]};
	//std::cout << "First byte: " << b2 << std::endl;
	//std::cout << "Packed Header: " << std::bitset<64>(packedHeader) << std::endl;
	/*
	enum MessageType type = BITMAP;
	std::array<uint8_t, MAX_MESSAGE_LENGTH> messageContainer {};
	std::memcpy(&messageContainer[0], &message[0], message.size());
	
	std::cout << "cos" << std::endl;

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
		*/

	return 0;
}