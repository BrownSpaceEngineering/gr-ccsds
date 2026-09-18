/*
 * cfdp_class2_sender.c — Class 2 (Acknowledged) sender state machine
 *
 * State transitions:
 *
 *   IDLE: cfdp2_sender_start() -> SEND_METADATA
 *
 *   SEND_METADATA: step(): send Metadata PDU -> SEND_FILE_DATA
 *
 *   SEND_FILE_DATA: step(): send next segment
 *          all bytes sent? -> SEND_EOF
 *          else stay in SEND_FILE_DATA
 *
 *   SEND_EOF: step(): send EOF PDU, start ACK timer ► AWAIT_EOF_ACK
 *
 *   AWAIT_EOF_ACK: step(): poll recv()
 *          ACK(EOF) received ->► AWAIT_FINISHED
 *          NAK received (before ACK) -> (buffer gaps, stay)
 *          timer expired, retry < limit -> SEND_EOF (resend)
 *          timer expired, retry >= limit -> CANCELLED
 *
 *   AWAIT_FINISHED: step(): poll recv()
 *          NAK received -> RETRANSMIT
 *          Finished received -> send ACK(Finished) → FINISHED
 *          inactivity timeout -> CANCELLED
 *
 *   RETRANSMIT: step(): replay one segment per call from nak_gaps[]
 *          all gaps replayed -> AWAIT_FINISHED
 *
 *   FINISHED / CANCELLED - terminal
 *
 * Receiving PDUs in AWAIT_EOF_ACK and AWAIT_FINISHED:
 *   We use non-blocking recv (transport returns 0 on timeout) so step()
 *   returns quickly and the caller can poll in a tight loop.  The UDP
 *   transport already sets SO_RCVTIMEO = 2 s, which is fine.
 *
 * NAK handling:
 *   NAKs carry a list of missing byte-range gaps.  We store all pending
 *   gaps in s->nak_gaps[].  In RETRANSMIT we seek the file to each gap
 *   and re-send segments until the gap is covered, advancing
 *   s->retransmit_offset within the current gap each step() call.
 */

#include "cfdp_class2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * Internal: build and send a complete PDU (same pattern as cfdp_sender.c)
 * ---------------------------------------------------------------------- */

static cfdp_status_t send_pdu(cfdp2_sender_t *s,
                              cfdp_pdu_type_t pdu_type,
                              cfdp_direction_t direction,
                              const uint8_t *data_field,
                              uint16_t data_len)
{
    uint8_t buf[CFDP_MAX_PDU_SIZE];

    cfdp_pdu_header_t hdr = {
        .version             = 1,
        .pdu_type            = pdu_type,
        .direction           = direction,
        .tx_mode             = CFDP_TX_MODE_ACKNOWLEDGED,   /* Class 2 */
        .crc_flag            = false,
        .large_file          = false,
        .data_field_len      = data_len,
        .entity_id_len       = s->entity_id_len,
        .seq_num_len         = s->seq_num_len,
        .source_entity_id    = s->source_entity_id,
        .dest_entity_id      = s->dest_entity_id,
        .transaction_seq_num = s->seq_num,
    };

    int hdr_len = cfdp_encode_header(&hdr, buf, sizeof(buf));
    if (hdr_len < 0) return (cfdp_status_t)hdr_len;

    if ((size_t)hdr_len + data_len > sizeof(buf))
        return CFDP_ERR_BUF_TOO_SMALL;

    memcpy(buf + hdr_len, data_field, data_len);

    return s->transport->send(s->transport, buf, (size_t)hdr_len + data_len);
}

/* -----------------------------------------------------------------------
 * Internal: send one EOF PDU and start/restart the ACK timer
 * ---------------------------------------------------------------------- */

static cfdp_status_t do_send_eof(cfdp2_sender_t *s)
{
    uint8_t data_field[CFDP_MAX_PDU_SIZE];

    cfdp_eof_pdu_t eof_pdu = {
        .condition_code = CFDP_COND_NO_ERROR,
        .checksum       = s->checksum,
        .file_size      = s->file_size,
    };

    int encoded = cfdp_encode_eof(&eof_pdu, data_field, sizeof(data_field));
    if (encoded < 0) return (cfdp_status_t)encoded;

    cfdp_status_t rc = send_pdu(s, CFDP_PDU_FILE_DIRECTIVE,
                                CFDP_DIR_TOWARD_RECEIVER,
                                data_field, (uint16_t)encoded);
    if (rc != CFDP_OK) return rc;

    printf("[SENDER2] Sent EOF: checksum=0x%08X size=%u (retry=%d)\n",
           s->checksum, s->file_size, s->eof_retry_count);

    s->eof_sent_time = time(NULL);
    return CFDP_OK;
}

/* -----------------------------------------------------------------------
 * Internal: send ACK(Finished) — the last PDU of the transaction
 * ---------------------------------------------------------------------- */

static cfdp_status_t send_ack_finished(cfdp2_sender_t *s)
{
    uint8_t data_field[8];

    cfdp_ack_pdu_t ack = {
        .acked_directive   = CFDP_DIRECTIVE_FINISHED,
        .directive_subtype = 1,   /* subtype 1 for ACK(Finished) */
        .condition_code    = CFDP_COND_NO_ERROR,
        .transaction_status= CFDP_TX_STATUS_TERMINATED,
    };

    int encoded = cfdp2_encode_ack(&ack, data_field, sizeof(data_field));
    if (encoded < 0) return (cfdp_status_t)encoded;

    cfdp_status_t rc = send_pdu(s, CFDP_PDU_FILE_DIRECTIVE,
                                CFDP_DIR_TOWARD_RECEIVER,
                                data_field, (uint16_t)encoded);
    if (rc != CFDP_OK) return rc;

    printf("[SENDER2] Sent ACK(Finished)\n");
    return CFDP_OK;
}

/* -----------------------------------------------------------------------
 * Internal: ingest one incoming PDU while waiting for ACK / Finished.
 * Returns CFDP_OK on success (even if the PDU was ignored).
 * Sets *got_eof_ack, *got_finished, or appends to s->nak_gaps[].
 * ---------------------------------------------------------------------- */

static cfdp_status_t process_incoming(cfdp2_sender_t *s,
                                      const uint8_t *buf, size_t len,
                                      bool *got_eof_ack,
                                      bool *got_finished)
{
    *got_eof_ack  = false;
    *got_finished = false;

    cfdp_pdu_header_t hdr;
    int hdr_len = cfdp_decode_header(buf, len, &hdr);
    if (hdr_len < 0) {
        fprintf(stderr, "[SENDER2] Bad PDU header — ignored\n");
        return CFDP_OK;
    }

    /* Only accept PDUs from our active transaction */
    if (hdr.source_entity_id != s->dest_entity_id ||
        hdr.dest_entity_id   != s->source_entity_id ||
        hdr.transaction_seq_num != s->seq_num) {
        fprintf(stderr, "[SENDER2] PDU from unknown transaction — ignored\n");
        return CFDP_OK;
    }

    if (hdr.pdu_type != CFDP_PDU_FILE_DIRECTIVE) {
        fprintf(stderr, "[SENDER2] Unexpected file-data PDU — ignored\n");
        return CFDP_OK;
    }

    const uint8_t *data  = buf + hdr_len;
    uint16_t       dlen  = hdr.data_field_len;

    if ((size_t)hdr_len + dlen > len || dlen < 1u)
        return CFDP_ERR_INVALID_PDU;

    uint8_t code = data[0];

    if (code == CFDP_DIRECTIVE_ACK) {
        cfdp_ack_pdu_t ack;
        if (cfdp2_decode_ack(data, dlen, &ack) < 0) return CFDP_OK;

        if (ack.acked_directive == CFDP_DIRECTIVE_EOF) {
            printf("[SENDER2] Received ACK(EOF)\n");
            *got_eof_ack = true;
        }
        /* ACK(Finished) can arrive in AWAIT_FINISHED but we don't
           need to act on it — receiving Finished is sufficient. */

    } else if (code == CFDP_DIRECTIVE_NAK) {
        cfdp_nak_pdu_t nak;
        if (cfdp2_decode_nak(data, dlen, &nak) < 0) return CFDP_OK;

        printf("[SENDER2] Received NAK: %u gap(s)\n", nak.gap_count);

        /* Merge NAK gaps into our pending list, avoiding duplicates.
         * Simple strategy: append only gaps not already present. */
        for (uint32_t i = 0; i < nak.gap_count; i++) {
            /* Check for an exact duplicate */
            bool duplicate = false;
            for (uint32_t j = 0; j < s->nak_gap_count; j++) {
                if (s->nak_gaps[j].start == nak.gaps[i].start &&
                    s->nak_gaps[j].end   == nak.gaps[i].end) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && s->nak_gap_count < CFDP_MAX_NAK_GAPS) {
                s->nak_gaps[s->nak_gap_count++] = nak.gaps[i];
                printf("[SENDER2]   gap [%u, %u)\n",
                       nak.gaps[i].start, nak.gaps[i].end);
            }
        }

    } else if (code == CFDP_DIRECTIVE_FINISHED) {
        cfdp_finished_pdu_t fin;
        if (cfdp2_decode_finished(data, dlen, &fin) < 0) return CFDP_OK;

        printf("[SENDER2] Received Finished: delivery=%s\n",
               fin.delivery_code == CFDP_DELIVERY_COMPLETE
                   ? "COMPLETE" : "INCOMPLETE");
        *got_finished = true;

    } else {
        fprintf(stderr, "[SENDER2] Unexpected directive 0x%02X — ignored\n",
                code);
    }

    return CFDP_OK;
}

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

cfdp_status_t cfdp2_sender_init(cfdp2_sender_t *s,
                                uint32_t source_entity_id,
                                uint32_t dest_entity_id,
                                uint32_t seq_num,
                                uint8_t  entity_id_len,
                                uint8_t  seq_num_len,
                                uint16_t segment_size,
                                cfdp_transport_t *transport)
{
    memset(s, 0, sizeof(*s));
    s->state             = CFDP2_SENDER_IDLE;
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

cfdp_status_t cfdp2_sender_start(cfdp2_sender_t *s,
                                 const char *src_path,
                                 const char *dst_filename)
{
    if (s->state != CFDP2_SENDER_IDLE) return CFDP_ERR_STATE;

    struct stat st;
    if (stat(src_path, &st) != 0) {
        perror("cfdp2_sender_start: stat");
        return CFDP_ERR_IO;
    }
    s->file_size = (uint32_t)st.st_size;

    s->file_fd = open(src_path, O_RDONLY);
    if (s->file_fd < 0) {
        perror("cfdp2_sender_start: open");
        return CFDP_ERR_IO;
    }

    strncpy(s->src_filename, src_path,     CFDP_MAX_FILENAME_LEN);
    strncpy(s->dst_filename, dst_filename,  CFDP_MAX_FILENAME_LEN);

    s->bytes_sent      = 0;
    s->checksum        = CFDP_MODULAR_SUM_INIT;
    s->nak_gap_count   = 0;
    s->eof_retry_count = 0;
    s->state           = CFDP2_SENDER_SEND_METADATA;
    return CFDP_OK;
}

cfdp_status_t cfdp2_sender_step(cfdp2_sender_t *s)
{
    uint8_t       data_field[CFDP_MAX_PDU_SIZE];
    int           encoded;
    cfdp_status_t rc;

    switch (s->state) {

    case CFDP2_SENDER_SEND_METADATA: {
        cfdp_metadata_pdu_t meta = {
            .closure_requested = true,   /* Class 2 always requests closure */
            .checksum_type     = CFDP_CHECKSUM_MODULAR,
            .file_size         = s->file_size,
        };
        strncpy(meta.src_filename, s->src_filename, CFDP_MAX_FILENAME_LEN);
        strncpy(meta.dst_filename, s->dst_filename, CFDP_MAX_FILENAME_LEN);

        encoded = cfdp_encode_metadata(&meta, data_field, sizeof(data_field));
        if (encoded < 0) return (cfdp_status_t)encoded;

        rc = send_pdu(s, CFDP_PDU_FILE_DIRECTIVE, CFDP_DIR_TOWARD_RECEIVER,
                      data_field, (uint16_t)encoded);
        if (rc != CFDP_OK) return rc;

        printf("[SENDER2] Sent Metadata: '%s' → '%s', size=%u\n",
               s->src_filename, s->dst_filename, s->file_size);

        s->state = CFDP2_SENDER_SEND_FILE_DATA;
        return CFDP_OK;
    }

    case CFDP2_SENDER_SEND_FILE_DATA: {
        uint8_t file_buf[CFDP_FILE_DATA_LIMIT];
        ssize_t nread = read(s->file_fd, file_buf, s->segment_size);

        if (nread < 0) {
            perror("cfdp2_sender_step: read");
            return CFDP_ERR_IO;
        }
        if (nread == 0) {
            s->state = CFDP2_SENDER_SEND_EOF;
            return CFDP_OK;
        }

        s->checksum = cfdp_checksum_modular(s->checksum,
                                            file_buf, (size_t)nread);

        cfdp_file_data_pdu_t fd_pdu = {
            .offset   = s->bytes_sent,
            .data     = file_buf,
            .data_len = (uint16_t)nread,
        };

        encoded = cfdp_encode_file_data(&fd_pdu, data_field, sizeof(data_field));
        if (encoded < 0) return (cfdp_status_t)encoded;

        rc = send_pdu(s, CFDP_PDU_FILE_DATA, CFDP_DIR_TOWARD_RECEIVER,
                      data_field, (uint16_t)encoded);
        if (rc != CFDP_OK) return rc;

        s->bytes_sent += (uint32_t)nread;

        printf("[SENDER2] Sent File Data: offset=%-6u len=%-4u  "
               "(%u / %u bytes)\n",
               fd_pdu.offset, fd_pdu.data_len,
               s->bytes_sent, s->file_size);

        if (s->bytes_sent >= s->file_size)
            s->state = CFDP2_SENDER_SEND_EOF;

        return CFDP_OK;
    }

    case CFDP2_SENDER_SEND_EOF: {
        rc = do_send_eof(s);
        if (rc != CFDP_OK) return rc;

        s->state = CFDP2_SENDER_AWAIT_EOF_ACK;
        return CFDP_OK;
    }

    case CFDP2_SENDER_AWAIT_EOF_ACK: {
        /*
         * Poll the transport for one incoming PDU.
         * The transport's recv() returns 0 on timeout (non-blocking),
         * so we return CFDP_OK and let the caller loop.
         */
        uint8_t recv_buf[CFDP_MAX_PDU_SIZE];
        int n = s->transport->recv(s->transport, recv_buf, sizeof(recv_buf));
        if (n < 0) return CFDP_ERR_TRANSPORT;

        if (n > 0) {
            bool got_eof_ack = false, got_finished = false;
            rc = process_incoming(s, recv_buf, (size_t)n,
                                  &got_eof_ack, &got_finished);
            if (rc != CFDP_OK) return rc;

            if (got_eof_ack) {
                s->state = CFDP2_SENDER_AWAIT_FINISHED;
                s->finished_wait_start = time(NULL);
                return CFDP_OK;
            }
            /* A Finished PDU before ACK(EOF) is unusual but valid —
               the receiver might skip sending the ACK(EOF) on some
               implementations.  Treat it as implying the ACK. */
            if (got_finished) {
                rc = send_ack_finished(s);
                if (rc != CFDP_OK) return rc;
                s->state = CFDP2_SENDER_FINISHED;
                return CFDP_OK;
            }
            /* NAK received — buffer the gaps; wait for ACK(EOF) still. */
        }

        /* Check ACK timer */
        time_t elapsed = time(NULL) - s->eof_sent_time;
        if (elapsed >= CFDP_ACK_TIMEOUT_S) {
            if (s->eof_retry_count >= CFDP_ACK_RETRY_LIMIT) {
                fprintf(stderr,
                        "[SENDER2] ACK timeout: retry limit reached — CANCELLED\n");
                s->state = CFDP2_SENDER_CANCELLED;
                return CFDP_ERR_TRANSPORT;
            }
            fprintf(stderr,
                    "[SENDER2] ACK timeout — resending EOF (retry %d/%d)\n",
                    s->eof_retry_count + 1, CFDP_ACK_RETRY_LIMIT);
            s->eof_retry_count++;
            /* Re-enter SEND_EOF to retransmit, then come back here */
            s->state = CFDP2_SENDER_SEND_EOF;
        }

        return CFDP_OK;
    }

    case CFDP2_SENDER_AWAIT_FINISHED: {
        uint8_t recv_buf[CFDP_MAX_PDU_SIZE];
        int n = s->transport->recv(s->transport, recv_buf, sizeof(recv_buf));
        if (n < 0) return CFDP_ERR_TRANSPORT;

        if (n > 0) {
            bool got_eof_ack = false, got_finished = false;
            rc = process_incoming(s, recv_buf, (size_t)n,
                                  &got_eof_ack, &got_finished);
            if (rc != CFDP_OK) return rc;

            if (got_finished) {
                rc = send_ack_finished(s);
                if (rc != CFDP_OK) return rc;
                s->state = CFDP2_SENDER_FINISHED;
                return CFDP_OK;
            }

            /* If we got a NAK, switch to retransmit mode */
            if (s->nak_gap_count > 0) {
                s->retransmit_gap_idx  = 0;
                s->retransmit_offset   = s->nak_gaps[0].start;
                s->state = CFDP2_SENDER_RETRANSMIT;
                return CFDP_OK;
            }
        }

        /* Inactivity check: if we've been waiting too long, cancel */
        time_t elapsed = time(NULL) - s->finished_wait_start;
        if (elapsed >= (time_t)(CFDP_ACK_TIMEOUT_S * (CFDP_ACK_RETRY_LIMIT + 1))) {
            fprintf(stderr,
                    "[SENDER2] Inactivity timeout waiting for Finished — CANCELLED\n");
            s->state = CFDP2_SENDER_CANCELLED;
            return CFDP_ERR_TRANSPORT;
        }

        return CFDP_OK;
    }

    case CFDP2_SENDER_RETRANSMIT: {
        /*
         * Retransmit one segment per step() call from the current gap.
         * We seek to retransmit_offset in the file and read segment_size
         * bytes (or until the gap end, whichever is smaller).
         */
        if (s->retransmit_gap_idx >= s->nak_gap_count) {
            /* All gaps replayed — clear the list and wait again */
            s->nak_gap_count = 0;
            s->finished_wait_start = time(NULL);   /* reset inactivity timer */
            s->state = CFDP2_SENDER_AWAIT_FINISHED;
            return CFDP_OK;
        }

        cfdp_nak_gap_t *gap = &s->nak_gaps[s->retransmit_gap_idx];

        /* Seek to the current retransmission offset */
        if (lseek(s->file_fd, (off_t)s->retransmit_offset, SEEK_SET) < 0) {
            perror("cfdp2_sender_step: lseek");
            return CFDP_ERR_IO;
        }

        uint32_t remaining_in_gap = gap->end - s->retransmit_offset;
        uint16_t to_read = (remaining_in_gap < s->segment_size)
                           ? (uint16_t)remaining_in_gap
                           : s->segment_size;

        uint8_t file_buf[CFDP_FILE_DATA_LIMIT];
        ssize_t nread = read(s->file_fd, file_buf, to_read);
        if (nread <= 0) {
            perror("cfdp2_sender_step: retransmit read");
            return CFDP_ERR_IO;
        }

        cfdp_file_data_pdu_t fd_pdu = {
            .offset   = s->retransmit_offset,
            .data     = file_buf,
            .data_len = (uint16_t)nread,
        };

        uint8_t df[CFDP_MAX_PDU_SIZE];
        encoded = cfdp_encode_file_data(&fd_pdu, df, sizeof(df));
        if (encoded < 0) return (cfdp_status_t)encoded;

        rc = send_pdu(s, CFDP_PDU_FILE_DATA, CFDP_DIR_TOWARD_RECEIVER,
                      df, (uint16_t)encoded);
        if (rc != CFDP_OK) return rc;

        printf("[SENDER2] Retransmit: offset=%-6u len=%-4u  "
               "(gap %u [%u,%u))\n",
               s->retransmit_offset, (uint16_t)nread,
               s->retransmit_gap_idx, gap->start, gap->end);

        s->retransmit_offset += (uint32_t)nread;

        /* Advance to next gap when this one is fully covered */
        if (s->retransmit_offset >= gap->end) {
            s->retransmit_gap_idx++;
            if (s->retransmit_gap_idx < s->nak_gap_count) {
                s->retransmit_offset =
                    s->nak_gaps[s->retransmit_gap_idx].start;
            }
        }

        return CFDP_OK;
    }

    case CFDP2_SENDER_FINISHED:
    case CFDP2_SENDER_CANCELLED:
        return CFDP_ERR_STATE;

    default:
        return CFDP_ERR_STATE;
    }
}

cfdp_status_t cfdp2_sender_run(cfdp2_sender_t *s)
{
    cfdp_status_t rc;
    while (s->state != CFDP2_SENDER_FINISHED &&
           s->state != CFDP2_SENDER_CANCELLED) {
        rc = cfdp2_sender_step(s);
        if (rc != CFDP_OK) return rc;
    }
    return (s->state == CFDP2_SENDER_FINISHED) ? CFDP_OK : CFDP_ERR_TRANSPORT;
}

void cfdp2_sender_destroy(cfdp2_sender_t *s)
{
    if (s->file_fd >= 0) {
        close(s->file_fd);
        s->file_fd = -1;
    }
}

const char *cfdp2_sender_state_str(cfdp2_sender_state_t s)
{
    switch (s) {
    case CFDP2_SENDER_IDLE:            return "IDLE";
    case CFDP2_SENDER_SEND_METADATA:   return "SEND_METADATA";
    case CFDP2_SENDER_SEND_FILE_DATA:  return "SEND_FILE_DATA";
    case CFDP2_SENDER_SEND_EOF:        return "SEND_EOF";
    case CFDP2_SENDER_AWAIT_EOF_ACK:   return "AWAIT_EOF_ACK";
    case CFDP2_SENDER_RETRANSMIT:      return "RETRANSMIT";
    case CFDP2_SENDER_AWAIT_FINISHED:  return "AWAIT_FINISHED";
    case CFDP2_SENDER_FINISHED:        return "FINISHED";
    case CFDP2_SENDER_CANCELLED:       return "CANCELLED";
    default:                           return "UNKNOWN";
    }
}