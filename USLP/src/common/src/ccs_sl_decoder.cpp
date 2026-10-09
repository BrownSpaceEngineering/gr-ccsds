#include <common/ccs_sl.h>
#include <iostream>


// Returns the two coded symbols (c1 << 1) | c2 for one input bit, matching Encode()
uint8_t CCS_SL::ConvOutputs(uint8_t state, uint8_t inputBit) {
    // State bit 5 is the most recent previous input, bit 0 the oldest
    uint8_t c1 = inputBit ^ ((state >> 5) & 1) ^ ((state >> 4) & 1) ^ ((state >> 3) & 1) ^ (state & 1); // G1 = 171 (octal)
    uint8_t c2 = inputBit ^ ((state >> 4) & 1) ^ ((state >> 3) & 1) ^ ((state >> 1) & 1) ^ (state & 1); // G2 = 133 (octal)

    return static_cast<uint8_t>((c1 << 1) | (c2 ^ 1)); // Invert c2 per CCSDS
}

// Precompute the expected coded symbols for every trellis branch
CCS_SL::CCS_SL(const CCS_SLConfig& config) : m_config(config) {
    for (uint8_t state = 0; state < CONV_STATES; ++state) {
        m_expected[state][0] = ConvOutputs(state, 0);
        m_expected[state][1] = ConvOutputs(state, 1);
    }
}



// Standard CCSDS r=1/2, K=7 Viterbi Decoder (hard decision, sliding-window traceback)
std::vector<uint8_t> CCS_SL::Decode(const uint8_t* coded, size_t length) {
    std::vector<uint8_t> decoded;
    decoded.reserve(length / 2 + 1);
    for (size_t i = 0; i < length; i++) {

    }
}



void CCS_SL::AddCompareSelect(uint8_t symbolPair) {
    std::array<uint32_t, 64> nextMetric; //replace 64 with CONV_STATES
    uint64_t decisions = 0;
    uint32_t bestMetric = UINT32_MAX;

    for (uint8_t ns = 0; ns < CONV_STATES; ++ns) {
        uint8_t inputBit = ns >> 5;               // Bit that was shifted in to reach ns
        uint8_t p0 = (ns << 1) & 0x3F;            // Predecessor whose shifted-out bit was 0
        uint8_t p1 = p0 | 1;                      // Predecessor whose shifted-out bit was 1

        uint32_t m0 = m_pathMetric[p0] + std::popcount<uint8_t>(symbolPair ^ m_expected[p0][inputBit]);
        uint32_t m1 = m_pathMetric[p1] + std::popcount<uint8_t>(symbolPair ^ m_expected[p1][inputBit]);

        if (m1 < m0) {
            nextMetric[ns] = m1;
            decisions |= (1ULL << ns);
        } else {
            nextMetric[ns] = m0;
        }

        if (nextMetric[ns] < bestMetric) {
            bestMetric = nextMetric[ns];
            m_bestState = ns;
        }
    }

    // Normalize so metrics never overflow on a continuous stream
    for (uint8_t s = 0; s < CONV_STATES; ++s) {
        m_pathMetric[s] = nextMetric[s] - bestMetric;
    }

    m_survivors[m_decodeSteps % TRACEBACK_DEPTH] = decisions;
    m_decodeSteps++;
}
