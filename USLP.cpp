/*
Struct TFPrimaryHeader {
	TFVN = 4;
	SCID;
	sourceOrDestinationID; //bool
	VCID;
	MAPID;
	endTFPrimaryHeaderFlag; //bool
	TFLength;
	bypassSequenceControlFlag; //bool
	protocolCommandControlFlag; //bool
	spare;
	operationalControlFieldFlag; //bool
	VCFrameCountLength;
	VCFrameCountField;
}


Struct TFInsertZone {
//Complete black box
}

Struct TFDFHeader {
	TFDZConstructionRules;
	USLPProtocolIdentifier;
	FirstHeaderLastValidOctetPointer; //Optional
}

Struct TFDataField {
	securityHeader;
	TFDFHeader header;
	TFDZ; //ALL THE DATA YOU WANT TO SEND GOES HERE
	securityTrailer;
}

Struct OperationalControlField {
	SDUtype; //3 bits
	OCFData; //idk
}

Struct FrameErrorControlField {
	FECFData; //idk
}

Struct TransferFrame {
	TFPrimaryHeader TFPH;
	TFInsertZone TFIZ; //tf is this
	TFDataField TFDF;
	OperationalControlField OCF;
}

Class GlobalData {
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