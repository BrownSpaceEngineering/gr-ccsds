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
#include <common/data.h>
#include <common/packing.h>
#include <common/utils.h>
#include <common/uslpstructs.h>
#include <string>
#include <set>


class USLP {
public:
    USLP(uint8_t numVirtualChannels);

    TFPrimaryHeader GetPrimaryHeader(uint8_t VCID);
    TFInsertZone GetInsertZone();
    BitBuffer<MAX_SECURITY_HEADER_LENGTH> GetSecurityHeader();
    BitBuffer<MAX_SECURITY_TRAILER_LENGTH> GetSecurityTrailer();
    TFDataField GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data);
    OperationalControlField GetOperationalControlField(USLPContext context);
    FrameErrorControlField GetFrameErrorControlField();
    TransferFrame DataToTransferFrame(
        uint8_t VCID, 
        BitBuffer<MAX_DATA_FIELD_LENGTH> message,
        USLPContext context); // Converts higher level input data into a Transfer Frame ready for transmission
    uint16_t TFDataSize(uint8_t vcid); // Determines how many bytes of the message should be sent in the next transfer frame
    BitBuffer<MAX_DATA_FIELD_LENGTH> GetTFDataZone(
        uint16_t &messagePtr, 
        BitBuffer<MAX_MESSAGE_LENGTH> message, 
        USLPContext context); // Converts part of message into the data that will be wrapped in a transfer frame
    BitBuffer<MAX_DATA_SIZE> DataToStream(MessageType type, BitBuffer<MAX_MESSAGE_LENGTH> message); // Converts higher level input data into a stream of bytes for the physical layer

private:
    USLPPacker packer;
    USLPConfig managedParams;
    std::array<uint16_t, 64> maxDataZoneLengths; // One per VC
};
