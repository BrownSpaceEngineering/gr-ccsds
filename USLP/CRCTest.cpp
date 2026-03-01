#include "CRCpp/inc/CRC.h" // Only need to include this header file!
                 // No libraries need to be included. No project settings need to be messed with.
				 
#include <iomanip>  // Includes ::std::hex
#include <iostream> // Includes ::std::cout
#include <cstdint>  // Includes ::std::uint32_t

struct Packet {
	std::vector<uint8_t> data;
	uint32_t crc;
};

int main(int argc, char ** argv) {
	const char myString[] = { 'H', 'E', 'L', 'L', 'O', ' ', 'W', 'O', 'R', 'L', 'D' };
	Packet p;
	p.data.assign(myString, myString + sizeof(myString));
	p.crc = CRC::Calculate(myString, sizeof(myString), CRC::CRC_32());
	std::vector<uint8_t> result(myString, myString + sizeof(myString));


	std::cout << std::hex << p.crc;
	
	return 0;
}