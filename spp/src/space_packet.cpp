#include "space_packet.h"
#include "binary_reader.h"

#include <cstdint>
#include <vector>

SpacePacketPrimaryHeader::SpacePacketPrimaryHeader(BinaryReader &reader) {
  uint16_t metadata_bytes = reader.read_u16();
  packet_version_number = (uint8_t)(metadata_bytes >> 13);
  packet_type = static_cast<PacketType>((metadata_bytes >> 12) & 0x1);
  secondary_header_flag = ((metadata_bytes >> 11) & 0x1);
  application_process_id = metadata_bytes & 0x7FF;

  uint16_t sequence_bytes = reader.read_u16();
  sequence_flag = (uint8_t)(sequence_bytes >> 14);
  packet_sequence = sequence_bytes & 0x3FFF;

  packet_data_length = reader.read_u16();
}

std::vector<uint8_t> SpacePacketPrimaryHeader::to_bytes() { return {}; }

SpacePacketSecondaryHeader::SpacePacketSecondaryHeader(BinaryReader &reader) {}

std::vector<uint8_t> SpacePacketSecondaryHeader::to_bytes() { return {}; }

SpacePacket::SpacePacket(const std::vector<uint8_t> &bytes) {
  BinaryReader reader(bytes);
  this->primary_header = SpacePacketPrimaryHeader(reader);
  this->secondary_header = SpacePacketSecondaryHeader(reader);
}

std::vector<uint8_t> SpacePacket::to_bytes() { return {}; }
