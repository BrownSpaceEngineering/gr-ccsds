#include "binary_writer.h"
#include <cstdint>

void BinaryWriter::write_u8(uint8_t num) { data.push_back(num); }

void BinaryWriter::write_u16(uint16_t num) {
  uint8_t high = num >> 8;
  uint8_t low = num & 0xFF;

  write_u8(low);
  write_u8(high);
}
