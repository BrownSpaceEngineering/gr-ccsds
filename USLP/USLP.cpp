#include <stdint.h> 
#include <iostream>
#include <bitset>
#include <vector>

// Transfer Frame Data Sizes
#define SECURITY_HEADER_LENGTH 				6
#define SECURITY_TRAILER_LENGTH 			16
#define OCF_DATA_LENGTH 					4
#define FECF_DATA_LENGTH 					4

// Transfer Frame Primary Header field bit positions
#define TFVN_POS                        	0    // 4 bits
#define SCID_POS                        	4    // 16 bits
#define SRC_DST_ID_POS                  	20   // 1 bit
#define VCID_POS                        	21   // 6 bits
#define MAPID_POS                       	27   // 4 bits
#define END_TF_PRIMARY_HEADER_FLAG_POS  	31   // 1 bit
#define TF_LENGTH_POS                   	32   // 16 bits
#define BYPASS_SEQ_CTRL_FLAG_POS        	48   // 1 bit
#define PROTOCOL_CMD_CTRL_FLAG_POS      	49   // 1 bit
#define SPARE_POS                       	50   // 2 bits
#define OCF_FLAG_POS                    	52   // 1 bit
#define VC_FRAME_COUNT_LENGTH_POS       	53   // 3 bits
#define VC_FRAME_COUNT_POS        			56   // ???? bits
// NOTE THAT WE ARE NOT CURRENTLY SET UP TO CONTAIN MORE THAN 255 FRAMES IN A VC

// TF Primary Header structure
#define TFDZ_CONSTRUCTION_RULES_POS			0  // 3 bits
#define USLP_PROTOCOL_ID_POS				3  // 5 bits
#define FIRST_HEADER_LAST_VALID_OCTET_POS	8  // 16 bits

struct TFPrimaryHeader {
	uint16_t TFVN = 4;
	uint16_t SCID;
	bool sourceOrDestinationID; //bool
	uint16_t VCID;
	uint16_t MAPID;
	bool endTFPrimaryHeaderFlag; //bool
	uint16_t TFLength;
	bool bypassSequenceControlFlag; //bool
	bool protocolCommandControlFlag; //bool
	uint16_t spare;
	bool operationalControlFieldFlag; //bool
	uint16_t VCFrameCountLength;
	uint32_t VCFrameCountField;
};

//Complete black box
struct TFInsertZone {
	std::vector<uint8_t> TFIZData;
};

struct TFDFHeader {
	uint16_t TFDZConstructionRules;
	uint16_t USLPProtocolIdentifier;
	uint16_t FirstHeaderLastValidOctetPointer; //Optional
};

struct TFDataField {
	uint8_t securityHeader[SECURITY_HEADER_LENGTH];
	TFDFHeader header;
	std::vector<uint8_t> TFDZ; //ALL THE DATA YOU WANT TO SEND GOES HERE
	uint8_t securityTrailer[SECURITY_TRAILER_LENGTH];
};

struct OperationalControlField {
	uint8_t OCFData[OCF_DATA_LENGTH]; //idk
};

struct FrameErrorControlField {
	uint8_t FECFData[4]; //Generated using CRC
};

struct TransferFrame {
	TFPrimaryHeader TFPH;
	TFInsertZone TFIZ; //tf is this
	TFDataField TFDF;
	OperationalControlField OCF;
	FrameErrorControlField FECF;
};

void appendVector(std::vector<uint8_t>& dest, const std::vector<uint8_t>& src) {
    dest.insert(dest.end(), src.begin(), src.end());
}

/*void packArray(uint8_t arr[], std::vector<uint8_t> packedData) {
	for (int i = 0; i < sizeof(arr); i++) {
		packedData.push_back(arr[i]);
	}
}*/

std::vector<uint8_t> packPrimaryHeader(TFPrimaryHeader tfph) {
    uint64_t packedHeader = 0;

    packedHeader |= ((uint64_t)(tfph.TFVN))                   << TFVN_POS;
    packedHeader |= ((uint64_t)(tfph.SCID))                   << SCID_POS;
    packedHeader |= ((uint64_t)(tfph.sourceOrDestinationID))  << SRC_DST_ID_POS;
    packedHeader |= ((uint64_t)(tfph.VCID))                   << VCID_POS;
    packedHeader |= ((uint64_t)(tfph.MAPID))                  << MAPID_POS;
    packedHeader |= ((uint64_t)(tfph.endTFPrimaryHeaderFlag)) << END_TF_PRIMARY_HEADER_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.TFLength))               << TF_LENGTH_POS;
    packedHeader |= ((uint64_t)(tfph.bypassSequenceControlFlag)) << BYPASS_SEQ_CTRL_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.protocolCommandControlFlag)) << PROTOCOL_CMD_CTRL_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.spare))                  << SPARE_POS;
    packedHeader |= ((uint64_t)(tfph.operationalControlFieldFlag)) << OCF_FLAG_POS;
    packedHeader |= ((uint64_t)(tfph.VCFrameCountLength))    << VC_FRAME_COUNT_LENGTH_POS;
	packedHeader |= ((uint64_t)(tfph.VCFrameCountField))    << VC_FRAME_COUNT_POS;

	std::vector<uint8_t> packedHeaderVector;

	for (int i = 0; i < 8; i++) {
		packedHeaderVector.push_back(uint8_t(packedHeader & 0xFF));
		packedHeader >>= 8;
	}

    return packedHeaderVector;
}

std::vector<uint8_t> packInsertZone(TFInsertZone tfiz) {
    return tfiz.TFIZData;
};

std::vector<uint8_t> packDataFieldHeader(TFDFHeader tfdfh) {
    uint32_t packedHeader = 0;

    packedHeader |= ((uint32_t)(tfdfh.TFDZConstructionRules))             << TFDZ_CONSTRUCTION_RULES_POS;
    packedHeader |= ((uint32_t)(tfdfh.USLPProtocolIdentifier))            << USLP_PROTOCOL_ID_POS;
    packedHeader |= ((uint32_t)(tfdfh.FirstHeaderLastValidOctetPointer))  << FIRST_HEADER_LAST_VALID_OCTET_POS;

	std::vector<uint8_t> packedHeaderVector;

	for (int i = 0; i < 3; i++) {
		packedHeaderVector.push_back(uint8_t(packedHeader & 0xFF));
		packedHeader >>= 8;
	}

    return packedHeaderVector;
}

std::vector<uint8_t> packDataField(TFDataField tfdf) {
    const int TFDZLength = tfdf.TFDZ.size();
	const int headerLength = 3;
	std::vector<uint8_t> packedDataField;
	
	for (int i = 0; i < SECURITY_HEADER_LENGTH; i++) {
		packedDataField.push_back(tfdf.securityHeader[i]);
	}
	
	std::vector<uint8_t> packedHeader = packDataFieldHeader(tfdf.header);
	appendVector(packedDataField, packedHeader);
	appendVector(packedDataField, tfdf.TFDZ);

	for (int i = 0; i < SECURITY_TRAILER_LENGTH; i++) {
		packedDataField.push_back(tfdf.securityTrailer[i]);
	}

    return packedDataField;
}

std::vector<uint8_t> packOperationalControlField(OperationalControlField ocf) {
	std::vector<uint8_t> packedOperationalControlField;
	
	for (int i = 0; i < OCF_DATA_LENGTH; i++) {
		packedOperationalControlField.push_back(ocf.OCFData[i]);
	}

    return packedOperationalControlField;
}

std::vector<uint8_t> packFrameErrorControlField(FrameErrorControlField fecf) {
	std::vector<uint8_t> packedOperationalControlField;
	
	for (int i = 0; i < FECF_DATA_LENGTH; i++) {
		packedOperationalControlField.push_back(fecf.FECFData[i]);
	}

    return packedOperationalControlField;
}

std::vector<uint8_t> packTransferFrame(TransferFrame tf) {
	std::vector<uint8_t> packedTransferFrame;

	std::vector<uint8_t> packedPrimaryHeader = packPrimaryHeader(tf.TFPH);
	std::vector<uint8_t> packedInsertZone = packInsertZone(tf.TFIZ);
	std::vector<uint8_t> packedDataField = packDataField(tf.TFDF);
	std::vector<uint8_t> packedOperationalControlField = packOperationalControlField(tf.OCF);
	std::vector<uint8_t> packedFrameErrorControlField = packFrameErrorControlField(tf.FECF);
	appendVector(packedTransferFrame, packedPrimaryHeader);
	appendVector(packedTransferFrame, packedInsertZone);
	appendVector(packedTransferFrame, packedDataField);
	appendVector(packedTransferFrame, packedOperationalControlField);
	appendVector(packedTransferFrame, packedFrameErrorControlField);

	return packedTransferFrame;
}

int main(int argc, char* argv[]) {
	// Example usage
	TFPrimaryHeader tfph;
	tfph.TFVN = 4;
	tfph.SCID = 2;
	tfph.sourceOrDestinationID = 1;
	tfph.VCID = 2;
	tfph.MAPID = 0;
	tfph.endTFPrimaryHeaderFlag = true;
	tfph.TFLength = 512;
	tfph.bypassSequenceControlFlag = false;
	tfph.protocolCommandControlFlag = false;
	tfph.spare = 0b00;
	tfph.operationalControlFieldFlag = true;
	tfph.VCFrameCountLength = 2;
	tfph.VCFrameCountField = 5;

	std::vector<uint8_t> packedHeader = packPrimaryHeader(tfph);
	//std::cout << "Packed Header: " << std::bitset<64>(packedHeader) << std::endl;

	return 0;
}

/*
class GlobalData {
	SCID; //Constant we decide on when the mission launches
	TFVN = 4; //Just carries the current version
	sourceOrDestinationID = 1; //0 is more important for multi-recipient systems
	MAPID = 0; //We do not need MAP, not so many pieces of data to transfer
	TFDFSize; //To be decided later (measured in octets)

	Def GetTransferFrame(
VCID, 
endTFPrimaryHeaderFlag, 
TFLength, 
bypassSequenceControlFlag,
protocolCommandControlFlag) 
{
	TFPrimaryHeader tfph;
	tfph.TFVN = this.TFVN
	tfph.SCID = this.SCID
	tfph.sourceOrDestinationID = this.sourceOrDestinationID;
    tfph.VCID = VCID
    tfph.MAPID = this.MAPID
    tfph.endTFPrimaryHeaderFlag = endTFPrimaryHeaderFlag
	tfph.TFLength = TFLength
	tfph.bypassSequenceControlFlag = bypassSequenceControlFlag
	tfph.protocolCommandControlFlag = protocolCommandControlFlag
	tfph.spare = 0b00; //Decide what to do with these later on
	tfph.operationalControlFieldFlag = True; //Seems like a good safeguard to have
	tfph.VCFrameCountLength = VCFrameCountLength;
	tfph.VCFrameCountField = VCFrameCountField;

	
}

Def SendCommand(enum type, data) {

	VCID = parseType(); //either 1 or 2 depending on command or bitmap
	protocolCommandControlFlag = False //Until we work on COP
		VCFrameCountLength = TBD;
		VCFrameCountField = TBD;

	TransferFrame TF = GetTransferFrame(
VCID, 
endTFPrimaryHeaderFlag, 
TFLength, 
protocolCommandControlFlag,
VCFrameCountLength,
VCFrameCountField);


}

}
*/
