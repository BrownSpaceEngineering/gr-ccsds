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
#include <common/tests.h>
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
	tfph.sourceOrDestinationID = 1; // CHANGE TO 0 ON THE FSW VERSION OF THIS CODE (Determines whether SCID refers to the frame source or destination)
	tfph.VCID = VCID;
	tfph.MAPID = 0; // We do not need MAP, not so many pieces of data to transfer
	tfph.endTFPrimaryHeaderFlag = 0; // Decided possibly if we need super fast transfer of packet
	tfph.TFLength = MAX_TRANSFER_FRAME_LENGTH - 1; // Could be variable, current model transmits fixed frames (in octets) (-1 since spec says one less than actual size)
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
		//std::cout << std::endl;
		//std::cout << "vcid: " << static_cast<int>(VCID) << std::endl;
		//std::cout << "vc frame count: " << tfph.VCFrameCount << std::endl;
	}

	log("GetPrimaryHeader2");
	PrintPrimaryHeader(tfph);

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
        // Initialize a placeholder FECF with the correct length.
        // The actual CRC calculation is deferred to AllFramesGenerationFunction 
        // after the entire frame has been serialized into a byte stream.
        size_t fecfSize = managedParams.physical.isCRC32 ? 4 : 2;
        fecf.FECFData.length = fecfSize;
        
        // Fill with dummy zeros for now
        for (size_t i = 0; i < fecfSize; ++i) {
            fecf.FECFData.data[i] = 0x00;
        }
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

	if (packet.length > MAX_MESSAGE_LENGTH) {
		std::cerr << "Provided length longer than maximum allowed packet size\n";
	}

	uint8_t VCID = static_cast<uint8_t>(VC_BITMASK & GVCID);
	int8_t vcidIndex = GetChannelByVCID(VCID, m_vcidToIndex);

	if (vcidIndex >= 0) {
		VirtualChannelAccumulator& accumulator = m_virtualChannels[vcidIndex].accumulator;
		
		accumulator.processIncomingPacket(packet, GVCID, PVN, SDU_ID, serviceType);
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
		std::unique_lock<std::mutex> lock(m_multiplexerMtx);
		bool receivedRealFrame = m_frameMultiplexerQueue.pop_with_timeout(frameToProcess, TICK_RATE);
		lock.unlock();
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
			int8_t vcidIndex = GetChannelByVCID(IDLE_VCID, m_vcidToIndex);
			m_virtualChannels[vcidIndex].incrementFrameCount();
            
			//log("prepare idle frame");
			PrepareTransferFrame(idlePayload, IDLE_VCID, DEFAULT_FHP, IDLE_UPID, {});
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

			std::unique_lock<std::mutex> lock(acc.m_bufferMtx);

			auto timeSinceFrame = std::chrono::steady_clock::now() - acc.m_lastFrameTime;
			bool transferFrameDue = (timeSinceFrame > m_virtualChannels[vcidIndex].expirationTime) && 
									(acc.m_accumulationBuffer.payloadBuffer.length > 0);
			bool bufferReady = acc.m_accumulationBuffer.payloadBuffer.length >= acc.m_fixedTfdzSize;

			if (bufferReady || transferFrameDue) {
				// We use a while loop to drain the buffer if multiple frames are ready
				while (acc.m_accumulationBuffer.payloadBuffer.length > 0 || transferFrameDue) {
					BitBuffer<MAX_ACCUMULATOR_LENGTH>& payloadBuffer = acc.m_accumulationBuffer.payloadBuffer;
					
					size_t numBytesToWrap = std::min(acc.m_fixedTfdzSize, payloadBuffer.length);
					size_t paddingNeeded = acc.m_fixedTfdzSize - numBytesToWrap;

					// 1. Calculate FHP and shift the metadata indices BEFORE erasing data
					uint16_t fhp = DEFAULT_FHP; // Default: No new packet starts in this frame (65535)

					PacketPtrBuffer& headerIndices = acc.m_accumulationBuffer.packetPointers;
					size_t consumedCount = 0;
					
					// Local vector to track completed SDU IDs for this specific frame
					std::vector<uint32_t> associatedSduIds;
					associatedSduIds.reserve(32);

					// --- PHASE 1: Handle Spillover Completion ---
					// If there is an active spillover packet from a previous frame, check if it finishes here
					if (acc.m_spilloverBytesRemaining > 0) {
						size_t bytesConsumedInThisFrame = std::min(acc.m_spilloverBytesRemaining, numBytesToWrap);
						acc.m_spilloverBytesRemaining -= bytesConsumedInThisFrame;

						// If no bytes remain, the spillover packet has officially completed inside this frame!
						if (acc.m_spilloverBytesRemaining == 0) {
							associatedSduIds.push_back(acc.m_spilloverSduId);
							acc.m_spilloverSduId = 0; // Reset spillover tracking
						}
					}

					// --- PHASE 2: Evaluate New Packets ---
					for (size_t count = 0; count < headerIndices.m_queueSize; count++) {
						size_t i = (headerIndices.m_queueTail + count);
						if (i >= MAX_INCOMING_PACKETS) i -= MAX_INCOMING_PACKETS;

						// Access the detailed packet metadata we designed
						PacketMetadata& meta = headerIndices.m_queuedPackets[i];
						size_t startIndex = meta.startIndex;
						size_t packetLength = meta.length;

						if (startIndex < numBytesToWrap) {
							// This packet starts inside our current frame window
							if (fhp == DEFAULT_FHP) {
								fhp = static_cast<uint16_t>(startIndex); // Grab the very first one
							}

							// Check if the entire packet fits and finishes inside this frame
							size_t packetEndIndex = startIndex + packetLength;
							if (packetEndIndex <= numBytesToWrap) {
								// Packet finishes in this frame! Add SDU ID immediately
								associatedSduIds.push_back(meta.sduId);
							} else {
								// Packet starts here but spills over to the next frame.
								// Do NOT add to associatedSduIds yet. Record the spillover state.
								acc.m_spilloverSduId = meta.sduId;
								acc.m_spilloverBytesRemaining = packetEndIndex - numBytesToWrap;
							}

							consumedCount++;
						} else {
							// This packet starts in a future frame. Shift its offset back
							meta.startIndex = startIndex - numBytesToWrap;
						}
					}

					// Cleanly pop the consumed packets from the queue
					for (size_t k = 0; k < consumedCount; k++) {
						headerIndices.pop();
					}

					// 2. Construct the Transfer Frame Payload
					BitBuffer<MAX_DATA_ZONE_LENGTH> tfdfPayload(&payloadBuffer.data[0], numBytesToWrap);
					if (paddingNeeded > 0) {
						tfdfPayload.fill(tfdfPayload.length, CCSDS_EIP_HEADER, paddingNeeded);
					}

					// 3. NOW it is safe to erase the bytes from the accumulator
					payloadBuffer.eraseFromStart(numBytesToWrap);

					// 4. Reset the temporal trigger and dispatch
					acc.m_lastFrameTime = std::chrono::steady_clock::now();
					transferFrameDue = false;

					lock.unlock();

					// --- Passing off the metadata to PrepareTransferFrame ---
					PrepareTransferFrame(tfdfPayload, vc, fhp, DEFAULT_UPID, associatedSduIds);
					m_virtualChannels[vcidIndex].incrementFrameCount();
					frameGeneratedThisTick = true;
					
					// Break the while loop if we don't have enough data for another full frame
					if (payloadBuffer.length < acc.m_fixedTfdzSize) {
						break;
					}

					lock.lock();
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
    uint8_t UPID,
	std::vector<uint32_t> associatedSduIds) {
	log("PrepareTransferFrame");
	TFDataField tfdf = VCPacketProcessing(data, VCID, fhp, UPID);
	TransferFrame tf = VCGeneration(tfdf, VCID);

	tf.associatedSduIds = std::move(associatedSduIds);

	if (tfdf.header.USLPProtocolIdentifier == DEFAULT_UPID) {
		//std::cout << "Pushing into multiplexer queue\n";
		std::lock_guard<std::mutex> lock(m_multiplexerMtx);
		m_frameMultiplexerQueue.push(std::move(tf));
	} else if (tfdf.header.USLPProtocolIdentifier == IDLE_UPID) {
		//AllFramesGenerationFunction(tf);
		//std::cout << "OID all frames generation hs been finshed\n";
	} else {
		std::cerr << "Unknown UPID, dropping frame\n";
	}
}

TFDataField USLP::VCPacketProcessing(BitBuffer<MAX_DATA_ZONE_LENGTH>& data, uint8_t VCID, uint16_t fhp, uint8_t UPID) {
	log("VCPacketProcessing");
	TFDataField tfdf;
	//tfdf.securityHeader = GetSecurityHeader();

	if (UPID == IDLE_UPID) {
		tfdf.header.TFDZConstructionRules = IDLE_CONSTRUCTION_RULE;
	} else {
		tfdf.header.TFDZConstructionRules = DEFAULT_CONSTRUCTION_RULE;
	}
	
	tfdf.header.USLPProtocolIdentifier = UPID;
	tfdf.header.firstHeaderLastValidOctetPointer = fhp;
	//std::cout << "fhp in VCPacketProcessing: " << fhp << "\n";
	tfdf.TFDZ = data;
	//tfdf.securityTrailer = GetSecurityTrailer();

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

    // 1. Initialize the placeholder FECF so the packer knows to reserve space at the end of the frame
    tf.FECF = GetFrameErrorControlField(tf);

    // 2. Pack the Transfer Frame first.
    // If managedParams.physical.FECFPresent is true, packer.packTransferFrame(tf) 
    // will reserve the final 2 or 4 bytes at the end of serializedBytes 
    // (filled with the dummy 0x00 placeholders from GetFrameErrorControlField).
    BitBuffer<MAX_TRANSFER_FRAME_LENGTH> serializedBytes = packer.packTransferFrame(tf);

    // 3. If FECF is present, calculate the CRC over the serialized byte stream
    if (managedParams.physical.FECFPresent) {
        size_t fecfSize = managedParams.physical.isCRC32 ? 4 : 2;
        
        if (serializedBytes.length <= fecfSize) {
            std::cerr << "[ERROR] USLP: Serialized frame is too short to compute FECF.\n";
            return;
        }

        // The CRC must be calculated over all bytes EXCLUDING the FECF itself
        size_t crcInputLength = serializedBytes.length - fecfSize;
        uint32_t computedCRC = ComputeCRC(serializedBytes.data.data(), crcInputLength, managedParams.physical.isCRC32);

        // 4. Dynamically pack the CRC in a loop (handles both CRC-16 and CRC-32)
        for (size_t i = 0; i < fecfSize; ++i) {
            // Calculate shift dynamically:
            // For 4 bytes (CRC-32), shifts are 24, 16, 8, 0.
            // For 2 bytes (CRC-16), shifts are 8, 0.
            size_t shift = (fecfSize - 1 - i) * 8;
            uint8_t byteVal = static_cast<uint8_t>((computedCRC >> shift) & 0xFF);
            
            // Overwrite serialized stream and update structural TF so they stay in sync
            serializedBytes.data[crcInputLength + i] = byteVal;
            tf.FECF.FECFData.data[i] = byteVal;
        }
        tf.FECF.FECFData.length = fecfSize;
    }

    // 5. Store the fully accurate, synchronized frame in our history log
    m_finishedTransferFrames[m_finishedTransferFramesIdx] = TFAllFormats{tf, serializedBytes};
    m_finishedTransferFramesIdx++;

	cout << "Length of serialized bytes: " << serializedBytes.length << endl;
	
	std::vector<uint8_t> physicalBytes = ccs_sl.ProcessFrame(serializedBytes);
	BitBuffer<MAX_CCS_SL_FRAME_LENGTH> physicalFrameBuffer(physicalBytes.data(), physicalBytes.size());
	cout << "Length of physical bytes: " << physicalFrameBuffer.length << endl;

    // 6. Send out over our ports and queues
    WriteBytes(physicalFrameBuffer);
    SendToGNURadio(physicalFrameBuffer);
    m_receptionQueue.push(serializedBytes);

	if (m_vcpNotifyCallback) {
        uint32_t gvcid = static_cast<uint32_t>(tf.TFPH.VCID);
        uint8_t pvn = 0b000; // Space Packet Protocol

        for (uint32_t sduId : tf.associatedSduIds) {
            // Notify the upper layer (CFDP) that this packet has physically departed
            m_vcpNotifyCallback(gvcid, pvn, sduId, ServiceType::EXPEDITED, TRANSMITTED_SUCCESSFULLY);
        }
    }

    if (tf.TFPH.VCID == 63) {
        log("Finished Idle AllFramesGenerationFunction");
    }
}

void USLP::CompletedPacketsWriterThread() {
    // 50ms polling interval balances latency with CPU efficiency on embedded systems
    constexpr auto TICK_RATE = std::chrono::milliseconds(50);
    
    while (m_running) {
        bool processedAnyPacket = false;
        
        // Iterate through all Virtual Channels to check their completed packet queues
        for (size_t i = 0; i < m_rxVirtualChannels.size(); ++i) {
            RxVirtualChannelState& rxState = m_rxVirtualChannels[i];
            BitBuffer<MAX_MESSAGE_LENGTH> completedPacket;
            
            // Drain the queue completely if multiple packets finished in this cycle
            while (rxState.completedPacketsQueue.pop(completedPacket)) {
                processedAnyPacket = true;
                
                // Determine the correct VCID associated with this state index
                uint8_t vcid = 0;
                if (i < m_rxVirtualChannels.size() - 1) {
                    vcid = managedParams.virtualChannelConfigs[i].VCID;
                } else {
                    vcid = IDLE_VCID; // VCID 63 (OID / Idle Channel)
                }
                
                // Open the file in append mode
                std::ofstream out("PacketOutput.txt", std::ios::app);
                
                out << "\n\n";
                out << "==================================" << "\n";
                out << "     RECONSTRUCTED SPACE PACKET   " << "\n";
                out << "==================================" << "\n";
                out << "  Source Virtual Channel (VCID): " << static_cast<int>(vcid) << "\n";
                out << "  Total Packet Size (Bytes)    : " << completedPacket.length << "\n";
                out << "----------------------------------";

                int lastLineIndex = 0;
                for (size_t j = 0; j < completedPacket.length; ++j) {
                    // Start a new line every 32 bytes to match WriteBytes formatting
                    if ((j - lastLineIndex) % 32 == 0) {
                        out << "\n";
                    }
                    
                    out << std::setw(3) << static_cast<int>(completedPacket.data[j]) << "  ";
                }
                out << "\n" << "==================================" << std::endl;
            }
        }
        
        // If no packets were processed during this sweep, sleep to yield the CPU
        if (!processedAnyPacket) {
            std::this_thread::sleep_for(TICK_RATE);
        }
    }
}

int main(int argc, char* argv[]) {
	std::cout << "started running\n";
	/*CCS_SLConfig m_config;
	CCS_SL test(m_config);
	std::vector<uint8_t> bits = {0b10001010, 0b01101110};
	test.EncodeAndTransmit(bits.data(), 1);

	for (int i = 0; i < bits.size(); i++) {
		cout << std::bitset<8>(bits[i]) << endl;
	}*/

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
				.expeditedCountLength = 1,
				.SDUType = USLPConfig::SDUType::CCSDS_PACKET,
				.TFDFCompletionTimeoutMs = 100 // Higher timeout tolerated for big frames
			},
			// Index 0: High-Priority Real-Time Telemetry / Commands
			USLPConfig::VirtualChannelConfig {
				.frameType = USLPConfig::FrameType::FIXED,
				.VCID = 0,
				.seqControlCountLength = 4,   // 4-byte frame counter width
				.expeditedCountLength = 1,
				.SDUType = USLPConfig::SDUType::CCSDS_PACKET,
				.TFDFCompletionTimeoutMs = 20, // Low latency flush
			},
			// Index 2: Secondary Payload / Science Instrument Logs
			USLPConfig::VirtualChannelConfig {
				.frameType = USLPConfig::FrameType::FIXED,
				.VCID = 2,
				.seqControlCountLength = 2,    // 2-byte truncated width to save space
				.expeditedCountLength = 1,
				.SDUType = USLPConfig::SDUType::CCSDS_PACKET,
				.TFDFCompletionTimeoutMs = 500
			},
			// Index 3: OID
			USLPConfig::VirtualChannelConfig {
				.frameType = USLPConfig::FrameType::FIXED,
				.VCID = 63,
				.seqControlCountLength = 2,    // 2-byte truncated width to save space
				.expeditedCountLength = 1,
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

	CCS_SLConfig ccs_slConfig;
	USLP uslp(managedParams, ccs_slConfig);
	PacketTransmissionTracker txTracker;

	// 3. Register the callback to update the tracker upon actual physical transmission
    uslp.RegisterVcpNotifyCallback([&txTracker](uint32_t gvcid, uint8_t pvn, uint32_t sduId, ServiceType srvType, USLP::NotificationType notifyType) {
        if (notifyType == USLP::TRANSMITTED_SUCCESSFULLY) {
            // Signal the tracker that the packet has cleared AllFramesGenerationFunction
            txTracker.markAsTransmitted(sduId);
        }
    });

	//RunUslpTemporalStandardsTest();
	RunVCPRequestMultiplexingTest(uslp, txTracker);

	return 0;
}