/*
 * test_pdu.c — Unit tests for PDU encode/decode round-trips and checksum
 *
 * Each test prints PASS or FAIL. Returns 0 if all pass.
 */

#include "cfdp.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            failures++; \
        } else { \
            printf("PASS: %s\n", msg); \
        } \
    } while (0)

/* -----------------------------------------------------------------------
 * PDU Header round-trip
 * ---------------------------------------------------------------------- */
static void test_header_roundtrip(void)
{
    cfdp_pdu_header_t orig = {
        .version            = 1,
        .pdu_type           = CFDP_PDU_FILE_DIRECTIVE,
        .direction          = CFDP_DIR_TOWARD_RECEIVER,
        .tx_mode            = CFDP_TX_MODE_UNACKNOWLEDGED,
        .crc_flag           = false,
        .large_file         = false,
        .data_field_len     = 42,
        .entity_id_len      = 2,
        .seq_num_len        = 2,
        .source_entity_id   = 0x1234,
        .dest_entity_id     = 0x5678,
        .transaction_seq_num= 0xABCD,
    };

    uint8_t buf[64];
    int enc = cfdp_encode_header(&orig, buf, sizeof(buf));
    CHECK(enc > 0, "header encode returns positive length");

    cfdp_pdu_header_t decoded = {0};
    int dec = cfdp_decode_header(buf, (size_t)enc + orig.data_field_len,
                                 &decoded);
    CHECK(dec == enc, "header decode consumes same bytes as encode");

    CHECK(decoded.version            == orig.version,            "version");
    CHECK(decoded.pdu_type           == orig.pdu_type,           "pdu_type");
    CHECK(decoded.direction          == orig.direction,          "direction");
    CHECK(decoded.tx_mode            == orig.tx_mode,            "tx_mode");
    CHECK(decoded.data_field_len     == orig.data_field_len,     "data_field_len");
    CHECK(decoded.entity_id_len      == orig.entity_id_len,      "entity_id_len");
    CHECK(decoded.seq_num_len        == orig.seq_num_len,        "seq_num_len");
    CHECK(decoded.source_entity_id   == orig.source_entity_id,   "source_entity_id");
    CHECK(decoded.dest_entity_id     == orig.dest_entity_id,     "dest_entity_id");
    CHECK(decoded.transaction_seq_num== orig.transaction_seq_num,"transaction_seq_num");
}

/* -----------------------------------------------------------------------
 * Metadata PDU round-trip
 * ---------------------------------------------------------------------- */
static void test_metadata_roundtrip(void)
{
    cfdp_metadata_pdu_t orig = {
        .closure_requested = true,
        .checksum_type     = CFDP_CHECKSUM_MODULAR,
        .file_size         = 123456,
    };
    strncpy(orig.src_filename, "/data/telemetry.bin", CFDP_MAX_FILENAME_LEN);
    strncpy(orig.dst_filename, "telemetry.bin",       CFDP_MAX_FILENAME_LEN);

    uint8_t buf[512];
    int enc = cfdp_encode_metadata(&orig, buf, sizeof(buf));
    CHECK(enc > 0, "metadata encode returns positive length");

    cfdp_metadata_pdu_t dec = {0};
    int consumed = cfdp_decode_metadata(buf, (size_t)enc, &dec);
    CHECK(consumed == enc, "metadata decode consumes same bytes as encode");

    CHECK(dec.closure_requested == orig.closure_requested, "closure_requested");
    CHECK(dec.checksum_type     == orig.checksum_type,     "checksum_type");
    CHECK(dec.file_size         == orig.file_size,         "file_size");
    CHECK(strcmp(dec.src_filename, orig.src_filename) == 0, "src_filename");
    CHECK(strcmp(dec.dst_filename, orig.dst_filename) == 0, "dst_filename");
}

/* -----------------------------------------------------------------------
 * File Data PDU round-trip
 * ---------------------------------------------------------------------- */
static void test_file_data_roundtrip(void)
{
    uint8_t  payload[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    cfdp_file_data_pdu_t orig = {
        .offset   = 0x400,
        .data     = payload,
        .data_len = sizeof(payload),
    };

    uint8_t buf[64];
    int enc = cfdp_encode_file_data(&orig, buf, sizeof(buf));
    CHECK(enc == (int)(4 + sizeof(payload)), "file_data encode length");

    cfdp_file_data_pdu_t dec = {0};
    int consumed = cfdp_decode_file_data(buf, (size_t)enc, &dec);
    CHECK(consumed == enc, "file_data decode consumes all bytes");
    CHECK(dec.offset   == orig.offset,   "offset");
    CHECK(dec.data_len == orig.data_len, "data_len");
    CHECK(memcmp(dec.data, orig.data, orig.data_len) == 0, "payload bytes");
}

/* -----------------------------------------------------------------------
 * EOF PDU round-trip
 * ---------------------------------------------------------------------- */
static void test_eof_roundtrip(void)
{
    cfdp_eof_pdu_t orig = {
        .condition_code = CFDP_COND_NO_ERROR,
        .checksum       = 0xDEADBEEF,
        .file_size      = 4096,
    };

    uint8_t buf[16];
    int enc = cfdp_encode_eof(&orig, buf, sizeof(buf));
    CHECK(enc == 10, "eof encode length == 10");

    cfdp_eof_pdu_t dec = {0};
    int consumed = cfdp_decode_eof(buf, (size_t)enc, &dec);
    CHECK(consumed == enc, "eof decode consumes all bytes");
    CHECK(dec.condition_code == orig.condition_code, "condition_code");
    CHECK(dec.checksum       == orig.checksum,       "checksum");
    CHECK(dec.file_size      == orig.file_size,      "eof file_size");
}

/* -----------------------------------------------------------------------
 * Checksum: known value
 *
 * The modular checksum of the 8-byte sequence:
 *   [0x01, 0x02, 0x03, 0x04,  0x05, 0x06, 0x07, 0x08]
 *
 * Word 0: 0x01020304
 * Word 1: 0x05060708
 * Sum   : 0x06080A0C
 * ---------------------------------------------------------------------- */
static void test_checksum_known(void)
{
    uint8_t data[8] = { 0x01, 0x02, 0x03, 0x04,
                         0x05, 0x06, 0x07, 0x08 };
    uint32_t sum = cfdp_checksum_modular(0, data, sizeof(data));
    CHECK(sum == 0x06080A0Cu, "modular checksum known value");
}

/* Verify that chunking gives the same result as one-shot */
static void test_checksum_incremental(void)
{
    uint8_t data[12] = { 0x11, 0x22, 0x33, 0x44,
                          0x55, 0x66, 0x77, 0x88,
                          0x99, 0xAA, 0xBB, 0xCC };

    uint32_t sum_full  = cfdp_checksum_modular(0, data, sizeof(data));

    /* Compute in two 4-byte chunks */
    uint32_t sum_inc   = cfdp_checksum_modular(0, data,     4);
    sum_inc            = cfdp_checksum_modular(sum_inc, data + 4, 4);
    sum_inc            = cfdp_checksum_modular(sum_inc, data + 8, 4);

    CHECK(sum_full == sum_inc, "incremental checksum matches full checksum");
}

/* -----------------------------------------------------------------------
 * Edge cases
 * ---------------------------------------------------------------------- */
static void test_encode_buf_too_small(void)
{
    cfdp_eof_pdu_t eof = {
        .condition_code = CFDP_COND_NO_ERROR,
        .checksum       = 0,
        .file_size      = 0,
    };
    uint8_t tiny[4];
    int rc = cfdp_encode_eof(&eof, tiny, sizeof(tiny));
    CHECK(rc == CFDP_ERR_BUF_TOO_SMALL, "eof encode into tiny buffer errors");
}

static void test_decode_truncated_header(void)
{
    /* Feed only 2 bytes of header — must fail gracefully */
    uint8_t two_bytes[2] = { 0x20, 0x00 };
    cfdp_pdu_header_t hdr;
    int rc = cfdp_decode_header(two_bytes, sizeof(two_bytes), &hdr);
    CHECK(rc == CFDP_ERR_INVALID_PDU, "truncated header decode fails cleanly");
}

/* -----------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
int main(void)
{
    printf("=== CFDP PDU Unit Tests ===\n\n");

    printf("-- Header round-trip --\n");
    test_header_roundtrip();

    printf("\n-- Metadata round-trip --\n");
    test_metadata_roundtrip();

    printf("\n-- File Data round-trip --\n");
    test_file_data_roundtrip();

    printf("\n-- EOF round-trip --\n");
    test_eof_roundtrip();

    printf("\n-- Checksum --\n");
    test_checksum_known();
    test_checksum_incremental();

    printf("\n-- Edge cases --\n");
    test_encode_buf_too_small();
    test_decode_truncated_header();

    printf("\n===========================\n");
    if (failures == 0) {
        printf("All tests PASSED.\n");
        return 0;
    } else {
        printf("%d test(s) FAILED.\n", failures);
        return 1;
    }
}