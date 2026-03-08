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

template <size_t Capacity> BitBuffer<Capacity> packInteger(uint64_t value, size_t numBytes);
BitBuffer<PRIMARY_HEADER_LENGTH> packPrimaryHeader(TFPrimaryHeader tfph);
BitBuffer<MAX_INSERT_ZONE_LENGTH> packInsertZone(TFInsertZone tfiz);
BitBuffer<DATA_FIELD_HEADER_LENGTH> packDataFieldHeader(TFDFHeader tfdfh);
BitBuffer<MAX_DATA_FIELD_LENGTH> packDataField(TFDataField tfdf);
BitBuffer<OCF_DATA_LENGTH> packOperationalControlField(OperationalControlField ocf);
BitBuffer<FECF_DATA_LENGTH> packFrameErrorControlField(FrameErrorControlField fecf);
BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packTransferFrame(TransferFrame tf);