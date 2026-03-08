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
#include <common/data.h>
#include <common/utils.h>
#include "CRC.h"

void printBytes(uint64_t value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (int i = sizeof(uint64_t) - 1; i >= 0; --i) {
        std::cout << static_cast<int>(bytes[i]) << " ";
    }
    std::cout << "\n";
}

std::array<uint8_t, 4> CRCGenerator() {
	//To be implemented later
	std::array<uint8_t, 4> CRC{};

	return CRC;
}

// Generates random bytes enough to fill 3 transfer frames with maximum capacity
std::array<uint8_t, TEST_ARRAY_SIZE> GenerateRandomBytes() {
	std::array<uint8_t, TEST_ARRAY_SIZE> message;

	/*std::generate(message.begin(), message.end(), [] {
		static std::mt19937 gen(std::random_device{}());
		static std::uniform_int_distribution<int> dist(0,255);
		return dist(gen);
	});*/

	std::mt19937 gen(12345);
	std::uniform_int_distribution<uint8_t> dist(0, 255);
	
	for (auto& b: message) {
		b = static_cast<uint8_t>(dist(gen));
	}

	return message;
}

// Writes Bytes to .txt assuming maximum transfer frame capacity
void WriteBytes(std::array<uint8_t, TEST_ARRAY_SIZE> message) {
	std::ofstream out("bytes.txt", std::ios::trunc);
	int lastTFIndex = 0;

	for (int i = 0; i < TEST_ARRAY_SIZE; i++) {
		//std::bitset<8> b2{message[i]};
		//std::cout << b2 << std::endl;
		if (i % 1014 == 0) {
			lastTFIndex = i;

			out << "\n";
			out << "\n";
			out << "------ NEW TRANSFER FRAME ------";
			out << "\n";
			out << "\n";
		}

		if ((i - lastTFIndex) % 32 == 0) {
			out << "\n";
		}

		out << std::setw(3) << static_cast<int>(message[i]) << "  ";
	}
}
