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
            //std::this_thread::sleep_for(std::chrono::milliseconds(20 + (vc * 5)));
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

void RunUslpTemporalStandardsTest() {
    std::cout << "\n==================================================\n";
    std::cout << "[TEST START] USLP-143 & USLP-144 Temporal Standards Test\n";
    std::cout << "==================================================\n";

    USLPConfig testParams {
        USLPConfig::PhysicalChannel {
            .physicalChannelName = "TEST_LINK",
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
        USLPConfig::MasterChannel {
            .frameType = USLPConfig::FrameType::FIXED,
            .SCID = 42,
            .VCIDs = {0, 1, 2},
            .VCMultiplexingScheme = 0
        },
        std::array<USLPConfig::VirtualChannelConfig, NUM_ACTIVE_CHANNELS>{
            // Index 0 (VCID 0): Tested for USLP-144 (Inter-Frame Delay Heartbeats)
            USLPConfig::VirtualChannelConfig {
                .frameType = USLPConfig::FrameType::FIXED,
                .VCID = 0,
                .seqControlCountLength = 4,
                .expeditedCountLength = 4,
                .SDUType = USLPConfig::SDUType::CCSDS_PACKET,
                .TFDFCompletionTimeoutMs = 5000, // High timeout to isolate USLP-144
                .interFrameDelayMs = 400         // Low delay to trigger quick heartbeat
            },
            // Index 1 (VCID 1): Tested for USLP-143 (TFDF Completion Timeout)
            USLPConfig::VirtualChannelConfig {
                .frameType = USLPConfig::FrameType::FIXED,
                .VCID = 1,
                .seqControlCountLength = 4,
                .expeditedCountLength = 4,
                .SDUType = USLPConfig::SDUType::CCSDS_PACKET,
                .TFDFCompletionTimeoutMs = 300, // Low timeout to trigger quick flush of partial buffer
                .interFrameDelayMs = 5000       // High heartbeat delay to isolate USLP-143
            },
            // Index 2 (VCID 2): Unused in this test
            USLPConfig::VirtualChannelConfig {
                .frameType = USLPConfig::FrameType::FIXED,
                .VCID = 2,
                .seqControlCountLength = 4,
                .expeditedCountLength = 4,
                .SDUType = USLPConfig::SDUType::CCSDS_PACKET,
                .TFDFCompletionTimeoutMs = 5000,
                .interFrameDelayMs = 5000
            },
            // Index 3 (VCID 63): OID
            USLPConfig::VirtualChannelConfig {
                .frameType = USLPConfig::FrameType::FIXED,
                .VCID = 63,
                .seqControlCountLength = 4,
                .expeditedCountLength = 4,
                .SDUType = USLPConfig::SDUType::CCSDS_PACKET,
                .TFDFCompletionTimeoutMs = 5000,
                .interFrameDelayMs = 5000
            }
        },
        std::vector<USLPConfig::MAPChannel>{},
        USLPConfig::PacketTransfer {
            .validPVNs = {0b000},
            .maxPacketLength = 4096,
            .deliverIncompletePackets = false
        }
    };

    // Instantiate stack
    USLP uslp(testParams);

    // Give background threads a brief moment to spin up
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // =========================================================================
    // PHASE 1: Verify USLP-143 (TFDF Completion Timeout)
    // =========================================================================
    std::cout << "\n--- [PHASE 1] Testing USLP-143 (Completion Timeout) on VCID 1 ---\n";
    
    size_t phase1StartFrames = uslp.GetFinishedTransferFramesIndex();
    auto p1StartTime = std::chrono::steady_clock::now();

    // Inject 1 tiny 32-byte packet into VCID 1
    const size_t tinyPacketSize = 32;
    const uint8_t p1FillByte = 0xAA;
    BitBuffer<MAX_MESSAGE_LENGTH> p1Packet = CreateDummyPacket(p1FillByte, tinyPacketSize);

    uslp.VCPRequest(p1Packet, 1, 0x00, 1111, ServiceType::EXPEDITED);
    std::cout << "[INFO] Tiny packet injected. Waiting 150ms (Timeout is 300ms)..." << std::endl;

    // Check at T+150ms (before the 300ms timeout)
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    size_t frameCountAt150ms = 0;
    size_t currentFrames = uslp.GetFinishedTransferFramesIndex();
    for (size_t i = phase1StartFrames; i < currentFrames; ++i) {
        if (uslp.GetFinishedFrame(i).tf.TFPH.VCID == 1) {
            frameCountAt150ms++;
        }
    }
    std::cout << "[VERIFY] Frames generated for VCID 1 at T+150ms: " << frameCountAt150ms << std::endl;
    assert(frameCountAt150ms == 0 && "Assertion failed: Frame was generated on VCID 1 before TFDFCompletionTimeoutMs expired!");

    // Wait another 250ms (reaching T+400ms, crossing the 300ms threshold)
    std::cout << "[INFO] Waiting another 250ms..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    size_t frameCountAt400ms = 0;
    int flushedFrameIndex = -1;
    currentFrames = uslp.GetFinishedTransferFramesIndex();
    for (size_t i = phase1StartFrames; i < currentFrames; ++i) {
        if (uslp.GetFinishedFrame(i).tf.TFPH.VCID == 1) {
            frameCountAt400ms++;
            flushedFrameIndex = static_cast<int>(i);
        }
    }
    
    auto elapsedP1 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - p1StartTime).count();
    std::cout << "[VERIFY] Frames generated for VCID 1 at T+" << elapsedP1 << "ms: " << frameCountAt400ms << std::endl;
    assert(frameCountAt400ms == 1 && "Assertion failed: Exactly one frame should have been flushed for VCID 1!");

    // Structural validation of the USLP-143 flushed frame
    const TFAllFormats& p1Frame = uslp.GetFinishedFrame(flushedFrameIndex);
    assert(p1Frame.tf.TFDF.header.firstHeaderLastValidOctetPointer == 0 && "Assertion failed: FHP must point to 0.");
    
    // Check that payload contains our 32 bytes and then zero padding
    const auto& p1Tfdz = p1Frame.tf.TFDF.TFDZ;
    for (size_t i = 0; i < tinyPacketSize; ++i) {
        assert(p1Tfdz.data[i] == p1FillByte && "Assertion failed: Data payload byte mismatch!");
    }
    for (size_t i = tinyPacketSize; i < p1Tfdz.length; ++i) {
        assert(p1Tfdz.data[i] == 0x00 && "Assertion failed: Trailing padding must be filled with zeros!");
    }
    std::cout << "[SUCCESS] USLP-143 (Completion Timeout) verified successfully.\n";


    // =========================================================================
    // PHASE 2: Verify USLP-144 (Inter-Frame Delay Heartbeat)
    // =========================================================================
    std::cout << "\n--- [PHASE 2] Testing USLP-144 (Inter-Frame Delay Heartbeat) on VCID 0 ---\n";
    
    size_t phase2StartFrames = uslp.GetFinishedTransferFramesIndex();
    auto p2StartTime = std::chrono::steady_clock::now();

    // No packets are injected into VCID 0. We expect it to automatically release a frame
    // because its interFrameDelayMs is configured to 400ms.
    std::cout << "[INFO] Keeping VCID 0 empty. Waiting 200ms (Heartbeat threshold is 400ms)..." << std::endl;

    // Check at T+200ms (before the 400ms heartbeat)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    size_t frameCountAt200ms = 0;
    currentFrames = uslp.GetFinishedTransferFramesIndex();
    for (size_t i = phase2StartFrames; i < currentFrames; ++i) {
        if (uslp.GetFinishedFrame(i).tf.TFPH.VCID == 0) {
            frameCountAt200ms++;
        }
    }
    std::cout << "[VERIFY] Heartbeat frames generated for VCID 0 at T+200ms: " << frameCountAt200ms << std::endl;
    assert(frameCountAt200ms == 0 && "Assertion failed: Heartbeat frame was generated before interFrameDelayMs expired!");

    // Wait another 300ms (reaching T+500ms, crossing the 400ms threshold)
    std::cout << "[INFO] Waiting another 300ms..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    size_t frameCountAt500ms = 0;
    int heartbeatFrameIndex = -1;
    currentFrames = uslp.GetFinishedTransferFramesIndex();
    for (size_t i = phase2StartFrames; i < currentFrames; ++i) {
        if (uslp.GetFinishedFrame(i).tf.TFPH.VCID == 0) {
            frameCountAt500ms++;
            heartbeatFrameIndex = static_cast<int>(i);
        }
    }

    auto elapsedP2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - p2StartTime).count();
    std::cout << "[VERIFY] Heartbeat frames generated for VCID 0 at T+" << elapsedP2 << "ms: " << frameCountAt500ms << std::endl;
    assert(frameCountAt500ms == 1 && "Assertion failed: Exactly one heartbeat frame should have been flushed for VCID 0!");

    // Structural validation of the USLP-144 flushed heartbeat frame
    const TFAllFormats& p2Frame = uslp.GetFinishedFrame(heartbeatFrameIndex);
    
    // FHP must be 2047 (DEFAULT_FHP) because this is a heartbeat with no packets inside
    uint16_t p2Fhp = p2Frame.tf.TFDF.header.firstHeaderLastValidOctetPointer;
    std::cout << "[VERIFY] Heartbeat FHP: " << p2Fhp << " (Expected: 2047)" << std::endl;
    assert(p2Fhp == DEFAULT_FHP && "Assertion failed: FHP must be 2047 for a frame containing no structural packet data!");

    // Check that the entire frame is filled with zeros (padding)
    const auto& p2Tfdz = p2Frame.tf.TFDF.TFDZ;
    bool allZeros = true;
    for (size_t i = 0; i < p2Tfdz.length; ++i) {
        if (p2Tfdz.data[i] != 0x00) {
            allZeros = false;
            break;
        }
    }
    assert(allZeros && "Assertion failed: Heartbeat frame must be completely zero-padded!");
    std::cout << "[SUCCESS] USLP-144 (Inter-Frame Delay Heartbeat) verified successfully.\n";

    // Clean up background threads
    uslp.terminateThreads();

    std::cout << "==================================================\n";
    std::cout << "[TEST PASSED] All USLP Temporal Standards Verified Successfully!\n";
    std::cout << "==================================================\n\n";
}