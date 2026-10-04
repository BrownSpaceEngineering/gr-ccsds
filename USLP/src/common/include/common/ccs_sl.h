#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <iostream>
#include <common/data.h>
#include <common/uslpstructs.h>

class CCS_SL {
public:
    CCS_SL(const CCS_SLConfig& config);
    ~CCS_SL() = default;

    /**
     * @brief Entry point from the USLP stack. Prepends ASM and delegates to the compiled pipeline.
     * @return A vector containing the sync-marked, scrambled, and potentially encoded physical bytes.
     */
    template <size_t Capacity>
    std::vector<uint8_t> ProcessFrame(const BitBuffer<Capacity>& logicalFrame) {
        // 1. Prepend the 4-byte 1ACFFC1D synchronization marker (creates 1024-byte block)
        BitBuffer<Capacity + 4> physicalFrame;
        physicalFrame.insertAtEnd(CCSDS_ASM, 4);
        physicalFrame.insertAtEnd(logicalFrame.data.data(), logicalFrame.length);

        // 2. Delegate the physical layer execution and return the resulting vector
        return ExecutePipeline(physicalFrame.data.data(), physicalFrame.length);
    }

    std::vector<uint8_t> Encode(const uint8_t* data, size_t length);
    void PseudoRandomize(uint8_t* data, size_t length);
private:


    std::vector<uint8_t> Decode(const uint8_t* coded, size_t length);
    void AddCompareSelect(uint8_t rxSymbols);
    static const uint8_t CONV_STATES = 64; // 2^(K-1), number of possible states for symbol
    static const uint8_t TRACEBACK_DEPTH = 64; //not sure what depth exactly
    // --- State variables for Viterbi decoding ---
    std::array<std::array<uint8_t, 2>, CONV_STATES> m_expected{}; // [state][input bit] -> (c1 << 1) | c2
    std::array<uint32_t, CONV_STATES> m_pathMetric{};             // Running cost of the best path into each state
    std::array<uint64_t, TRACEBACK_DEPTH> m_survivors{};          // Ring buffer, one decision bit per state per step
    size_t m_decodeSteps = 0;
    size_t m_bitsEmitted = 0;
    uint8_t m_bestState = 0;
    uint8_t m_outByte = 0;
    uint8_t m_outBitCount = 0;

    std::vector<uint8_t> ExecutePipeline(uint8_t* physicalData, size_t physicalLength);

    CCS_SLConfig m_config;
    const uint8_t CCSDS_ASM[4] = {0x1A, 0xCF, 0xFC, 0x1D};
    
    // --- State variables for convolutional encoding ---
    uint8_t convState = 0; 
};