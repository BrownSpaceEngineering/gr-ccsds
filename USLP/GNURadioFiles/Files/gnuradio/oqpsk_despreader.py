"""
Embedded Python Block: O-QPSK despreader.

Input : float, one sample per chip (from symbol_sync / clock recovery)
Output: uint8, one 4-bit symbol (0..15) per 32 chips

Replaces the despreading half of gr-ieee802-15-4's packet_sink, without its
802.15.4 framing (SFD search, one-byte PHY length, 127-byte cap). Framing is
left to a downstream ASM correlator.

Chip order and masking follow packet_sink.cc: shift register fills MSB-first,
and the first and last chip are ignored because MSK phase memory makes them
depend on the neighbouring symbol.
"""

import numpy as np
from gnuradio import gr

# FM-demodulated chip sequences, 32 chips per 4-bit symbol.
# From gr-ieee802-15-4 lib/packet_sink.cc (Notor/Caviglia/Levy mapping).
CHIP_MAPPING = np.array([
    1618456172, 1309113062, 1826650030, 1724778362,  778887287, 2061946375,
    2007919840,  125494990,  529027475,  838370585,  320833617,  422705285,
    1368596360,   85537272,  139563807, 2021988657,
], dtype=np.uint32)

MASK = np.uint32(0x7FFFFFFE)
CHIPS_PER_SYMBOL = 32
INVALID = 0xFF

_POPCOUNT = np.array([bin(i).count('1') for i in range(256)], dtype=np.uint8)


def _hamming(reg):
    """Hamming distance from `reg` to each of the 16 reference sequences."""
    x = (np.uint32(reg) & MASK) ^ (CHIP_MAPPING & MASK)
    return (_POPCOUNT[x & 0xFF] + _POPCOUNT[(x >> 8) & 0xFF] +
            _POPCOUNT[(x >> 16) & 0xFF] + _POPCOUNT[(x >> 24) & 0xFF])


def decode_chips(reg, threshold):
    """Best-matching symbol, or INVALID if no sequence is close enough."""
    d = _hamming(reg)
    k = int(np.argmin(d))
    return (k, int(d[k])) if d[k] < threshold else (INVALID, int(d[k]))


class blk(gr.basic_block):
    """Despread 32 chips to one 4-bit symbol, with chip-alignment acquisition.

    acq_threshold is deliberately stricter than threshold: a *misaligned*
    window can still land within the tracking threshold (measured: a 5-chip
    offset decoded as a valid symbol at hamming 9), so locking on a loose
    match gives a confident, permanently wrong alignment.
    """

    SEARCH, VERIFY, LOCKED = 0, 1, 2

    def __init__(self, threshold=10, acq_threshold=4, verify_syms=3):
        gr.basic_block.__init__(
            self,
            name='O-QPSK Despreader',
            in_sig=[np.float32],
            out_sig=[np.uint8])
        self.threshold = int(threshold)          # chip errors tolerated once locked
        self.acq_threshold = int(acq_threshold)  # stricter, used to trust an alignment
        self.verify_syms = int(verify_syms)      # good symbols at 32-chip spacing to lock
        self.reg = 0
        self.chip_cnt = 0
        self.good = 0
        self.state = self.SEARCH

    def forecast(self, noutput_items, ninputs):
        return [noutput_items * CHIPS_PER_SYMBOL] * ninputs

    def general_work(self, input_items, output_items):
        inp, out = input_items[0], output_items[0]
        produced = consumed = 0

        for x in inp:
            if produced >= len(out):
                break
            self.reg = ((self.reg << 1) | (1 if x > 0 else 0)) & 0xFFFFFFFF
            consumed += 1

            if self.state == self.SEARCH:
                # Slide one chip at a time, demanding a near-exact match.
                sym, dist = decode_chips(self.reg, self.acq_threshold)
                if sym != INVALID:
                    self.state = self.VERIFY
                    self.chip_cnt = 0
                    self.good = 1
                continue

            self.chip_cnt += 1
            if self.chip_cnt < CHIPS_PER_SYMBOL:
                continue
            self.chip_cnt = 0

            if self.state == self.VERIFY:
                # Confirm the candidate alignment at true symbol spacing.
                sym, dist = decode_chips(self.reg, self.acq_threshold)
                if sym == INVALID:
                    self.state = self.SEARCH
                    continue
                self.good += 1
                if self.good >= self.verify_syms:
                    self.state = self.LOCKED
                continue

            sym, dist = decode_chips(self.reg, self.threshold)
            if sym == INVALID:
                self.state = self.SEARCH      # lost alignment, re-acquire
                continue
            out[produced] = sym
            produced += 1

        self.consume(0, consumed)
        return produced
