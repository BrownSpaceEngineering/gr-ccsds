#pragma once

#include "binary_reader.h"
#include "binary_writer.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

enum PacketType : uint8_t {
  TELEMETRY,
  TELECOMMAND,
};

// https://ccsds.org/Pubs/133x0b2e2.pdf#%5B%7B%22num%22%3A89%2C%22gen%22%3A0%7D%2C%7B%22name%22%3A%22XYZ%22%7D%2C78%2C728%2C0%5D

class SpacePacketPrimaryHeader {
public:
  // Note(rks): If we end up being very memory constrained these can be packed
  // into 2 bytes see above
  uint8_t packet_version_number;
  PacketType packet_type;
  bool secondary_header_flag;
  uint16_t application_process_id;

  /*
     Sequence Flags (optional, can be used to indicate that packet is part of
     larger set of data)
     - 00 if continuation segment
     - 01 if first segment
     - 10 if last segment
     - 11 if unsegmented
     all values above are bit values
  */
  uint8_t sequence_flag;

  /*
     Packet Sequence Cound or Packet Name
     - Continuous (mod 16384)
     - Has to be count for telemetry (can be name for telecommand)
  */
  uint16_t packet_sequence;

  // Length for data segment of packet
  uint16_t packet_data_length;

  SpacePacketPrimaryHeader() {}

  SpacePacketPrimaryHeader(BinaryReader &reader);

  void write(BinaryWriter &writer);
};

class SpacePacketSecondaryHeader {
  // Can contain either time code, ancillary data field, or neither
  // TODO: Should probably talk to ADCS about what (if anything) we want here

public:
  SpacePacketSecondaryHeader() {}

  SpacePacketSecondaryHeader(BinaryReader &reader);

  void write(BinaryWriter &writer);
};

class SpacePacket {
public:
  SpacePacketPrimaryHeader primary_header;

  // Optional
  std::optional<SpacePacketSecondaryHeader> secondary_header;

  std::vector<uint8_t> data;

  SpacePacket() {}

  SpacePacket(const std::vector<uint8_t> &bytes);

  std::vector<uint8_t> to_bytes();
};
