#pragma once

#include <stdint.h> 
#include <iostream>
#include <bitset>
#include <vector>
#include <array>
#include <cstdint>
#include <cassert>
#include <random>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iomanip>

constexpr int TEST_ARRAY_SIZE = 3042;

void printBytes(uint64_t value);
template <size_t M, size_t N> void append(const BitBuffer<M>& src, BitBuffer<N>& dest, size_t& offset);
std::array<uint8_t, 4> CRCGenerator();
// Generates random bytes enough to fill 3 transfer frames with maximum capacity
std::array<uint8_t, TEST_ARRAY_SIZE> GenerateRandomBytes();
// Writes Bytes to .txt assuming maximum transfer frame capacity
void WriteBytes(std::array<uint8_t, TEST_ARRAY_SIZE> message);

// Definition lives here because it is a template
template <size_t M, size_t N> void append(const BitBuffer<M>& src, BitBuffer<N>& dest, size_t& offset) {
	std::memcpy(&dest.data[offset], &src.data[0], src.length);
	offset += src.length;
};