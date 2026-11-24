#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <iostream>
#include <common/data.h>
#include <common/uslpstructs.h>
#include <bit>

class CCS_SL {
public:
    CCS_SL(const CCS_SLConfig& config);
    ~CCS_SL() = default;

    /**
     * @brief Entry point from the USLP stack. Builds one AT86RF215 RAW-SFD-16 PPDU.
     *
     * The transfer frame alone becomes the PSDU, so it is what the randomizer and the
     * encoder operate on. The Synchronization Header is then prepended in the clear,
     * because the receiver correlates against the preamble and the sync word before it
     * has recovered any bit timing and so cannot descramble them.
     *
     * @return preamble, then SFD-16, then the randomized and optionally encoded frame.
     */
    template <size_t Capacity>
    BitBuffer<MAX_CCS_SL_FRAME_LENGTH> ProcessFrame(const BitBuffer<Capacity>& logicalFrame) {
        BitBuffer<MAX_CCS_SL_FRAME_LENGTH> ppdu;

        // 1. Lay the SHR down first, so the PSDU lands at its final offset and never moves.
        // Every frame carries its own: the AT86RF215 is a packet receiver that returns to
        // LISTEN after each PPDU, and SFD detection requires the PREAMBLE_LOCK state.
        ppdu.fill(0, FSK_PREAMBLE_BYTE, FSK_PREAMBLE_LENGTH);
        ppdu.insertAtEnd(FSK_SFD0, FSK_SFD_LENGTH);

        // 2. Copy the transfer frame in behind it. This is the one unavoidable copy: the
        // caller's frame is const and the pipeline scrambles in place.
        ppdu.insertAtEnd(logicalFrame.data.data(), logicalFrame.length);

        // 3. Run the physical layer over the PSDU region only, leaving the SHR in the clear.
        ppdu.length = SHR_LENGTH + ExecutePipeline(ppdu.data.data() + SHR_LENGTH,
                                                   logicalFrame.length,
                                                   MAX_CCS_SL_FRAME_LENGTH - SHR_LENGTH);
        return ppdu;
    }

    /**
     * @brief Octets of Synchronization Header prepended to each frame by ProcessFrame.
     * Lets a caller log or replay just the PSDU, which is what the receiver's frame buffer holds.
     */
    static constexpr size_t SHRLength() { return SHR_LENGTH; }

    std::vector<uint8_t> Encode(const uint8_t* data, size_t length);
    std::vector<uint8_t> FourEncode(const uint8_t* data, size_t length);
    void PseudoRandomize(uint8_t* data, size_t length);
    std::vector<uint8_t> Decode(const uint8_t* coded, size_t length);
    std::vector<uint8_t> FlushDecoder();
    void UndoPseudoRandomize(uint8_t* data, size_t length);

private:
    void AddCompareSelect(uint8_t rxSymbols);
    void AppendBit(uint8_t bit, std::vector<uint8_t>& out);
    void ResetDecoder();

    static const uint8_t CONV_STATES = 64; // 2^(K-1), number of possible states for symbol

    static uint8_t ConvOutputs(uint8_t state, uint8_t inputBit); //setup for lookup table
    static uint8_t PrevState(uint8_t state, uint64_t decisions);
    

    static const uint8_t TRACEBACK_DEPTH = 64; //not sure what depth exactly
    // --- State variables for Viterbi decoding ---
    std::array<std::array<uint8_t, 2>, CONV_STATES> m_expected{}; //lookup table for possible states for n+1 symbol
    std::array<uint32_t, CONV_STATES> m_pathMetric{};             // Running cost of the best path into each state
    std::array<uint64_t, TRACEBACK_DEPTH> m_survivors{};          // Ring buffer, one decision bit per state per step
    size_t m_decodeSteps = 0;
    size_t m_bitsEmitted = 0;
    uint8_t m_bestState = 0;
    uint8_t m_outByte = 0;
    uint8_t m_outBitCount = 0;

    /**
     * @brief Randomizes and optionally encodes the PSDU in place.
     * @return the resulting PSDU length, which the encoder doubles when FEC is enabled.
     */
    size_t ExecutePipeline(uint8_t* physicalData, size_t physicalLength, size_t capacity);

    CCS_SLConfig m_config;

    // AT86RF215 SFD0, the uncoded sync word. Registers {FSKSFD0H, FSKSFD0L} hold {0x72, 0x09}
    // and are transmitted LSB first (datasheet 6.10.7.7-6.10.7.8), which puts 10010000 01001110
    // on air. SFD1, the coded counterpart, would be {0x6F, 0x4E}.
    static constexpr uint8_t FSK_SFD0[FSK_SFD_LENGTH] = {0x90, 0x4E};
    
    // --- State variables for convolutional encoding ---
    uint8_t convState = 0; 
};