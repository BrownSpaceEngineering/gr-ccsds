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
    //std::cout << "Packing the FHP: " << static_cast<uint32_t>(tfdfh.firstHeaderLastValidOctetPointer) << "\n";

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

TransferFrame USLPPacker::unpackTransferFrame(const BitBuffer<MAX_TRANSFER_FRAME_LENGTH>& serializedBytes) {
    TransferFrame tf {};

    // 1. Minimum possible USLP frame size check:
    // 7 bytes (Fixed Header) + 3 bytes (TFDF Header) = 10 bytes minimum
    if (serializedBytes.length < 10) {
        std::cerr << "[ERROR] USLP Unpacker: Serialized frame is too short to parse.\n";
        return tf;
    }

    // 2. Unpack the Fixed Primary Header (First 7 bytes / 56 bits)
    uint64_t fixedHeader = 0;
    for (int i = 0; i < 7; ++i) {
        fixedHeader = (fixedHeader << 8) | serializedBytes.data[i];
    }
    
    // Shift left by 8 bits to align with the 64-bit positions of the macros
    fixedHeader = fixedHeader << 8;

    tf.TFPH.TFVN = (fixedHeader >> TFVN_POS) & 0x0F;
    tf.TFPH.SCID = (fixedHeader >> SCID_POS) & 0xFFFF;
    tf.TFPH.sourceOrDestinationID = (fixedHeader >> SRC_DST_ID_POS) & 0x01;
    tf.TFPH.VCID = (fixedHeader >> VCID_POS) & 0x3F;
    tf.TFPH.MAPID = (fixedHeader >> MAPID_POS) & 0x0F;
    tf.TFPH.endTFPrimaryHeaderFlag = (fixedHeader >> END_TF_PRIMARY_HEADER_FLAG_POS) & 0x01;
    tf.TFPH.TFLength = (fixedHeader >> TF_LENGTH_POS) & 0xFFFF;
    tf.TFPH.bypassSequenceControlFlag = (fixedHeader >> BYPASS_SEQ_CTRL_FLAG_POS) & 0x01;
    tf.TFPH.protocolCommandControlFlag = (fixedHeader >> PROTOCOL_CMD_CTRL_FLAG_POS) & 0x01;
    tf.TFPH.spare = (fixedHeader >> SPARE_POS) & 0x03;
    tf.TFPH.operationalControlFieldFlag = (fixedHeader >> OCF_FLAG_POS) & 0x01;
    tf.TFPH.VCFrameCountLength = (fixedHeader >> VC_FRAME_COUNT_LENGTH_POS) & 0x07;

    // 3. Unpack the Variable VC Frame Count (Starts at byte index 7)
    size_t vcCountLen = tf.TFPH.VCFrameCountLength;
    if (7 + vcCountLen > serializedBytes.length) {
        std::cerr << "[ERROR] USLP Unpacker: Frame truncated during VC Frame Count read.\n";
        return tf;
    }

    uint64_t frameCount = 0;
    for (size_t i = 0; i < vcCountLen; ++i) {
        frameCount = (frameCount << 8) | serializedBytes.data[7 + i];
    }
    tf.TFPH.VCFrameCount = frameCount;

    size_t primaryHeaderLength = 7 + vcCountLen;

    // 4. Unpack the Insert Zone (if present)
    size_t insertZoneLength = 0;
    if (managedParams.physical.insertZonePresent) {
        insertZoneLength = managedParams.physical.insertZoneLength;
        if (primaryHeaderLength + insertZoneLength > serializedBytes.length) {
            std::cerr << "[ERROR] USLP Unpacker: Frame truncated during Insert Zone read.\n";
            return tf;
        }
        std::copy(serializedBytes.data.begin() + primaryHeaderLength,
                  serializedBytes.data.begin() + primaryHeaderLength + insertZoneLength,
                  tf.TFIZ.TFIZData.data.begin());
        tf.TFIZ.TFIZData.length = insertZoneLength;
    }

    // 5. Calculate Boundary Offsets for the TFDF and Suffix Fields (OCF, FECF)
    size_t tfdfStart = primaryHeaderLength + insertZoneLength;
    size_t tfdfEnd = serializedBytes.length;

    // Subtract FECF (CRC) space from the back of the frame if present
    if (managedParams.physical.FECFPresent) {
        size_t fecfSize = managedParams.physical.isCRC32 ? 4 : 2;
        if (tfdfEnd > fecfSize) {
            tfdfEnd -= fecfSize;
        }
    }

    // Subtract OCF space from the back of the frame if present
    if (tf.TFPH.operationalControlFieldFlag) {
        if (tfdfEnd > 4) {
            tfdfEnd -= 4;
        }
    }

    // 6. Unpack the TFDF Header (3 bytes starting at tfdfStart)
    if (tfdfStart + 3 > tfdfEnd) {
        std::cerr << "[ERROR] USLP Unpacker: Frame truncated during TFDF Header read.\n";
        return tf;
    }

    uint32_t tfdfHeaderVal = (static_cast<uint32_t>(serializedBytes.data[tfdfStart]) << 16) |
                             (static_cast<uint32_t>(serializedBytes.data[tfdfStart + 1]) << 8) |
                             (static_cast<uint32_t>(serializedBytes.data[tfdfStart + 2]));

    tf.TFDF.header.TFDZConstructionRules = (tfdfHeaderVal >> TFDZ_CONSTRUCTION_RULES_POS) & 0x07;
    tf.TFDF.header.USLPProtocolIdentifier = (tfdfHeaderVal >> USLP_PROTOCOL_ID_POS) & 0x1F;
    tf.TFDF.header.firstHeaderLastValidOctetPointer = (tfdfHeaderVal >> FIRST_HEADER_LAST_VALID_OCTET_POS) & 0xFFFF;

    // 7. Unpack the TFDZ (Payload Data Zone)
    size_t tfdzStart = tfdfStart + 3;
    size_t tfdzEnd = tfdfEnd;

    if (tfdzStart > tfdzEnd) {
        std::cerr << "[ERROR] USLP Unpacker: TFDZ boundaries are invalid.\n";
        return tf;
    }

    size_t tfdzLength = tfdzEnd - tfdzStart;
    std::copy(serializedBytes.data.begin() + tfdzStart,
              serializedBytes.data.begin() + tfdzEnd,
              tf.TFDF.TFDZ.data.begin());
    tf.TFDF.TFDZ.length = tfdzLength;

    // Initialize empty security fields (since SDLS is assumed inactive for now)
    tf.TFDF.securityHeader.length = 0;
    tf.TFDF.securityTrailer.length = 0;

    // 8. Unpack the OCF (if present)
    if (tf.TFPH.operationalControlFieldFlag) {
        uint32_t ocfVal = (static_cast<uint32_t>(serializedBytes.data[tfdfEnd]) << 24) |
                          (static_cast<uint32_t>(serializedBytes.data[tfdfEnd + 1]) << 16) |
                          (static_cast<uint32_t>(serializedBytes.data[tfdfEnd + 2]) << 8)  |
                          (static_cast<uint32_t>(serializedBytes.data[tfdfEnd + 3]));
        tf.OCF.SDUType = (ocfVal >> 29) & 0x07;
        tf.OCF.OCFData = ocfVal & 0x1FFFFFFF;
    }

    // 9. Unpack the FECF (if present)
    if (managedParams.physical.FECFPresent) {
        size_t fecfSize = managedParams.physical.isCRC32 ? 4 : 2;
        size_t fecfStart = serializedBytes.length - fecfSize;
        std::copy(serializedBytes.data.begin() + fecfStart,
                  serializedBytes.data.begin() + serializedBytes.length,
                  tf.FECF.FECFData.data.begin());
        tf.FECF.FECFData.length = fecfSize;
    }

    return tf;
}