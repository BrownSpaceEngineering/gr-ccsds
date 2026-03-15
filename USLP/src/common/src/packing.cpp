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
#include <common/packing.h>

template <size_t Capacity> BitBuffer<Capacity> packInteger(uint64_t value, size_t numBytes) {
    BitBuffer<Capacity> buffer;
    buffer.length = numBytes;

    for (size_t i = 0; i < numBytes; i++) {
        buffer.data[numBytes - 1 - i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }

    return buffer;
}

BitBuffer<PRIMARY_HEADER_LENGTH> packPrimaryHeader(TFPrimaryHeader tfph) {
    uint64_t packedHeader = 0;

    packedHeader |= ((uint64_t)(tfph.TFVN))                   		<< TFVN_POS;
	packedHeader |= ((uint64_t)(tfph.SCID))                   		<< SCID_POS;
    packedHeader |= ((uint64_t)(tfph.sourceOrDestinationID))  		<< SRC_DST_ID_POS;
    packedHeader |= ((uint64_t)(tfph.VCID))                   		<< VCID_POS;
    packedHeader |= ((uint64_t)(tfph.MAPID))                  		<< MAPID_POS;
    packedHeader |= ((uint64_t)(tfph.endTFPrimaryHeaderFlag)) 		<< END_TF_PRIMARY_HEADER_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.TFLength))                		<< TF_LENGTH_POS;
    packedHeader |= ((uint64_t)(tfph.bypassSequenceControlFlag)) 	<< BYPASS_SEQ_CTRL_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.protocolCommandControlFlag)) 	<< PROTOCOL_CMD_CTRL_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.spare))                  		<< SPARE_POS;
    packedHeader |= ((uint64_t)(tfph.operationalControlFieldFlag)) 	<< OCF_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.VCFrameCountLength))    		<< VC_FRAME_COUNT_LENGTH_POS;
	packedHeader |= ((uint64_t)(tfph.VCFrameCountField))    		<< VC_FRAME_COUNT_POS;
	printBytes(packedHeader);
	int numBytes = (tfph.endTFPrimaryHeaderFlag == 1) ? 4 : 8;

    return packInteger<PRIMARY_HEADER_LENGTH>(packedHeader, numBytes);
}

BitBuffer<MAX_INSERT_ZONE_LENGTH> packInsertZone(TFInsertZone tfiz) {
    return tfiz.TFIZData;
};

BitBuffer<DATA_FIELD_HEADER_LENGTH> packDataFieldHeader(TFDFHeader tfdfh) {
    uint64_t packed = 0;

    packed |= ((uint64_t)(tfdfh.TFDZConstructionRules))             << TFDZ_CONSTRUCTION_RULES_POS;
    packed |= ((uint64_t)(tfdfh.USLPProtocolIdentifier))            << USLP_PROTOCOL_ID_POS;
    packed |= ((uint64_t)(tfdfh.FirstHeaderLastValidOctetPointer))  << FIRST_HEADER_LAST_VALID_OCTET_POS;

	int numBytes = DATA_FIELD_HEADER_LENGTH;

    BitBuffer<DATA_FIELD_HEADER_LENGTH> packedDataFieldHeader = packInteger<DATA_FIELD_HEADER_LENGTH>(packed, numBytes);
    
    std::cout << packed << std::endl;
    std::cout << "packed data field header:" << std::endl;
    for (int i = 0; i < 3; i++) {
        std::bitset<8> b{packedDataFieldHeader.data[i]};
        std::cout << b << " ";
    }
    std::cout << std::endl;

    return packedDataFieldHeader;
}

BitBuffer<MAX_DATA_FIELD_LENGTH> packDataField(TFDataField tfdf) {
	BitBuffer<MAX_DATA_FIELD_LENGTH> packed;
    //std::cout << "pack header" << std::endl;
    BitBuffer<3> packedHeader = packDataFieldHeader(tfdf.header);
    size_t offset = 0;

    append(tfdf.securityHeader, packed, offset);
    append(packedHeader, packed, offset);
    append(tfdf.TFDZ, packed, offset);
    append(tfdf.securityTrailer, packed, offset);

    packed.length = offset;

    return packed;
}

BitBuffer<OCF_DATA_LENGTH> packOperationalControlField(OperationalControlField ocf) {
	uint32_t packed = 0;

    packed |= ((uint32_t)(ocf.SDUType)) << 29;
    packed |= ((uint32_t)(ocf.OCFData));
	int numBytes = OCF_DATA_LENGTH;

    return packInteger<OCF_DATA_LENGTH>(packed, numBytes);
}

BitBuffer<FECF_DATA_LENGTH> packFrameErrorControlField(FrameErrorControlField fecf) {
    return fecf.FECFData;
}

BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packTransferFrame(TransferFrame tf) {
	BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packed;
    //std::cout << "packPrimary" << std::endl;
	BitBuffer<PRIMARY_HEADER_LENGTH> packedPrimaryHeader = packPrimaryHeader(tf.TFPH);
    std::cout << "packedParimaryHeader" << std::endl;
    for (int i = 0; i < 8; i++) {
        std::bitset<8> b{packedPrimaryHeader.data[i]};
        std::cout << b << " ";
    }
    std::cout << "\n";
	BitBuffer<MAX_INSERT_ZONE_LENGTH> packedInsertZone = packInsertZone(tf.TFIZ);
    //std::cout << "packData" << std::endl;
	BitBuffer<MAX_DATA_FIELD_LENGTH> packedDataField = packDataField(tf.TFDF);
    //std::cout << "pack Op" << std::endl;
	BitBuffer<OCF_DATA_LENGTH> packedOperationalControlField = packOperationalControlField(tf.OCF);
    //std::cout << "pack Error" << std::endl;
	BitBuffer<FECF_DATA_LENGTH> packedFrameErrorControlField = packFrameErrorControlField(tf.FECF);

	size_t offset = 0;

	append(packedPrimaryHeader, packed, offset);
	append(packedInsertZone, packed, offset);
	append(packedDataField, packed, offset);
	append(packedOperationalControlField, packed, offset);
	append(packedFrameErrorControlField, packed, offset);

	packed.length = offset;

	return packed;
}