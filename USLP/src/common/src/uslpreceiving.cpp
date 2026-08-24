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

}

// TODO
void USLP::AllFramesReception(const BitBuffer<MAX_TRANSFER_FRAME_LENGTH>& serializedBytes, bool isError) {
    if (isError) {
        // Do something now, or save it for a later part of the processing pipeline
    }

    // If FECF is present, recompute CRC over all bytes except for the last 4
        // if does not match, throw out frame, increment a "CRC error" counter, and log it

    // (To not be implemented) extract insert data from insert zone

    // Convert serialized bytes into Transfer Frame object with accessible fields
    // Send transfer frame to virtual VirtualChannelDemultiplexing
}

void USLP::VCDemultiplexing(TransferFrame& tf) {
    // Extract VCID
    // Throw out OID frames (VCID 63)
    // Send non-OID frames and VCID to Virtual Channel Reception
}

void USLP::VCReception(TransferFrame& tf, uint8_t VCID) {
    // If gap detected between previous received frame on VC, deliver loss flag to users
    // Send TFDZ to VCPacketExtraction
}

// Every VC contains a bitbuffer of unfinished CFDP packet bytes
void USLP::VCPacketExtraction(TFDataField& TFDF, uint8_t VCID) {
    // Use fhp to extract packet data and place into VC bitbuffers. If packet finishes within transfer frame, add to separate queue for each virtual channel
    // Adds packet bytes to separate queue for each virtual channel
}