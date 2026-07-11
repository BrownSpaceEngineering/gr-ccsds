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

template <size_t Capacity> BitBuffer<Capacity> USLPPacker::packInteger(uint64_t value, size_t numBytes) {
    BitBuffer<Capacity> buffer;
    buffer.length = numBytes;

    for (size_t i = 0; i < numBytes; i++) {
        buffer.data[numBytes - 1 - i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }

    return buffer;
}

BitBuffer<PRIMARY_HEADER_LENGTH> USLPPacker::packPrimaryHeader(TFPrimaryHeader tfph) {
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
	packedHeader |= ((uint64_t)(tfph.VCFrameCount))    		        << VC_FRAME_COUNT_POS;
	//printBytes(packedHeader);
	int numBytes = (tfph.endTFPrimaryHeaderFlag == 1) ? 4 : 8;

    return packInteger<PRIMARY_HEADER_LENGTH>(packedHeader, numBytes);
}

BitBuffer<MAX_INSERT_ZONE_LENGTH> USLPPacker::packInsertZone(TFInsertZone tfiz) {
    if (managedParams.physical.insertZonePresent) {
        return tfiz.TFIZData;
    } else {
        BitBuffer<MAX_INSERT_ZONE_LENGTH> TFIZData {};

        return TFIZData;
    }
};

BitBuffer<DATA_FIELD_HEADER_LENGTH> USLPPacker::packDataFieldHeader(TFDFHeader tfdfh) {
    uint64_t packed = 0;

    packed |= ((uint64_t)(tfdfh.TFDZConstructionRules))             << TFDZ_CONSTRUCTION_RULES_POS;
    packed |= ((uint64_t)(tfdfh.USLPProtocolIdentifier))            << USLP_PROTOCOL_ID_POS;
    packed |= ((uint64_t)(tfdfh.firstHeaderLastValidOctetPointer))  << FIRST_HEADER_LAST_VALID_OCTET_POS;

	int numBytes = DATA_FIELD_HEADER_LENGTH;

    BitBuffer<DATA_FIELD_HEADER_LENGTH> packedDataFieldHeader = USLPPacker::packInteger<DATA_FIELD_HEADER_LENGTH>(packed, numBytes);
    
    //std::cout << packed << std::endl;
    //std::cout << "packed data field header:" << std::endl;
    for (int i = 0; i < 3; i++) {
        std::bitset<8> b{packedDataFieldHeader.data[i]};
        //std::cout << b << " ";
    }
    //std::cout << std::endl;

    return packedDataFieldHeader;
}

BitBuffer<MAX_DATA_FIELD_LENGTH> USLPPacker::packDataField(TFDataField& tfdf) {
	BitBuffer<MAX_DATA_FIELD_LENGTH> packed;
    //std::cout << "pack header" << std::endl;
    BitBuffer<3> packedHeader = packDataFieldHeader(tfdf.header);
    size_t offset = 0;
    //std::cout << "packDataField last payload byte: " << static_cast<uint32_t>(tfdf.TFDZ.data[tfdf.TFDZ.length - 1]) << "\n";

    append(tfdf.securityHeader, packed, offset);
    append(packedHeader, packed, offset);
    append(tfdf.TFDZ, packed, offset);
    append(tfdf.securityTrailer, packed, offset);

    packed.length = offset;

    //std::cout << "packed packDataField last payload byte: " << static_cast<uint32_t>(packed.data[packed.length - 1]) << "\n";

    return packed;
}

BitBuffer<OCF_DATA_LENGTH> USLPPacker::packOperationalControlField(OperationalControlField ocf, uint8_t VCID) {
	int index = GetChannelByVCID(VCID, m_vcidToIndex);
    //std::cout << "\n";
    //std::cout << "VCID: " << static_cast<uint32_t>(VCID) << "\n";
    //std::cout << "index" << index;
    
    if ((index != -1) && (managedParams.virtualChannelConfigs[index].COPInEffect != USLPConfig::COPType::NONE)) {
        uint32_t packed = 0;
        std::cout << "WHY ARE WE HERE\n";

        packed |= ((uint32_t)(ocf.SDUType)) << 29;
        packed |= ((uint32_t)(ocf.OCFData));
        int numBytes = OCF_DATA_LENGTH;

        return packInteger<OCF_DATA_LENGTH>(packed, numBytes);
    } else {
        BitBuffer<OCF_DATA_LENGTH> OCFData {};

        return OCFData;
    }
}

BitBuffer<FECF_DATA_LENGTH> USLPPacker::packFrameErrorControlField(FrameErrorControlField fecf) {
    if (managedParams.physical.FECFPresent) {
        return fecf.FECFData;
    } else {
        BitBuffer<FECF_DATA_LENGTH> FECFData {};

        return FECFData;
    }
}

BitBuffer<MAX_TRANSFER_FRAME_LENGTH> USLPPacker::packTransferFrame(TransferFrame tf) {
    BitBuffer<MAX_TRANSFER_FRAME_LENGTH> packed;
    //std::cout << "packPrimary" << std::endl;
	BitBuffer<PRIMARY_HEADER_LENGTH> packedPrimaryHeader = packPrimaryHeader(tf.TFPH);
    
    //std::cout << "\n";
	BitBuffer<MAX_INSERT_ZONE_LENGTH> packedInsertZone = packInsertZone(tf.TFIZ);
	BitBuffer<MAX_DATA_FIELD_LENGTH> packedDataField = packDataField(tf.TFDF);
    std::cout << "Data Field last byte: " << static_cast<uint32_t>(tf.TFDF.TFDZ.data[tf.TFDF.TFDZ.length - 1]) << "\n";
    //std::cout << tf.TFDF.TFDZ.length;
	BitBuffer<OCF_DATA_LENGTH> packedOperationalControlField = packOperationalControlField(tf.OCF, tf.TFPH.VCID);
	BitBuffer<FECF_DATA_LENGTH> packedFrameErrorControlField = packFrameErrorControlField(tf.FECF);

	size_t offset = 0;

	append(packedPrimaryHeader, packed, offset);
	append(packedInsertZone, packed, offset);
	append(packedDataField, packed, offset);
	append(packedOperationalControlField, packed, offset);
	append(packedFrameErrorControlField, packed, offset);
    if (tf.TFPH.VCID == 63) {
	}

	packed.length = offset;

    //std::cout << "Final packed bytes: " << packed.length << "\n";
    for (int i = 0; i < 4; i++) {
        //std::cout << static_cast<int>(packed.data[packed.length - 1 - i]) << "\n";
    }

	return packed;
}