#include "space_packet.h"

#include <cstdint>
#include <optional>
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

void SpacePacketPrimaryHeader::write(BinaryWriter &writer) {
  uint16_t metadata_bytes = (this->packet_version_number << 13) |
                            (this->packet_type << 12) |
                            (this->secondary_header_flag << 11) |
                            (this->application_process_id & 0x7FF);

  // Note(rks): We should never run into this but I think the C++ integral
  // promotion rules mean that on an embedded system not having the static cast
  // could cause an overflow
  uint16_t sequence_bytes = (static_cast<uint16_t>(this->sequence_flag) << 14) |
                            (this->packet_sequence & 0x3FFF);

  writer.write_u16(metadata_bytes);
  writer.write_u16(sequence_bytes);
  writer.write_u16(this->packet_data_length);
}

SpacePacketSecondaryHeader::SpacePacketSecondaryHeader(BinaryReader &reader) {
  // Supress unused var warnings
  (void)reader;
}

void SpacePacketSecondaryHeader::write(BinaryWriter &writer) {
  // Supress unused var warnings
  (void)writer;
}

SpacePacket::SpacePacket(const std::vector<uint8_t> &bytes) {
  BinaryReader reader(bytes);
  this->primary_header = SpacePacketPrimaryHeader(reader);
  if (this->primary_header.secondary_header_flag) {
    this->secondary_header = SpacePacketSecondaryHeader(reader);
  } else {
    this->secondary_header = std::nullopt;
  }

  for (int i = 0; i < this->primary_header.packet_data_length + 1; i++) {
    data.push_back(reader.read_u8());
  }
}

std::vector<uint8_t> SpacePacket::to_bytes() {
  std::vector<uint8_t> bytes;
  BinaryWriter writer(bytes);

  this->primary_header.write(writer);
  if (this->secondary_header) {
    this->secondary_header.value().write(writer);
  }

  for (const auto &byte : this->data) {
    writer.write_u8(byte);
  }

  return bytes;
};
