#include <common/ccs_sl.h>
#include <iostream>

// Standard CCSDS r=1/2, K=7 Viterbi Decoder (hard decision, sliding-window traceback)
std::vector<uint8_t> CCS_SL::Decode(const uint8_t* coded, size_t length) {
    std::vector<uint8_t> decoded;
    decoded.reserve(length / 2 + 1);
    for (size_t i = 0; i < length; i++) {

    }
}

void CCS_SL::AddCompareSelect(uint8_t rxSymbols) {
    std::array<uint32_t, 64> nextMetric; //replace 64 with CONV_STATES
    uint64_t decisions = 0;
    uint32_t bestMetric = UINT32_MAX;

    for (uint8_t ns = 0; ns < CONV_STATES; ++ns) {
        uint8_t inputBit = ns >> 5;               // Bit that was shifted in to reach ns
        uint8_t p0 = (ns << 1) & 0x3F;            // Predecessor whose shifted-out bit was 0
        uint8_t p1 = p0 | 1;                      // Predecessor whose shifted-out bit was 1

        uint32_t m0 = m_pathMetric[p0] + std::popcount<uint8_t>(rxSymbols ^ m_expected[p0][inputBit]);
        uint32_t m1 = m_pathMetric[p1] + std::popcount<uint8_t>(rxSymbols ^ m_expected[p1][inputBit]);

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
