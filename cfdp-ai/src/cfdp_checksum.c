/*
 * cfdp_checksum.c — CFDP modular (additive 32-bit) checksum
 *
 * CCSDS 727.0-B-5 4.2 defines the checksum as a modular 32-bit sum of
 * the file contents interpreted as 4-byte big-endian words, zero-padded
 * to a 4-byte boundary on the last word.
 *
 * The "modular" checksum is type 0x00 and is simple to implement,
 * though it provides weak error detection.
 */

#include "cfdp.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * Modular (additive) 32-bit checksum
 *
 * The algorithm:
 *   1. Treat the file as a sequence of 4-byte big-endian words.
 *   2. If the file length is not a multiple of 4, pad the last partial
 *      word with zeros on the right (i.e., the most-significant missing
 *      bytes are zero, consistent with big-endian alignment).
 *   3. Sum all words modulo 2^32.
 * ---------------------------------------------------------------------- */

uint32_t cfdp_checksum_modular(uint32_t sum,
                               const uint8_t *data, size_t len)
{
    size_t i = 0;

    /*
     * Process complete 4-byte words. We accumulate bytes shifted into
     * position based on their offset within the word so that the sum is
     * identical regardless of whether we process byte-by-byte or in bulk.
     */
    while (i + 4 <= len) {
        uint32_t word =
            ((uint32_t)data[i    ] << 24) |
            ((uint32_t)data[i + 1] << 16) |
            ((uint32_t)data[i + 2] <<  8) |
            ((uint32_t)data[i + 3]);
        sum += word;
        i   += 4;
    }

    /*
     * Partial last word — pad with zeros on the right (big-endian).
     * This handles the case where the chunk ends mid-word. For full
     * protocol correctness the caller should ensure that the final
     * chunk ends at the last byte of the file so this padding matches
     * the spec's zero-padding of the final word.
     */
    if (i < len) {
        uint32_t word = 0;
        size_t rem = len - i;
        for (size_t j = 0; j < rem; j++) {
            word |= (uint32_t)data[i + j] << (8 * (3 - j));
        }
        sum += word;
    }

    return sum;
}

/* -----------------------------------------------------------------------
 * CRC-32
 *
 * CFDP checksum type 0x01. This provides much better error detection than
 * the modular sum. The table is generated at first call.
 * ---------------------------------------------------------------------- */

static uint32_t crc32_table[256];
static bool     crc32_table_ready = false;

static void crc32_build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;  /* reflected polynomial */
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_ready = true;
}

/**
 * Incremental CRC-32.
 * Initialize with crc = 0xFFFFFFFF.
 * After the final chunk, XOR with 0xFFFFFFFF to get the final CRC.
 */
uint32_t cfdp_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    if (!crc32_table_ready) crc32_build_table();

    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFFu];
    }
    return crc;
}