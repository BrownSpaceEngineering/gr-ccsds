/*
 * cfdp_class2_pdu.c — PDU serialization for Class 2 (Acknowledged) mode
 *
 * Implements encode/decode for:
 *   Finished PDU  (CCSDS 727.0-B-5 5.2.3, directive code 0x05)
 *   ACK PDU       (5.2.4, directive code 0x06)
 *   NAK PDU       (5.2.6, directive code 0x08)
 *
 * Follows the same conventions as cfdp_pdu.c:
 *   - encode: returns bytes written, or negative cfdp_status_t on error
 *   - decode: returns bytes consumed, or negative cfdp_status_t on error
 *   - all multi-byte integers are big-endian on the wire
 */

#include "cfdp_class2.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * Finished PDU (5.2.3)
 *
 * Data field layout:
 *   Byte 0: directive code = 0x05
 *   Byte 1: [7-4: condition_code][3: delivery_code][2-1: file_status]
 *           [0: spare]
 * ---------------------------------------------------------------------- */

int cfdp2_encode_finished(const cfdp_finished_pdu_t *fin,
                          uint8_t *buf, size_t buf_len)
{
    if (buf_len < 2u) return CFDP_ERR_BUF_TOO_SMALL;

    buf[0] = CFDP_DIRECTIVE_FINISHED;
    buf[1] = (uint8_t)(
        (((uint8_t)fin->condition_code & 0x0Fu) << 4) |
        (((uint8_t)fin->delivery_code  & 0x01u) << 2) |
        ( (uint8_t)fin->file_status    & 0x03u)
    );
    return 2;
}

int cfdp2_decode_finished(const uint8_t *buf, size_t buf_len,
                          cfdp_finished_pdu_t *fin)
{
    if (buf_len < 2u) return CFDP_ERR_INVALID_PDU;
    if (buf[0] != CFDP_DIRECTIVE_FINISHED) return CFDP_ERR_INVALID_PDU;

    fin->condition_code = (cfdp_condition_code_t)((buf[1] >> 4) & 0x0Fu);
    fin->delivery_code  = (cfdp_delivery_code_t) ((buf[1] >> 2) & 0x01u);
    fin->file_status    = (cfdp_file_status_t)   ( buf[1]       & 0x03u);
    return 2;
}

/* -----------------------------------------------------------------------
 * ACK PDU (5.2.4)
 *
 * Data field layout:
 *   Byte 0: directive code = 0x06
 *   Byte 1: [7-4: acked_directive_code][3-2: directive_subtype][1-0: spare]
 *   Byte 2: [7-4: condition_code][3: spare][2-0: transaction_status]
 *
 * The Blue Book specifies:
 *   directive_subtype = 0 when acknowledging EOF
 *   directive_subtype = 1 when acknowledging Finished
 * ---------------------------------------------------------------------- */

int cfdp2_encode_ack(const cfdp_ack_pdu_t *ack,
                     uint8_t *buf, size_t buf_len)
{
    if (buf_len < 3u) return CFDP_ERR_BUF_TOO_SMALL;

    buf[0] = CFDP_DIRECTIVE_ACK;
    buf[1] = (uint8_t)(
        (((uint8_t)ack->acked_directive  & 0x0Fu) << 4) |
        (((uint8_t)ack->directive_subtype & 0x03u) << 2)
    );
    buf[2] = (uint8_t)(
        (((uint8_t)ack->condition_code     & 0x0Fu) << 4) |
        ( (uint8_t)ack->transaction_status & 0x07u)
    );
    return 3;
}

int cfdp2_decode_ack(const uint8_t *buf, size_t buf_len,
                     cfdp_ack_pdu_t *ack)
{
    if (buf_len < 3u) return CFDP_ERR_INVALID_PDU;
    if (buf[0] != CFDP_DIRECTIVE_ACK) return CFDP_ERR_INVALID_PDU;

    ack->acked_directive   = (cfdp_directive_code_t)((buf[1] >> 4) & 0x0Fu);
    ack->directive_subtype = (buf[1] >> 2) & 0x03u;
    ack->condition_code    = (cfdp_condition_code_t)((buf[2] >> 4) & 0x0Fu);
    ack->transaction_status= (cfdp_transaction_status_t)(buf[2] & 0x07u);
    return 3;
}

/* -----------------------------------------------------------------------
 * NAK PDU (5.2.6)
 *
 * Data field layout:
 *   Byte 0:    directive code = 0x08
 *   Bytes 1-4: scope_start (uint32, big-endian)
 *   Bytes 5-8: scope_end   (uint32, big-endian)
 *   For each gap [0 .. gap_count):
 *     Bytes +0..+3: segment_offset_start (uint32, big-endian)
 *     Bytes +4..+7: segment_offset_end   (uint32, big-endian)
 *
 * Total size = 9 + gap_count * 8 bytes.
 * ---------------------------------------------------------------------- */

int cfdp2_encode_nak(const cfdp_nak_pdu_t *nak,
                     uint8_t *buf, size_t buf_len)
{
    if (nak->gap_count > CFDP_MAX_NAK_GAPS) return CFDP_ERR_BUF_TOO_SMALL;

    size_t needed = 9u + (size_t)nak->gap_count * 8u;
    if (buf_len < needed) return CFDP_ERR_BUF_TOO_SMALL;

    int pos = 0;

    buf[pos++] = CFDP_DIRECTIVE_NAK;

    /* scope_start */
    buf[pos++] = (uint8_t)(nak->scope_start >> 24);
    buf[pos++] = (uint8_t)(nak->scope_start >> 16);
    buf[pos++] = (uint8_t)(nak->scope_start >>  8);
    buf[pos++] = (uint8_t)(nak->scope_start);

    /* scope_end */
    buf[pos++] = (uint8_t)(nak->scope_end >> 24);
    buf[pos++] = (uint8_t)(nak->scope_end >> 16);
    buf[pos++] = (uint8_t)(nak->scope_end >>  8);
    buf[pos++] = (uint8_t)(nak->scope_end);

    /* Gap pairs */
    for (uint32_t i = 0; i < nak->gap_count; i++) {
        uint32_t s = nak->gaps[i].start;
        uint32_t e = nak->gaps[i].end;

        buf[pos++] = (uint8_t)(s >> 24);
        buf[pos++] = (uint8_t)(s >> 16);
        buf[pos++] = (uint8_t)(s >>  8);
        buf[pos++] = (uint8_t)(s);

        buf[pos++] = (uint8_t)(e >> 24);
        buf[pos++] = (uint8_t)(e >> 16);
        buf[pos++] = (uint8_t)(e >>  8);
        buf[pos++] = (uint8_t)(e);
    }

    return pos;
}

int cfdp2_decode_nak(const uint8_t *buf, size_t buf_len,
                     cfdp_nak_pdu_t *nak)
{
    if (buf_len < 9u) return CFDP_ERR_INVALID_PDU;
    if (buf[0] != CFDP_DIRECTIVE_NAK) return CFDP_ERR_INVALID_PDU;

    int pos = 1;

    nak->scope_start =
        ((uint32_t)buf[pos  ] << 24) | ((uint32_t)buf[pos+1] << 16) |
        ((uint32_t)buf[pos+2] <<  8) |  (uint32_t)buf[pos+3];
    pos += 4;

    nak->scope_end =
        ((uint32_t)buf[pos  ] << 24) | ((uint32_t)buf[pos+1] << 16) |
        ((uint32_t)buf[pos+2] <<  8) |  (uint32_t)buf[pos+3];
    pos += 4;

    /* Each gap pair is 8 bytes */
    size_t remaining = buf_len - (size_t)pos;
    uint32_t gap_count = (uint32_t)(remaining / 8u);

    if (gap_count > CFDP_MAX_NAK_GAPS) {
        /* Truncate silently — we handle what we can */
        gap_count = CFDP_MAX_NAK_GAPS;
    }
    nak->gap_count = gap_count;

    for (uint32_t i = 0; i < gap_count; i++) {
        nak->gaps[i].start =
            ((uint32_t)buf[pos  ] << 24) | ((uint32_t)buf[pos+1] << 16) |
            ((uint32_t)buf[pos+2] <<  8) |  (uint32_t)buf[pos+3];
        pos += 4;

        nak->gaps[i].end =
            ((uint32_t)buf[pos  ] << 24) | ((uint32_t)buf[pos+1] << 16) |
            ((uint32_t)buf[pos+2] <<  8) |  (uint32_t)buf[pos+3];
        pos += 4;
    }

    return pos;
}