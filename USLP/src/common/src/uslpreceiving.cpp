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

// Reads from reception queue, processes frame if present
void USLP::AllFramesReceptionThread() {
    constexpr auto TICK_RATE = std::chrono::milliseconds(20);
    
    while (m_running) {
        BitBuffer<MAX_TRANSFER_FRAME_LENGTH> rawFrame;
        
        // Non-blocking pop with timeout to allow thread to exit smoothly on shutdown
        if (m_receptionQueue.pop(rawFrame)) {
            cout << "found frame being received\n";
            // Pass the frame to the AllFramesReception decoder
            AllFramesReception(rawFrame, false);
        } else {
            std::this_thread::sleep_for(TICK_RATE);
        }
    }
}

void USLP::AllFramesReception(const BitBuffer<MAX_TRANSFER_FRAME_LENGTH>& serializedBytes, bool isError) {
    if (isError) {
        std::cerr << "[ERROR] USLP AllFramesReception: Lower layer flagged a transmission error. Discarding.\n";
        return;
    }

    // If FECF is present, recompute CRC over all bytes except for the last 4 (or 2)
    if (managedParams.physical.FECFPresent) {
        size_t fecfSize = managedParams.physical.isCRC32 ? 4 : 2;
        
        if (serializedBytes.length <= fecfSize) {
            std::cerr << "[ERROR] USLP: Serialized frame is too short to contain FECF.\n";
            m_crcErrorCount++;
            return;
        }

        size_t crcInputLength = serializedBytes.length - fecfSize;
        
        // Recompute the CRC checksum over the header, insert zone, and payload
        uint32_t computedCRC = ComputeCRC(serializedBytes.data.data(), crcInputLength, managedParams.physical.isCRC32);
        
        // Extract the transmitted CRC from the end of the frame
        uint32_t receivedCRC = 0;

        for (size_t i = 0; i < fecfSize; ++i) {
            receivedCRC = (receivedCRC << 8) | static_cast<uint32_t>(serializedBytes.data[crcInputLength + i]);
        }

        // If it does not match, throw out the frame, increment a "CRC error" counter, and log it
        if (computedCRC != receivedCRC) {
            m_crcErrorCount++;
            std::cerr << "[ERROR] USLP: CRC Mismatch (Computed: 0x" << std::hex << computedCRC 
                      << ", Received: 0x" << receivedCRC << std::dec << "). Frame discarded.\n";
            return;
        } else {
            cout << "CRC PASSES!\n";
        }
    }

    // (To not be implemented, leave as comment) extract insert data from insert zone if present
    /*
    if (managedParams.physical.insertZonePresent) {
        // Extract insert zone data...
    }
    */

    // Convert serialized bytes into Transfer Frame object with accessible fields
    cout << endl;
    cout << "---- PRINTING FIRST BYTES OF TRANSFER FRAME ----" << endl;

    for (int i = 0; i < 8; i++) {
        cout << static_cast<int>(serializedBytes.data[i]) << " ";
    }

    cout << endl;
    cout << endl;

    TransferFrame tf = packer.unpackTransferFrame(serializedBytes);
    cout << "---- RECEIVE SIDE ----" << endl;
    PrintPrimaryHeader(tf.TFPH);

    // Send transfer frame to Virtual Channel Demultiplexing
    VCDemultiplexing(tf);
}

void USLP::VCDemultiplexing(TransferFrame& tf) {
    // Extract VCID
    uint8_t vcid = tf.TFPH.VCID;

    // Throw out OID frames (VCID 63)
    if (vcid == IDLE_VCID) {
        return;
    }

    // Send non-OID frames and VCID to Virtual Channel Reception
    VCReception(tf, vcid);
}

void USLP::VCReception(TransferFrame& tf, uint8_t VCID) {
    int8_t vcidIndex = GetChannelByVCID(VCID, m_vcidToIndex);
    if (vcidIndex < 0) {
        std::cerr << "[ERROR] USLP: Received frame on unmapped VCID: " << static_cast<int>(VCID) << "\n";
        return;
    }

    RxVirtualChannelState& rxState = m_rxVirtualChannels[vcidIndex];
    uint64_t currentFrameCount = tf.TFPH.VCFrameCount;

    // If gap detected between previous received frame on VC, deliver loss flag to users
    if (rxState.firstFrameReceived) {
        uint64_t expectedFrameCount = (rxState.lastFrameCount + 1) & m_virtualChannels[vcidIndex].vcFrameCountMask;
        if (currentFrameCount != expectedFrameCount) {
            std::cerr << "[WARNING] USLP VCReception: Frame loss detected on VCID " << static_cast<int>(VCID)
                      << ". Expected Frame Count: " << expectedFrameCount 
                      << ", Received: " << currentFrameCount << "\n";
            
            // Clear the incomplete packet assembler to prevent parsing corrupted packets across the gap
            rxState.packetAssemblerBuffer.clear();
        }
    } else {
        rxState.firstFrameReceived = true;
    }

    rxState.lastFrameCount = currentFrameCount;

    // Send TFDZ to VCPacketExtraction
    VCPacketExtraction(tf.TFDF, VCID);
}

// Every VC contains a bitbuffer of unfinished packet bytes
void USLP::VCPacketExtraction(TFDataField& TFDF, uint8_t VCID) {
    int8_t vcidIndex = GetChannelByVCID(VCID, m_vcidToIndex);
    if (vcidIndex < 0) return;

    RxVirtualChannelState& rxState = m_rxVirtualChannels[vcidIndex];
    const uint8_t* tfdz_data = &TFDF.TFDZ.data[0];
    size_t tfdz_len = TFDF.TFDZ.length;

    uint16_t fhp = TFDF.header.firstHeaderLastValidOctetPointer;
    size_t idx = 0;

    // --- PHASE 1: Handle Spillover Data from a Previous Frame ---
    if (rxState.packetAssemblerBuffer.length > 0) {
        // If a new packet starts at 'fhp', then the spillover data ends at 'fhp'.
        // If FHP is '65535' (no new packet starts), then the entire TFDZ belongs to the spillover packet.
        size_t bytesToAppend = (fhp != DEFAULT_FHP) ? fhp : tfdz_len;

        // If there's no new packet starting, strip any trailing EIP (0xE0) padding bytes before appending
        if (fhp == DEFAULT_FHP) {
            while (bytesToAppend > 0 && tfdz_data[bytesToAppend - 1] == 0xE0) {
                bytesToAppend--;
            }
        }

        if (bytesToAppend > 0) {
            rxState.packetAssemblerBuffer.insertAtEnd(&tfdz_data[0], bytesToAppend);
            idx += bytesToAppend;
        }

        // Check if the accumulated packet has reached its expected length
        if (rxState.packetAssemblerBuffer.length >= 6) {
            // CCSDS Space Packet length field is big-endian at bytes 4 and 5
            uint16_t lengthField = (static_cast<uint16_t>(rxState.packetAssemblerBuffer.data[4]) << 8) |
                                   (static_cast<uint16_t>(rxState.packetAssemblerBuffer.data[5]));
            size_t totalExpectedLength = lengthField + 7; // CCSDS length field = (Total Length) - 7

            if (rxState.packetAssemblerBuffer.length >= totalExpectedLength) {
                // Adds finished packet to separate queue for each virtual channel
                rxState.completedPacketsQueue.push(rxState.packetAssemblerBuffer);
                std::cout << "[INFO] USLP VCP: Fully assembled spillover packet of size " 
                          << rxState.packetAssemblerBuffer.length << " bytes on VCID " << static_cast<int>(VCID) << "\n";
                
                rxState.packetAssemblerBuffer.clear();
            }
        }
    }

    // --- PHASE 2: Parse New Packet Headers starting at FHP ---
    if (fhp != DEFAULT_FHP && fhp < tfdz_len) {
        idx = fhp;

        // Chain-parse all subsequent packets in the frame
        while (idx < tfdz_len) {
            // If we hit standard 1-octet Encapsulation Idle Packet (0xE0) padding, stop parsing
            if (tfdz_data[idx] == 0xE0) {
                break;
            }

            // Ensure we have enough bytes remaining in the TFDZ to read at least the 6-byte header
            if (idx + 6 <= tfdz_len) {
                uint16_t lengthField = (static_cast<uint16_t>(tfdz_data[idx + 4]) << 8) |
                                       (static_cast<uint16_t>(tfdz_data[idx + 5]));
                size_t totalExpectedLength = lengthField + 7;

                if (idx + totalExpectedLength <= tfdz_len) {
                    // The entire packet fits within this frame's TFDZ
                    BitBuffer<MAX_MESSAGE_LENGTH> completedPacket(&tfdz_data[idx], totalExpectedLength);
                    
                    // Adds finished packet to separate queue for each virtual channel
                    rxState.completedPacketsQueue.push(completedPacket);
                    std::cout << "[INFO] USLP VCP: Fully assembled packet of size " 
                              << totalExpectedLength << " bytes on VCID " << static_cast<int>(VCID) << "\n";
                    
                    idx += totalExpectedLength;
                } else {
                    // The packet is cut off and spills over into the next frame
                    size_t partialLength = tfdz_len - idx;
                    rxState.packetAssemblerBuffer.insertAtEnd(&tfdz_data[idx], partialLength);
                    idx += partialLength;
                }
            } else {
                // Not enough bytes remaining to read the header; save the fragment for the next frame
                size_t partialLength = tfdz_len - idx;
                rxState.packetAssemblerBuffer.insertAtEnd(&tfdz_data[idx], partialLength);
                idx += partialLength;
            }
        }
    }
}