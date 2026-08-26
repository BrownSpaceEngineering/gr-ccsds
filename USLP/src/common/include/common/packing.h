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
#include <common/uslpstructs.h>
#include <common/utils.h>

class USLPPacker {
public:
    USLPPacker(USLPConfig& config, std::array<int8_t, MAX_VC_COUNT>& vcidMap)
        : managedParams(config), m_vcidToIndex(vcidMap) {
            packedAsm.insert(0, CCSDS_ASM, ASM_LENGTH);
        }

    template <size_t Capacity> BitBuffer<Capacity> packInteger(uint64_t value, size_t numBytes);
    BitBuffer<PRIMARY_HEADER_LENGTH> packPrimaryHeader(TFPrimaryHeader tfph);
    BitBuffer<MAX_INSERT_ZONE_LENGTH> packInsertZone(TFInsertZone tfiz);
    BitBuffer<DATA_FIELD_HEADER_LENGTH> packDataFieldHeader(TFDFHeader tfdfh);
    BitBuffer<MAX_DATA_FIELD_LENGTH> packDataField(TFDataField& tfdf);
    BitBuffer<OCF_DATA_LENGTH> packOperationalControlField(OperationalControlField ocf, uint8_t VCID);
    BitBuffer<FECF_DATA_LENGTH> packFrameErrorControlField(FrameErrorControlField fecf);
    BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packTransferFrame(TransferFrame tf);

    TransferFrame unpackTransferFrame(const BitBuffer<MAX_TRANSFER_FRAME_LENGTH>& serializedBytes);
private:
    USLPConfig& managedParams;
    std::array<int8_t, MAX_VC_COUNT> &m_vcidToIndex;
    const uint8_t CCSDS_ASM[4] = {0x1A, 0xCF, 0xFC, 0x1D};
    BitBuffer<ASM_LENGTH> packedAsm;
};