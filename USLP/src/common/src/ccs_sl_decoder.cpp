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

void CCS_SL::AppendBit(uint8_t bit, std::vector<uint8_t>& out) {
    m_outByte = static_cast<uint8_t>((m_outByte << 1) | bit);
    if (++m_outBitCount == 8) {
        out.push_back(m_outByte);
        m_outByte = 0;
        m_outBitCount = 0;
    }
}

uint8_t CCS_SL::PrevState(uint8_t state, uint64_t decisions) {
    uint8_t b = (decisions >> state) & 1;
    return static_cast<uint8_t>(((state << 1) & 0x3F) | b);
}

// Standard CCSDS r=1/2, K=7 Viterbi Decoder (hard decision, sliding-window traceback)
std::vector<uint8_t> CCS_SL::Decode(const uint8_t* coded, size_t length) {
    std::vector<uint8_t> decoded;
    decoded.reserve(length / 2 + 1);
    for (size_t i = 0; i < length; i++) {
        for (int shift = 6; shift >= 0; shift -= 2) {
            AddCompareSelect((coded[i] >> shift) & 0x3); //splits a byte into 4 symbols
            // Once the window is full, trace back TRACEBACK_DEPTH steps from the best
            // state and release the oldest undecided bit
            if (m_decodeSteps > TRACEBACK_DEPTH) {
                uint8_t state = m_bestState;
                size_t newest = m_decodeSteps - 1;
                for (size_t k = newest; k > newest - TRACEBACK_DEPTH; k--) { //goes back TRACEBACK_DEPTH bits 
                    state = PrevState(state, m_survivors[k % TRACEBACK_DEPTH]); //uses the list of survivors to check previous state
                }
            AppendBit(state >> 5, decoded); // Input bit that entered this state
            m_bitsEmitted++;
            }
        }
        
    }
    return decoded;
}

// Releases the bits still held in the traceback window, then resets the decoder
std::vector<uint8_t> CCS_SL::FlushDecoder() {
    std::vector<uint8_t> decoded;
    size_t pendingBits = m_decodeSteps - m_bitsEmitted; // Always <= TRACEBACK_DEPTH
    std::array<uint8_t, TRACEBACK_DEPTH> bits{};

    // 1. Trace back from the best state over every undecided step
    uint8_t state = m_bestState;
    for (size_t j = 0; j < pendingBits; ++j) {
        size_t step = m_decodeSteps - 1 - j;
        bits[pendingBits - 1 - j] = state >> 5;
        state = PrevState(state, m_survivors[step % TRACEBACK_DEPTH]);
    }

    // 2. Emit them oldest first
    for (size_t j = 0; j < pendingBits; ++j) {
        AppendBit(bits[j], decoded);
    }

    // 3. Pad out any partial final byte
    if (m_outBitCount > 0) {
        decoded.push_back(static_cast<uint8_t>(m_outByte << (8 - m_outBitCount)));
    }
    ResetDecoder();
    return decoded;
}

// Clears decoder history
void CCS_SL::ResetDecoder() {
    m_pathMetric.fill(0);
    m_survivors.fill(0);
    m_decodeSteps = 0;
    m_bitsEmitted = 0;
    m_bestState = 0;
    m_outByte = 0;
    m_outBitCount = 0;
}


void CCS_SL::AddCompareSelect(uint8_t symbolPair) {
    std::array<uint32_t, CONV_STATES> nextMetric;
    uint64_t decisions = 0;
    uint32_t bestMetric = UINT32_MAX;

    for (uint8_t i = 0; i < CONV_STATES; i++) {
        uint8_t inputBit = i >> 5;               // Bit that was shifted in to reach ns
        uint8_t p0 = (i << 1) & 0x3F;            // Predecessor whose shifted-out bit was 0
        uint8_t p1 = p0 | 1;                      // Predecessor whose shifted-out bit was 1

        uint32_t m0 = m_pathMetric[p0] + std::popcount<uint8_t>(symbolPair ^ m_expected[p0][inputBit]);
        uint32_t m1 = m_pathMetric[p1] + std::popcount<uint8_t>(symbolPair ^ m_expected[p1][inputBit]);

        if (m1 < m0) {
            nextMetric[i] = m1;
            decisions |= (1ULL << i); //sets bit i of decisions to 1
        } 
        else {
            nextMetric[i] = m0;
        }

        if (nextMetric[i] < bestMetric) {
            bestMetric = nextMetric[i];
            m_bestState = i;
        }
    }

    // Normalize so metrics never overflow on a continuous stream
    for (uint8_t s = 0; s < CONV_STATES; ++s) {
        m_pathMetric[s] = nextMetric[s] - bestMetric;
    }

    m_survivors[m_decodeSteps % TRACEBACK_DEPTH] = decisions;
    m_decodeSteps++;
}
