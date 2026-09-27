#pragma once

#include <cstdint>
#include <vector>

class BinaryReader {
private:
  const std::vector<uint8_t> &data;
  int pos = 0;

public:
  BinaryReader(const std::vector<uint8_t> &data) : data(data) {}

  uint8_t read_u8();
  uint16_t read_u16();
};
