#include "binary_reader.h"

#include <cstdint>

uint8_t BinaryReader::read_u8() {
  // if (pos >= data.size()) {
  //   throw std::out_of_range("Tried to read out of binary reader range");
  // }

  return data[pos++];
}

uint16_t BinaryReader::read_u16() {
  uint16_t low = read_u8();
  uint16_t high = read_u8();

  return (high << 8) | low;
}
