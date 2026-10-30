#include "binary_reader.h"
#include "binary_writer.h"
#include "space_packet.h"

#include <cstdint>
#include <iostream>
#include <vector>

bool check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool test_binary_io() {
  std::vector<uint8_t> bytes;
  BinaryWriter writer(bytes);
  writer.write_u8(0x12);
  writer.write_u16(0x3456);

  bool ok = check(bytes == std::vector<uint8_t>{0x12, 0x34, 0x56},
                  "BinaryWriter smoke test failed");
  BinaryReader reader(bytes);
  ok &= check(reader.read_u8() == 0x12 && reader.read_u16() == 0x3456,
              "BinaryReader smoke test failed");
  return ok;
}

// Port of spp-rs::space_packet::tests::test_parse_space_packet.
bool test_parse_space_packet() {
  // The length field is the two-byte Packet Data Field length minus one.
  const std::vector<uint8_t> bytes{0xF1, 0x23, 0xDF, 0xFF,
                                   0x00, 0x01, 0xDE, 0xAD};
  const SpacePacket packet(bytes);
  const auto &header = packet.primary_header;

  bool ok = true;
  ok &= check(header.packet_version_number == 7, "Incorrect packet version");
  ok &= check(header.packet_type == TELECOMMAND, "Incorrect packet type");
  ok &= check(!header.secondary_header_flag, "Incorrect secondary header flag");
  ok &=
      check(header.application_process_id == 0x123, "Incorrect application ID");
  ok &= check(header.sequence_flag == 3, "Incorrect sequence flag");
  ok &= check(header.packet_sequence == 0x1FFF, "Incorrect packet sequence");
  ok &= check(header.packet_data_length == 1, "Incorrect packet data length");
  ok &= check(!packet.secondary_header, "Unexpected secondary header");
  ok &= check(packet.data == std::vector<uint8_t>{0xDE, 0xAD},
              "Incorrect packet data");
  return ok;
}

// Port of spp-rs::space_packet::tests::test_space_packet_roundtrip.
bool test_space_packet_roundtrip() {
  SpacePacket packet;
  packet.primary_header.packet_version_number = 1;
  packet.primary_header.packet_type = TELEMETRY;
  packet.primary_header.secondary_header_flag = true;
  packet.primary_header.application_process_id = 0x2A;
  packet.primary_header.sequence_flag = 2;
  packet.primary_header.packet_sequence = 0x1234;
  packet.primary_header.packet_data_length = 2; // Three data bytes minus one.
  packet.secondary_header.emplace();
  packet.data = {0x01, 0x02, 0x03};

  const auto bytes = packet.to_bytes();
  bool ok = check(bytes == std::vector<uint8_t>{0x28, 0x2A, 0x92, 0x34, 0x00,
                                                0x02, 0x01, 0x02, 0x03},
                  "Incorrect serialized packet bytes");
  const SpacePacket parsed(bytes);
  const auto &header = parsed.primary_header;
  ok &= check(header.packet_version_number ==
                  packet.primary_header.packet_version_number,
              "Roundtrip changed packet version");
  ok &= check(header.packet_type == packet.primary_header.packet_type,
              "Roundtrip changed packet type");
  ok &= check(header.secondary_header_flag ==
                  packet.primary_header.secondary_header_flag,
              "Roundtrip changed secondary header flag");
  ok &= check(header.application_process_id ==
                  packet.primary_header.application_process_id,
              "Roundtrip changed application ID");
  ok &= check(header.sequence_flag == packet.primary_header.sequence_flag,
              "Roundtrip changed sequence flag");
  ok &= check(header.packet_sequence == packet.primary_header.packet_sequence,
              "Roundtrip changed packet sequence");
  ok &= check(header.packet_data_length ==
                  packet.primary_header.packet_data_length,
              "Roundtrip changed packet data length");
  ok &= check(parsed.secondary_header.has_value(),
              "Roundtrip lost secondary header");
  ok &= check(parsed.data == packet.data, "Roundtrip changed packet data");
  return ok;
}

int main() {
  bool ok = test_binary_io();
  ok &= test_parse_space_packet();
  ok &= test_space_packet_roundtrip();
  if (ok) {
    std::cout << "spp tests passed\n";
  }
  return ok ? 0 : 1;
}
