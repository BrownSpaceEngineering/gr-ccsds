/*
 * cfdp_sender.c — Class 1 (Unacknowledged) sender state machine
 *
 * State transitions:
 *
 *   IDLE: cfdp_sender_start() -> SEND_METADATA
 *
 *   SEND_METADATA: step(): send Metadata PDU -> SEND_FILE_DATA
 *
 *   SEND_FILE_DATA: step(): send next File Data PDU - if all bytes sent -> SEND_EOF
 *
 *   SEND_EOF: step(): send EOF PDU -> FINISHED
 *
 *   FINISHED / CANCELLED — terminal
 */

#include "cfdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* -----------------------------------------------------------------------
 * Helper: build and transmit one complete PDU
 *
 * Fills in the fixed header (with data_field_len set to data_len),
 * appends the data field, and calls transport->send.
 * ---------------------------------------------------------------------- */

static cfdp_status_t send_pdu(cfdp_sender_t *s,
                              cfdp_pdu_type_t pdu_type,
                              const uint8_t *data_field,
                              uint16_t data_len)
{
    uint8_t buf[CFDP_MAX_PDU_SIZE];

    cfdp_pdu_header_t hdr = {
        .version            = 1,
        .pdu_type           = pdu_type,
        .direction          = CFDP_DIR_TOWARD_RECEIVER,
        .tx_mode            = CFDP_TX_MODE_UNACKNOWLEDGED,
        .crc_flag           = false,
        .large_file         = false,
        .data_field_len     = data_len,
        .entity_id_len      = s->entity_id_len,
        .seq_num_len        = s->seq_num_len,
        .source_entity_id   = s->source_entity_id,
        .dest_entity_id     = s->dest_entity_id,
        .transaction_seq_num= s->seq_num,
    };

    int hdr_len = cfdp_encode_header(&hdr, buf, sizeof(buf));
    if (hdr_len < 0) return (cfdp_status_t)hdr_len;

    if ((size_t)hdr_len + data_len > sizeof(buf))
        return CFDP_ERR_BUF_TOO_SMALL;

    memcpy(buf + hdr_len, data_field, data_len);

    return s->transport->send(s->transport,
                              buf, (size_t)hdr_len + data_len);
}

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

cfdp_status_t cfdp_sender_init(cfdp_sender_t *s,
                               uint32_t source_entity_id,
                               uint32_t dest_entity_id,
                               uint32_t seq_num,
                               uint8_t  entity_id_len,
                               uint8_t  seq_num_len,
                               uint16_t segment_size,
                               cfdp_transport_t *transport)
{
    memset(s, 0, sizeof(*s));
    s->state             = CFDP_SENDER_IDLE;
    s->source_entity_id  = source_entity_id;
    s->dest_entity_id    = dest_entity_id;
    s->seq_num           = seq_num;
    s->entity_id_len     = entity_id_len;
    s->seq_num_len       = seq_num_len;
    s->segment_size      = segment_size ? segment_size : 512u;
    s->transport         = transport;
    s->file_fd           = -1;
    return CFDP_OK;
}

cfdp_status_t cfdp_sender_start(cfdp_sender_t *s,
                                const char *src_path,
                                const char *dst_filename)
{
    if (s->state != CFDP_SENDER_IDLE) return CFDP_ERR_STATE;

    /* Determine file size */
    struct stat st;
    if (stat(src_path, &st) != 0) {
        perror("cfdp_sender_start: stat");
        return CFDP_ERR_IO;
    }
    s->file_size = (uint32_t)st.st_size;

    /* Open file */
    s->file_fd = open(src_path, O_RDONLY);
    if (s->file_fd < 0) {
        perror("cfdp_sender_start: open");
        return CFDP_ERR_IO;
    }

    /* Store filenames */
    strncpy(s->src_filename, src_path,    CFDP_MAX_FILENAME_LEN);
    strncpy(s->dst_filename, dst_filename, CFDP_MAX_FILENAME_LEN);

    s->bytes_sent = 0;
    s->checksum   = CFDP_MODULAR_SUM_INIT;
    s->state      = CFDP_SENDER_SEND_METADATA;
    return CFDP_OK;
}

cfdp_status_t cfdp_sender_step(cfdp_sender_t *s)
{
    uint8_t  data_field[CFDP_MAX_PDU_SIZE];
    int      encoded;
    cfdp_status_t rc;

    switch (s->state) {

    case CFDP_SENDER_SEND_METADATA: {
        cfdp_metadata_pdu_t meta = {
            .closure_requested = false,
            .checksum_type     = CFDP_CHECKSUM_MODULAR,
            .file_size         = s->file_size,
        };
        strncpy(meta.src_filename, s->src_filename, CFDP_MAX_FILENAME_LEN);
        strncpy(meta.dst_filename, s->dst_filename, CFDP_MAX_FILENAME_LEN);

        encoded = cfdp_encode_metadata(&meta,
                                        data_field, sizeof(data_field));
        if (encoded < 0) return (cfdp_status_t)encoded;

        rc = send_pdu(s, CFDP_PDU_FILE_DIRECTIVE,
                      data_field, (uint16_t)encoded);
        if (rc != CFDP_OK) return rc;

        printf("[SENDER] Sent Metadata: file='%s' → '%s', size=%u\n",
               s->src_filename, s->dst_filename, s->file_size);

        s->state = CFDP_SENDER_SEND_FILE_DATA;
        return CFDP_OK;
    }

    case CFDP_SENDER_SEND_FILE_DATA: {
        /* Read one segment from the file */
        uint8_t  file_buf[CFDP_FILE_DATA_LIMIT];
        ssize_t  nread = read(s->file_fd, file_buf, s->segment_size);

        if (nread < 0) {
            perror("cfdp_sender_step: read");
            return CFDP_ERR_IO;
        }

        if (nread == 0) {
            /* Reached EOF — no more data to send */
            s->state = CFDP_SENDER_SEND_EOF;
            return CFDP_OK;
        }

        /* Accumulate checksum before building PDU */
        s->checksum = cfdp_checksum_modular(s->checksum,
                                            file_buf, (size_t)nread);

        cfdp_file_data_pdu_t fd_pdu = {
            .offset   = s->bytes_sent,
            .data     = file_buf,
            .data_len = (uint16_t)nread,
        };

        encoded = cfdp_encode_file_data(&fd_pdu,
                                         data_field, sizeof(data_field));
        if (encoded < 0) return (cfdp_status_t)encoded;

        rc = send_pdu(s, CFDP_PDU_FILE_DATA,
                      data_field, (uint16_t)encoded);
        if (rc != CFDP_OK) return rc;

        s->bytes_sent += (uint32_t)nread;

        printf("[SENDER] Sent File Data: offset=%-6u len=%-4u  "
               "(%u / %u bytes)\n",
               fd_pdu.offset, fd_pdu.data_len,
               s->bytes_sent, s->file_size);

        /* If we've sent everything, advance to EOF */
        if (s->bytes_sent >= s->file_size) {
            s->state = CFDP_SENDER_SEND_EOF;
        }
        return CFDP_OK;
    }

    case CFDP_SENDER_SEND_EOF: {
        cfdp_eof_pdu_t eof_pdu = {
            .condition_code = CFDP_COND_NO_ERROR,
            .checksum       = s->checksum,
            .file_size      = s->file_size,
        };

        encoded = cfdp_encode_eof(&eof_pdu, data_field, sizeof(data_field));
        if (encoded < 0) return (cfdp_status_t)encoded;

        rc = send_pdu(s, CFDP_PDU_FILE_DIRECTIVE,
                      data_field, (uint16_t)encoded);
        if (rc != CFDP_OK) return rc;

        printf("[SENDER] Sent EOF: checksum=0x%08X, file_size=%u\n",
               s->checksum, s->file_size);

        s->state = CFDP_SENDER_FINISHED;
        return CFDP_OK;
    }

    case CFDP_SENDER_FINISHED:
    case CFDP_SENDER_CANCELLED:
        return CFDP_ERR_STATE;

    default:
        return CFDP_ERR_STATE;
    }
}

cfdp_status_t cfdp_sender_run(cfdp_sender_t *s)
{
    cfdp_status_t rc;
    while (s->state != CFDP_SENDER_FINISHED &&
           s->state != CFDP_SENDER_CANCELLED) {
        rc = cfdp_sender_step(s);
        if (rc != CFDP_OK) return rc;
    }
    return CFDP_OK;
}

void cfdp_sender_destroy(cfdp_sender_t *s)
{
    if (s->file_fd >= 0) {
        close(s->file_fd);
        s->file_fd = -1;
    }
}