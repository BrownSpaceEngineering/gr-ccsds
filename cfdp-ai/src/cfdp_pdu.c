/*
 * cfdp_pdu.c — PDU serialization / deserialization
 *
 * Every encode function returns the number of bytes written.
 * Every decode function returns the number of bytes consumed.
 * Both return a negative cfdp_status_t on error.
 *
 * Wire format follows CCSDS 727.0-B-5 5.1–5.2.
 */

#include "cfdp.h"

#include <string.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Internal helpers: big-endian integer encode / decode
 * ---------------------------------------------------------------------- */

/* Write n bytes of value v in big-endian into buf. Returns n. */
static int write_be(uint8_t *buf, size_t buf_len, uint32_t v, uint8_t n)
{
    if ((size_t)n > buf_len) return CFDP_ERR_BUF_TOO_SMALL;
    for (int i = n - 1; i >= 0; i--) {
        buf[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
    return n;
}

/* Read n bytes from buf as big-endian uint32. Returns n. */
static int read_be(const uint8_t *buf, size_t buf_len,
                   uint32_t *out, uint8_t n)
{
    if ((size_t)n > buf_len) return CFDP_ERR_INVALID_PDU;
    *out = 0;
    for (int i = 0; i < n; i++) {
        *out = (*out << 8) | buf[i];
    }
    return n;
}

/* -----------------------------------------------------------------------
 * Fixed PDU Header
 *
 * Byte 0:  [2:1][1:pdu_type][1:direction][1:tx_mode][1:crc_flag]
 *          [1:large_file][1:reserved=0]  (version=001 in top 3 bits)
 *
 * Byte 1-2: PDU data field length (uint16, big-endian)
 *
 * Byte 3:  [7:segmentation_ctrl=0][6-4:entity_id_len-1][3:seg_meta=0]
 *          [2-0:seq_num_len-1]
 *
 * Bytes 4..4+eid-1:            source entity ID
 * Bytes ..+sn_len:             transaction sequence number
 * Bytes ..+eid:                destination entity ID
 * ---------------------------------------------------------------------- */

int cfdp_encode_header(const cfdp_pdu_header_t *hdr,
                       uint8_t *buf, size_t buf_len)
{
    /* Minimum fixed header without IDs is 4 bytes, then variable part */
    size_t header_size = 4u
        + (size_t)hdr->entity_id_len   /* source entity ID  */
        + (size_t)hdr->seq_num_len     /* transaction seq   */
        + (size_t)hdr->entity_id_len;  /* dest entity ID    */

    if (buf_len < header_size) return CFDP_ERR_BUF_TOO_SMALL;

    /* Byte 0 */
    buf[0] = (uint8_t)(
        ((hdr->version & 0x07u) << 5)         |
        ((hdr->pdu_type  & 0x01u) << 4)       |
        ((hdr->direction & 0x01u) << 3)        |
        ((hdr->tx_mode   & 0x01u) << 2)        |
        ((hdr->crc_flag  ? 1u : 0u) << 1)     |
        ( hdr->large_file ? 1u : 0u)
    );

    /* Bytes 1-2: data field length */
    buf[1] = (uint8_t)(hdr->data_field_len >> 8);
    buf[2] = (uint8_t)(hdr->data_field_len & 0xFF);

    /* Byte 3: lengths nibble */
    uint8_t eid_code = (uint8_t)((hdr->entity_id_len - 1u) & 0x07u);
    uint8_t sn_code  = (uint8_t)((hdr->seq_num_len  - 1u) & 0x07u);
    buf[3] = (uint8_t)((eid_code << 4) | sn_code);

    int pos = 4;

    /* Source entity ID */
    int r = write_be(buf + pos, buf_len - (size_t)pos,
                     hdr->source_entity_id, hdr->entity_id_len);
    if (r < 0) return r;
    pos += r;

    /* Transaction sequence number */
    r = write_be(buf + pos, buf_len - (size_t)pos,
                 hdr->transaction_seq_num, hdr->seq_num_len);
    if (r < 0) return r;
    pos += r;

    /* Destination entity ID */
    r = write_be(buf + pos, buf_len - (size_t)pos,
                 hdr->dest_entity_id, hdr->entity_id_len);
    if (r < 0) return r;
    pos += r;

    return pos;  /* total bytes written */
}

int cfdp_decode_header(const uint8_t *buf, size_t buf_len,
                       cfdp_pdu_header_t *hdr)
{
    if (buf_len < 4) return CFDP_ERR_INVALID_PDU;

    hdr->version   = (buf[0] >> 5) & 0x07u;
    hdr->pdu_type  = (cfdp_pdu_type_t)  ((buf[0] >> 4) & 0x01u);
    hdr->direction = (cfdp_direction_t) ((buf[0] >> 3) & 0x01u);
    hdr->tx_mode   = (cfdp_tx_mode_t)   ((buf[0] >> 2) & 0x01u);
    hdr->crc_flag  = (buf[0] >> 1) & 0x01u;
    hdr->large_file= buf[0] & 0x01u;

    hdr->data_field_len = (uint16_t)((buf[1] << 8) | buf[2]);

    hdr->entity_id_len = (uint8_t)(((buf[3] >> 4) & 0x07u) + 1u);
    hdr->seq_num_len   = (uint8_t)(( buf[3]        & 0x07u) + 1u);

    /* Validate length fields are 1, 2, or 4 */
    if (hdr->entity_id_len != 1 && hdr->entity_id_len != 2 &&
        hdr->entity_id_len != 4) return CFDP_ERR_INVALID_PDU;
    if (hdr->seq_num_len   != 1 && hdr->seq_num_len   != 2 &&
        hdr->seq_num_len   != 4) return CFDP_ERR_INVALID_PDU;

    size_t min_len = 4u + (size_t)hdr->entity_id_len * 2u
                        + (size_t)hdr->seq_num_len;
    if (buf_len < min_len) return CFDP_ERR_INVALID_PDU;

    int pos = 4;

    int r = read_be(buf + pos, buf_len - (size_t)pos,
                    &hdr->source_entity_id, hdr->entity_id_len);
    if (r < 0) return r;
    pos += r;

    r = read_be(buf + pos, buf_len - (size_t)pos,
                &hdr->transaction_seq_num, hdr->seq_num_len);
    if (r < 0) return r;
    pos += r;

    r = read_be(buf + pos, buf_len - (size_t)pos,
                &hdr->dest_entity_id, hdr->entity_id_len);
    if (r < 0) return r;
    pos += r;

    return pos;
}

/* -----------------------------------------------------------------------
 * Metadata PDU data field (5.2.5)
 *
 * Byte 0:    [7:closure_requested][6-4:reserved][3-0:checksum_type]
 * Bytes 1-4: file size (uint32, big-endian)
 * Byte 5:    source filename length LV (Length-Value)
 * Bytes ...: source filename (not NUL-terminated on wire)
 * Byte ...:  dest filename length LV
 * Bytes ...: dest filename
 * ---------------------------------------------------------------------- */

int cfdp_encode_metadata(const cfdp_metadata_pdu_t *meta,
                         uint8_t *buf, size_t buf_len)
{
    uint8_t src_len = (uint8_t)strlen(meta->src_filename);
    uint8_t dst_len = (uint8_t)strlen(meta->dst_filename);

    /* Directive code + flags byte + 4 file-size bytes + 2 LV headers */
    size_t needed = 1u + 1u + 4u + 1u + src_len + 1u + dst_len;
    if (buf_len < needed) return CFDP_ERR_BUF_TOO_SMALL;

    int pos = 0;

    /* Directive code */
    buf[pos++] = CFDP_DIRECTIVE_METADATA;

    /* Flags byte */
    buf[pos++] = (uint8_t)(
        ((meta->closure_requested ? 1u : 0u) << 7) |
        ((uint8_t)(meta->checksum_type) & 0x0Fu)
    );

    /* File size */
    buf[pos++] = (uint8_t)(meta->file_size >> 24);
    buf[pos++] = (uint8_t)(meta->file_size >> 16);
    buf[pos++] = (uint8_t)(meta->file_size >>  8);
    buf[pos++] = (uint8_t)(meta->file_size);

    /* Source filename LV */
    buf[pos++] = src_len;
    memcpy(buf + pos, meta->src_filename, src_len);
    pos += src_len;

    /* Dest filename LV */
    buf[pos++] = dst_len;
    memcpy(buf + pos, meta->dst_filename, dst_len);
    pos += dst_len;

    return pos;
}

int cfdp_decode_metadata(const uint8_t *buf, size_t buf_len,
                         cfdp_metadata_pdu_t *meta)
{
    if (buf_len < 7u) return CFDP_ERR_INVALID_PDU;  /* min plausible */
    if (buf[0] != CFDP_DIRECTIVE_METADATA) return CFDP_ERR_INVALID_PDU;

    int pos = 1;  /* skip directive code */

    uint8_t flags = buf[pos++];
    meta->closure_requested = (flags >> 7) & 0x01u;
    meta->checksum_type = (cfdp_checksum_type_t)(flags & 0x0Fu);

    /* File size */
    meta->file_size =
        ((uint32_t)buf[pos]   << 24) |
        ((uint32_t)buf[pos+1] << 16) |
        ((uint32_t)buf[pos+2] <<  8) |
        ((uint32_t)buf[pos+3]);
    pos += 4;

    /* Source filename LV */
    if ((size_t)pos >= buf_len) return CFDP_ERR_INVALID_PDU;
    uint8_t src_len = buf[pos++];
    /* src_len is uint8_t [0,255] and CFDP_MAX_FILENAME_LEN == 255, so always fits */
    if ((size_t)(pos + src_len) > buf_len) return CFDP_ERR_INVALID_PDU;
    memcpy(meta->src_filename, buf + pos, src_len);
    meta->src_filename[src_len] = '\0';
    pos += src_len;

    /* Dest filename LV */
    if ((size_t)pos >= buf_len) return CFDP_ERR_INVALID_PDU;
    uint8_t dst_len = buf[pos++];
    /* dst_len is uint8_t [0,255] and CFDP_MAX_FILENAME_LEN == 255, so always fits */
    if ((size_t)(pos + dst_len) > buf_len) return CFDP_ERR_INVALID_PDU;
    memcpy(meta->dst_filename, buf + pos, dst_len);
    meta->dst_filename[dst_len] = '\0';
    pos += dst_len;

    return pos;
}

/* -----------------------------------------------------------------------
 * File Data PDU data field (5.2.1)
 *
 * Bytes 0-3: offset (uint32, big-endian)
 * Bytes 4..: file data
 * ---------------------------------------------------------------------- */

int cfdp_encode_file_data(const cfdp_file_data_pdu_t *pdu,
                          uint8_t *buf, size_t buf_len)
{
    size_t needed = 4u + (size_t)pdu->data_len;
    if (buf_len < needed) return CFDP_ERR_BUF_TOO_SMALL;

    buf[0] = (uint8_t)(pdu->offset >> 24);
    buf[1] = (uint8_t)(pdu->offset >> 16);
    buf[2] = (uint8_t)(pdu->offset >>  8);
    buf[3] = (uint8_t)(pdu->offset);

    memcpy(buf + 4, pdu->data, pdu->data_len);
    return (int)(4 + pdu->data_len);
}

int cfdp_decode_file_data(const uint8_t *buf, size_t buf_len,
                          cfdp_file_data_pdu_t *pdu)
{
    if (buf_len < 4u) return CFDP_ERR_INVALID_PDU;

    pdu->offset =
        ((uint32_t)buf[0] << 24) |
        ((uint32_t)buf[1] << 16) |
        ((uint32_t)buf[2] <<  8) |
        ((uint32_t)buf[3]);

    pdu->data     = buf + 4;
    pdu->data_len = (uint16_t)(buf_len - 4u);
    return (int)buf_len;
}

/* -----------------------------------------------------------------------
 * EOF PDU data field (5.2.4)
 *
 * Byte 0:    directive code = 0x04
 * Byte 1:    [7-4: condition code][3-0: spare]
 * Bytes 2-5: checksum (uint32, big-endian)
 * Bytes 6-9: file size (uint32, big-endian)
 * ---------------------------------------------------------------------- */

int cfdp_encode_eof(const cfdp_eof_pdu_t *eof,
                    uint8_t *buf, size_t buf_len)
{
    if (buf_len < 10u) return CFDP_ERR_BUF_TOO_SMALL;

    buf[0] = CFDP_DIRECTIVE_EOF;
    buf[1] = (uint8_t)(((uint8_t)eof->condition_code & 0x0Fu) << 4);

    buf[2] = (uint8_t)(eof->checksum >> 24);
    buf[3] = (uint8_t)(eof->checksum >> 16);
    buf[4] = (uint8_t)(eof->checksum >>  8);
    buf[5] = (uint8_t)(eof->checksum);

    buf[6] = (uint8_t)(eof->file_size >> 24);
    buf[7] = (uint8_t)(eof->file_size >> 16);
    buf[8] = (uint8_t)(eof->file_size >>  8);
    buf[9] = (uint8_t)(eof->file_size);

    return 10;
}

int cfdp_decode_eof(const uint8_t *buf, size_t buf_len,
                    cfdp_eof_pdu_t *eof)
{
    if (buf_len < 10u) return CFDP_ERR_INVALID_PDU;
    if (buf[0] != CFDP_DIRECTIVE_EOF) return CFDP_ERR_INVALID_PDU;

    eof->condition_code =
        (cfdp_condition_code_t)((buf[1] >> 4) & 0x0Fu);

    eof->checksum =
        ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
        ((uint32_t)buf[4] <<  8) |  (uint32_t)buf[5];

    eof->file_size =
        ((uint32_t)buf[6] << 24) | ((uint32_t)buf[7] << 16) |
        ((uint32_t)buf[8] <<  8) |  (uint32_t)buf[9];

    return 10;
}