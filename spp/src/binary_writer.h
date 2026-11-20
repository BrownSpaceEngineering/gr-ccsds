#pragma once

#include <cstdint>
#include <vector>

class BinaryWriter {
private:
  std::vector<uint8_t> &data;

public:
  BinaryWriter(std::vector<uint8_t> &data) : data(data) {}

  void write_u8(uint8_t num);
  void write_u16(uint16_t num);
};
