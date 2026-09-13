#include <common/ccs_sl.h>
#include <iostream>

// Constructor implementation
CCS_SL::CCS_SL(const CCS_SLConfig& config) : m_config(config) {}

// Coordinates the physical layer execution sequence and returns the finished data
std::vector<uint8_t> CCS_SL::ExecutePipeline(uint8_t* physicalData, size_t physicalLength) {
    // 1. Pseudorandomisation (scrambling)
    switch (m_config.selectedOptions.randomizer) {
        case CCS_SLConfig::Randomizer::LONG:
            PseudoRandomize(physicalData, physicalLength);
            break;
        case CCS_SLConfig::Randomizer::NONE:
            // Bypassed: frame passes through unscrambled
            break;
        case CCS_SLConfig::Randomizer::SHORT:
            std::cerr << "Short randomizer not implemented, frame left unscrambled" << std::endl;
            break;
    }

    // 2. Channel Coding
    if (m_config.selectedOptions.codingMethod == CCS_SLConfig::CodingMethod::CONVOLUTIONAL) {
        // Returns the 2048-byte convolutional encoded stream
        return Encode(physicalData, physicalLength);
    } else {
        // If no channel coding is selected, return the scrambled 1024-byte physical frame directly
        return std::vector<uint8_t>(physicalData, physicalData + physicalLength);
    }
}

// Standard CCSDS Additive Scrambler (x^17 + x^14 + 1)
void CCS_SL::PseudoRandomize(uint8_t* data, size_t length) {
    uint32_t state = 0b11000111000111000; 

    for (size_t i = 0; i < length; ++i) {
        uint8_t scrambled_byte = 0;
        
        for (int bit = 7; bit >= 0; --bit) {
            uint32_t x0 = state & 1;

            // Extract the original data bit and XOR it with the scrambler/feedback bit
            uint8_t data_bit = (data[i] >> bit) & 1;
            scrambled_byte |= ((data_bit ^ x0) << bit);

            uint32_t x14 = (state >> 14) & 1; 
            uint32_t x17 = x0 ^ x14;

            // Shift right by 1, and feed the calculated bit back into Stage 1 (bit 16)
            state = (state >> 1) | (x17 << 16);
        }
        
        data[i] = scrambled_byte;
    }
}

// Standard CCSDS r=1/2, K=7 Convolutional Encoder
std::vector<uint8_t> CCS_SL::Encode(const uint8_t* data, size_t length) {
    // Dynamically allocate a flat vector to store the double-sized coded symbols.
    // This keeps the encoder in the compiled source file without template dependency.
    std::vector<uint8_t> encoded(length * 2);
    std::vector<uint8_t> g1 {5, 4, 3, 0};
    std::vector<uint8_t> g2 {4, 3, 1, 0};
    size_t outIdx = 0;

    for (size_t i = 0; i < length; ++i) {
        uint16_t packed16 = 0;
        for (int bit = 7; bit >= 0; --bit) {
            uint8_t inputBit = (data[i] >> bit) & 1;
            uint8_t c1 = inputBit;
            uint8_t c2 = inputBit;

            for (int i = 0; i < 4; i++) {
                c1 ^= ((convState >> g1[i]) & 1);
                c2 ^= ((convState >> g2[i]) & 1);
            }

            c2 ^= 1; // invert c2
            convState = ((convState >> 1) & 0x3F) | (inputBit << 5); // Push new bit into state
            packed16 = (packed16 << 2) | (c1 << 1) | c2; // Interleave the symbols sequentially: g1 followed by g2
        }

        // Pack the 16 output bits into two big-endian bytes
        encoded[outIdx] = static_cast<uint8_t>((packed16 >> 8) & 0xFF);
        encoded[outIdx + 1] = static_cast<uint8_t>(packed16 & 0xFF);
        outIdx += 2;
    }

    /*
    for (int i = 0; i < encoded.size(); i++) {
        cout << std::bitset<8>(encoded[i]) << " ";
    }
    cout << endl;
    */

    // Forward the fully encoded stream to your GNU Radio physical interface
    return encoded;
}