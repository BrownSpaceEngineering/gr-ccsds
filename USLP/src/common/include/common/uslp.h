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
#include <common/utils.h>

TFPrimaryHeader GetPrimaryHeader(int VCID);
TFInsertZone GetInsertZone();
TFDataField GetDataField(BitBuffer<MAX_DATA_FIELD_LENGTH> data);
OperationalControlField GetOperationalControlField();
FrameErrorControlField GetFrameErrorControlField();
// Converts higher level input data into a Transfer Frame ready for transmission
TransferFrame DataToTransferFrame(MessageType type, BitBuffer<MAX_DATA_FIELD_LENGTH> message);
// (To be implemented) Determines how many bytes of the message should be sent in the next transfer frame
uint16_t TFDataSize();
// Converts part of message into the data that will be wrapped in a transfer frame
BitBuffer<MAX_DATA_FIELD_LENGTH> GetTFDataZone(uint16_t &messagePtr, BitBuffer<MAX_MESSAGE_LENGTH> message);
// Converts higher level input data into a stream of bytes for the physical layer
BitBuffer<MAX_DATA_SIZE> DataToStream(MessageType type, BitBuffer<MAX_MESSAGE_LENGTH> message);