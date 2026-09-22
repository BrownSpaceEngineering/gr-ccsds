/*
 * cfdp_class2_receiver.c — Class 2 (Acknowledged) receiver state machine
 *
 * State transitions:
 *
 *   IDLE: cfdp2_receiver_init() -> AWAIT_METADATA
 *
 *   AWAIT_METADATA:
 *      ingest(Metadata PDU)
 *          alloc rx_buf, gap_list={[0,file_size)} -> RECEIVING
 *      ingest(File Data PDU)
 *          no file size known yet: drop it, send NAK[0,0) asking for
 *          Metadata (rate-limited) -> stay AWAIT_METADATA
 *      ingest(EOF PDU)
 *          EOF carries file_size + checksum, so we can set up the buffer
 *          without Metadata: alloc rx_buf, gap_list={[0,file_size)},
 *          send ACK(EOF) + NAK (incl. [0,0) Metadata request) -> AWAIT_DATA
 *
 *   RECEIVING:
 *      ingest(File Data PDU)
 *          update gap_list -> stay RECEIVING
 *      ingest(EOF PDU)
 *          send ACK(EOF) -> SEND_FINISHED (if no gaps)
 *          send ACK(EOF) + NAK -> AWAIT_DATA    (if gaps)
 *
 *   AWAIT_DATA:
 *      ingest(File Data PDU)
 *          update gap_list, reset check retry counter
 *          gap_list empty? -> SEND_FINISHED
 *      ingest(EOF PDU)             [our ACK(EOF) was lost]
 *          resend ACK(EOF) -> stay AWAIT_DATA
 *      ingest(Metadata PDU)        [late arrival after our request]
 *          record filename -> stay AWAIT_DATA
 *      check timer expires with pending gaps
 *          retry < limit: resend NAK -> stay AWAIT_DATA
 *          retry >= limit -> CANCELLED
 *
 *   SEND_FINISHED  [transient — executes inline, no recv needed]
 *      verify checksum
 *          OK:  send Finished(COMPLETE) -> AWAIT_FINISHED_ACK
 *          BAD: send Finished(CHECKSUM_FAILURE) -> CANCELLED
 *
 *   AWAIT_FINISHED_ACK
 *      ingest(ACK(Finished)) -> FINISHED
 *      ingest(EOF PDU)             [sender never saw our ACK(EOF)]
 *          resend ACK(EOF) + Finished -> stay
 *      timer expires, retry < limit
 *          resend Finished -> stay AWAIT_FINISHED_ACK
 *      timer expires, retry >= limit
 *          file is complete and verified, so -> FINISHED (with warning)
 *
 *   FINISHED / CANCELLED — terminal
 *
 * Gap list design:
 *   We track missing byte ranges as a sorted, growable array of
 *   {start, end} pairs (cfdp2_gap_list_t, cfdp_class2_gaps.c).  On
 *   receipt of a File Data PDU at [offset, offset+len) we punch that
 *   range out of the gap list.  An empty gap list means the file is
 *   complete.  The list is not bounded by CFDP_MAX_NAK_GAPS: that is a
 *   per-PDU limit, and send_nak() emits as many NAK PDUs as needed.
 *
 * Timer model:
 *   We use time(NULL) for wall-clock elapsed time.  No threads are needed;
 *   the receiver run loop calls cfdp2_receiver_ingest() on each received
 *   PDU and handles timer checks in cfdp2_receiver_run().
 */

#include "cfdp_class2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * Internal: build and send a complete PDU toward the sender
 * ---------------------------------------------------------------------- */

static cfdp_status_t send_pdu(cfdp2_receiver_t *r,
                              cfdp_pdu_type_t pdu_type,
                              const uint8_t *data_field,
                              uint16_t data_len)
{
    uint8_t buf[CFDP_MAX_PDU_SIZE];

    cfdp_pdu_header_t hdr = {
        .version             = 1,
        .pdu_type            = pdu_type,
        .direction           = CFDP_DIR_TOWARD_SENDER,
        .tx_mode             = CFDP_TX_MODE_ACKNOWLEDGED,
        .crc_flag            = false,
        .large_file          = false,
        .data_field_len      = data_len,
        .entity_id_len       = r->entity_id_len,
        .seq_num_len         = r->seq_num_len,
        /* Swap source/dest: the receiver's source is the original sender */
        .source_entity_id    = r->dest_entity_id,
        .dest_entity_id      = r->source_entity_id,
        .transaction_seq_num = r->seq_num,
    };

    int hdr_len = cfdp_encode_header(&hdr, buf, sizeof(buf));
    if (hdr_len < 0) return (cfdp_status_t)hdr_len;

    if ((size_t)hdr_len + data_len > sizeof(buf))
        return CFDP_ERR_BUF_TOO_SMALL;

    memcpy(buf + hdr_len, data_field, data_len);

    return r->transport->send(r->transport, buf, (size_t)hdr_len + data_len);
}

/* -----------------------------------------------------------------------
 * Internal: send ACK(EOF)
 * ---------------------------------------------------------------------- */

static cfdp_status_t send_ack_eof(cfdp2_receiver_t *r)
{
    uint8_t data_field[8];

    cfdp_ack_pdu_t ack = {
        .acked_directive   = CFDP_DIRECTIVE_EOF,
        .directive_subtype = 0,   /* subtype 0 for ACK(EOF) per Blue Book */
        .condition_code    = CFDP_COND_NO_ERROR,
        .transaction_status= CFDP_TX_STATUS_ACTIVE,
    };

    int encoded = cfdp2_encode_ack(&ack, data_field, sizeof(data_field));
    if (encoded < 0) return (cfdp_status_t)encoded;

    cfdp_status_t rc = send_pdu(r, CFDP_PDU_FILE_DIRECTIVE,
                                data_field, (uint16_t)encoded);
    if (rc != CFDP_OK) return rc;

    printf("[RECV2] Sent ACK(EOF)\n");
    return CFDP_OK;
}

/* -----------------------------------------------------------------------
 * Internal: send one NAK PDU carrying the given gaps
 * ---------------------------------------------------------------------- */

static cfdp_status_t send_one_nak(cfdp2_receiver_t *r,
                                  const cfdp_nak_gap_t *gaps,
                                  uint32_t gap_count,
                                  uint32_t scope_start,
                                  uint32_t scope_end)
{
    uint8_t data_field[CFDP_MAX_PDU_SIZE];

    cfdp_nak_pdu_t nak = {
        .scope_start = scope_start,
        .scope_end   = scope_end,
        .gap_count   = gap_count,
    };
    memcpy(nak.gaps, gaps, gap_count * sizeof(cfdp_nak_gap_t));

    int encoded = cfdp2_encode_nak(&nak, data_field, sizeof(data_field));
    if (encoded < 0) return (cfdp_status_t)encoded;

    cfdp_status_t rc = send_pdu(r, CFDP_PDU_FILE_DIRECTIVE,
                                data_field, (uint16_t)encoded);
    if (rc != CFDP_OK) return rc;

    printf("[RECV2] Sent NAK: %u gap(s), scope [%u, %u)\n",
           gap_count, scope_start, scope_end);
    return CFDP_OK;
}

/*
 * Send NAK PDU(s) covering every gap in the list, CFDP_MAX_NAK_GAPS per
 * PDU.  If Metadata is still missing, the first PDU also carries the
 * [0,0) "resend Metadata" request (CCSDS 727.0-B-5 4.6.4.3.3).
 */
static cfdp_status_t send_nak(cfdp2_receiver_t *r)
{
    cfdp_nak_gap_t  chunk[CFDP_MAX_NAK_GAPS];
    uint32_t        total = r->gap_list.count;
    uint32_t        i     = 0;
    bool            want_metadata = !r->got_metadata;

    printf("[RECV2] NAK round: %u gap(s) pending%s\n",
           total, want_metadata ? " + Metadata request" : "");

    do {
        uint32_t n = 0;
        if (want_metadata) {
            chunk[n].start = 0;
            chunk[n].end   = 0;
            n++;
            want_metadata = false;
        }
        while (i < total && n < CFDP_MAX_NAK_GAPS) {
            chunk[n++] = r->gap_list.gaps[i++];
        }

        /* Scope = span of the real gaps in this PDU (0 if only Metadata) */
        uint32_t scope_start = 0, scope_end = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (chunk[k].end == 0) continue;
            if (scope_end == 0 || chunk[k].start < scope_start)
                scope_start = chunk[k].start;
            if (chunk[k].end > scope_end) scope_end = chunk[k].end;
        }

        cfdp_status_t rc = send_one_nak(r, chunk, n, scope_start, scope_end);
        if (rc != CFDP_OK) return rc;
    } while (i < total);

    return CFDP_OK;
}

/* Rate-limited "please resend Metadata" NAK while we have no file size. */
static cfdp_status_t send_metadata_request(cfdp2_receiver_t *r)
{
    time_t now = time(NULL);
    if (r->last_metadata_nak_time != 0 &&
        now - r->last_metadata_nak_time < CFDP_METADATA_NAK_INTERVAL_S)
        return CFDP_OK;
    r->last_metadata_nak_time = now;

    cfdp_nak_gap_t req = { .start = 0, .end = 0 };
    return send_one_nak(r, &req, 1, 0, 0);
}

/* -----------------------------------------------------------------------
 * Internal: send Finished PDU and transition state
 * ---------------------------------------------------------------------- */

static cfdp_status_t send_finished_pdu(cfdp2_receiver_t *r,
                                       cfdp_condition_code_t cond,
                                       cfdp_delivery_code_t delivery)
{
    uint8_t data_field[8];

    cfdp_finished_pdu_t fin = {
        .condition_code = cond,
        .delivery_code  = delivery,
        .file_status    = CFDP_FILESTATUS_RETAINED,
    };

    int encoded = cfdp2_encode_finished(&fin, data_field, sizeof(data_field));
    if (encoded < 0) return (cfdp_status_t)encoded;

    cfdp_status_t rc = send_pdu(r, CFDP_PDU_FILE_DIRECTIVE,
                                data_field, (uint16_t)encoded);
    if (rc != CFDP_OK) return rc;

    printf("[RECV2] Sent Finished: delivery=%s cond=%d\n",
           delivery == CFDP_DELIVERY_COMPLETE ? "COMPLETE" : "INCOMPLETE",
           (int)cond);
    r->finished_sent_time = time(NULL);
    return CFDP_OK;
}

static cfdp_status_t do_send_finished(cfdp2_receiver_t *r,
                                      cfdp_condition_code_t cond,
                                      cfdp_delivery_code_t delivery)
{
    cfdp_status_t rc = send_finished_pdu(r, cond, delivery);
    if (rc != CFDP_OK) return rc;

    r->finished_retry_count = 0;
    r->state = CFDP2_RECV_AWAIT_FINISHED_ACK;
    return CFDP_OK;
}

/* -----------------------------------------------------------------------
 * Internal: verify checksum and send Finished, transitioning state
 * ---------------------------------------------------------------------- */

static cfdp_status_t finish_transfer(cfdp2_receiver_t *r)
{
    /* Verify checksum */
    uint32_t computed = CFDP_MODULAR_SUM_INIT;
    if (r->rx_buf.data && r->expected_file_size > 0) {
        computed = cfdp_checksum_modular(computed,
                                         r->rx_buf.data,
                                         r->expected_file_size);
    }

    if (computed != r->expected_checksum) {
        fprintf(stderr,
                "[RECV2] CHECKSUM MISMATCH: computed=0x%08X expected=0x%08X\n",
                computed, r->expected_checksum);

        /* Send Finished with checksum-failure condition */
        cfdp_status_t rc = do_send_finished(r,
                               CFDP_COND_FILE_CHECKSUM_FAILURE,
                               CFDP_DELIVERY_INCOMPLETE);
        if (rc != CFDP_OK) return rc;

        /* Immediately cancel — we cannot recover from a checksum error
           in this simplified implementation. */
        r->state = CFDP2_RECV_CANCELLED;
        return CFDP_ERR_CHECKSUM;
    }

    printf("[RECV2] Checksum OK (0x%08X). Sending Finished.\n", computed);
    return do_send_finished(r, CFDP_COND_NO_ERROR, CFDP_DELIVERY_COMPLETE);
}

/* -----------------------------------------------------------------------
 * Internal: allocate the reassembly buffer once the file size is known
 * (from Metadata or, failing that, from EOF).
 * ---------------------------------------------------------------------- */

static cfdp_status_t setup_reassembly(cfdp2_receiver_t *r, uint32_t file_size)
{
    r->expected_file_size = file_size;

    if (file_size > 0) {
        r->rx_buf.data = (uint8_t *)calloc(1, file_size);
        if (!r->rx_buf.data) {
            fprintf(stderr, "[RECV2] OOM allocating %u bytes\n", file_size);
            return CFDP_ERR_IO;
        }
        r->rx_buf.capacity = file_size;
    }

    return cfdp2_gap_list_reset(&r->gap_list, file_size);
}

/* -----------------------------------------------------------------------
 * PDU dispatch handlers
 * ---------------------------------------------------------------------- */

static cfdp_status_t handle_metadata(cfdp2_receiver_t *r,
                                     const uint8_t *data, uint16_t len)
{
    if (r->got_metadata) {
        fprintf(stderr, "[RECV2] Duplicate Metadata PDU — ignored\n");
        return CFDP_OK;
    }

    cfdp_metadata_pdu_t meta;
    int rc = cfdp_decode_metadata(data, len, &meta);
    if (rc < 0) return (cfdp_status_t)rc;

    r->checksum_type = meta.checksum_type;
    strncpy(r->dst_filename, meta.dst_filename, CFDP_MAX_FILENAME_LEN);

    printf("[RECV2] Got Metadata: src='%s' dst='%s' size=%u\n",
           meta.src_filename, r->dst_filename, meta.file_size);

    if (r->state == CFDP2_RECV_AWAIT_METADATA) {
        cfdp_status_t src = setup_reassembly(r, meta.file_size);
        if (src != CFDP_OK) return src;
        r->state = CFDP2_RECV_RECEIVING;
    } else if (meta.file_size != r->expected_file_size) {
        /* Buffer was already set up from EOF; sizes must agree. */
        fprintf(stderr,
                "[RECV2] Metadata size %u != EOF size %u — CANCELLED\n",
                meta.file_size, r->expected_file_size);
        r->state = CFDP2_RECV_CANCELLED;
        return CFDP_ERR_INVALID_PDU;
    }

    r->got_metadata = true;

    /* Metadata was the last thing we were waiting on */
    if (r->state == CFDP2_RECV_AWAIT_DATA && r->gap_list.count == 0) {
        return finish_transfer(r);
    }
    return CFDP_OK;
}

static cfdp_status_t handle_file_data(cfdp2_receiver_t *r,
                                      const uint8_t *data, uint16_t len)
{
    if (r->state == CFDP2_RECV_AWAIT_METADATA) {
        /* No file size yet, so nowhere to put this.  Ask for Metadata. */
        return send_metadata_request(r);
    }

    if (r->state != CFDP2_RECV_RECEIVING &&
        r->state != CFDP2_RECV_AWAIT_DATA) {
        fprintf(stderr,
                "[RECV2] File Data PDU in unexpected state %d — ignored\n",
                r->state);
        return CFDP_OK;
    }

    cfdp_file_data_pdu_t fd;
    int rc = cfdp_decode_file_data(data, len, &fd);
    if (rc < 0) return (cfdp_status_t)rc;

    /* Bounds check */
    if ((uint64_t)fd.offset + fd.data_len > r->rx_buf.capacity) {
        fprintf(stderr,
                "[RECV2] File Data beyond buffer "
                "(offset=%u len=%u cap=%u)\n",
                fd.offset, fd.data_len, r->rx_buf.capacity);
        return CFDP_ERR_INVALID_PDU;
    }

    /* Write data into reassembly buffer at the correct offset */
    memcpy(r->rx_buf.data + fd.offset, fd.data, fd.data_len);
    r->rx_buf.received_bytes += fd.data_len;

    /* Update gap list.  On OOM the gap simply stays recorded as missing,
       which costs a redundant retransmission but never loses data. */
    (void)cfdp2_gap_list_mark_received(&r->gap_list,
                                       fd.offset,
                                       fd.offset + fd.data_len);

    printf("[RECV2] Got File Data: offset=%-6u len=%-4u  "
           "gaps=%u\n",
           fd.offset, fd.data_len, r->gap_list.count);

    if (r->state == CFDP2_RECV_AWAIT_DATA) {
        /* Progress: the link is alive, so give the sender fresh retries */
        r->check_retry_count = 0;

        if (r->gap_list.count == 0 && r->got_metadata) {
            return finish_transfer(r);
        }
    }

    return CFDP_OK;
}

static cfdp_status_t handle_eof(cfdp2_receiver_t *r,
                                const uint8_t *data, uint16_t len)
{
    cfdp_eof_pdu_t eof;
    int rc = cfdp_decode_eof(data, len, &eof);
    if (rc < 0) return (cfdp_status_t)rc;

    /* Duplicate EOF: the sender didn't see our ACK(EOF).  Re-ACK, and
       if we've already finished, re-send Finished too. */
    if (r->got_eof) {
        printf("[RECV2] Duplicate EOF — re-sending ACK(EOF)\n");
        cfdp_status_t arc = send_ack_eof(r);
        if (arc != CFDP_OK) return arc;
        if (r->state == CFDP2_RECV_AWAIT_FINISHED_ACK) {
            return send_finished_pdu(r, CFDP_COND_NO_ERROR,
                                     CFDP_DELIVERY_COMPLETE);
        }
        return CFDP_OK;
    }

    if (r->state != CFDP2_RECV_AWAIT_METADATA &&
        r->state != CFDP2_RECV_RECEIVING) {
        fprintf(stderr,
                "[RECV2] EOF PDU in unexpected state %d — ignored\n",
                r->state);
        return CFDP_OK;
    }

    printf("[RECV2] Got EOF: cond=%d checksum=0x%08X size=%u\n",
           (int)eof.condition_code, eof.checksum, eof.file_size);

    /* Sender-side cancellation: eof.condition_code != NO_ERROR */
    if (eof.condition_code != CFDP_COND_NO_ERROR) {
        fprintf(stderr,
                "[RECV2] Sender cancelled transaction (cond=%d)\n",
                (int)eof.condition_code);
        r->state = CFDP2_RECV_CANCELLED;
        return CFDP_ERR_STATE;
    }

    /* Metadata never arrived: EOF tells us the size, so set up now. */
    if (r->state == CFDP2_RECV_AWAIT_METADATA) {
        printf("[RECV2] EOF before Metadata — setting up from EOF size\n");
        cfdp_status_t src = setup_reassembly(r, eof.file_size);
        if (src != CFDP_OK) return src;
    } else if (eof.file_size != r->expected_file_size) {
        fprintf(stderr,
                "[RECV2] EOF size %u != Metadata size %u — CANCELLED\n",
                eof.file_size, r->expected_file_size);
        r->state = CFDP2_RECV_CANCELLED;
        return CFDP_ERR_INVALID_PDU;
    }

    r->expected_checksum = eof.checksum;
    r->got_eof           = true;

    /* Always send ACK(EOF) first */
    cfdp_status_t arc = send_ack_eof(r);
    if (arc != CFDP_OK) return arc;

    if (r->gap_list.count == 0 && r->got_metadata) {
        /* No missing data — go straight to Finished */
        return finish_transfer(r);
    }

    /* Missing segments (and/or Metadata) — NAK and wait */
    arc = send_nak(r);
    if (arc != CFDP_OK) return arc;

    r->check_timer_start = time(NULL);
    r->check_retry_count = 0;
    r->state = CFDP2_RECV_AWAIT_DATA;
    return CFDP_OK;
}

static cfdp_status_t handle_ack_finished(cfdp2_receiver_t *r)
{
    if (r->state != CFDP2_RECV_AWAIT_FINISHED_ACK) {
        fprintf(stderr, "[RECV2] ACK(Finished) in unexpected state — ignored\n");
        return CFDP_OK;
    }
    printf("[RECV2] Got ACK(Finished). Transfer complete.\n");
    r->state = CFDP2_RECV_FINISHED;
    return CFDP_OK;
}

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

cfdp_status_t cfdp2_receiver_init(cfdp2_receiver_t *r,
                                  uint32_t dest_entity_id,
                                  uint8_t  entity_id_len,
                                  uint8_t  seq_num_len,
                                  cfdp_transport_t *transport)
{
    memset(r, 0, sizeof(*r));
    r->state          = CFDP2_RECV_AWAIT_METADATA;
    r->dest_entity_id = dest_entity_id;
    r->entity_id_len  = entity_id_len;
    r->seq_num_len    = seq_num_len;
    r->transport      = transport;
    return cfdp2_gap_list_init(&r->gap_list);
}

cfdp_status_t cfdp2_receiver_ingest(cfdp2_receiver_t *r,
                                    const uint8_t *pdu_buf,
                                    size_t pdu_len)
{
    if (pdu_len < 4u) return CFDP_ERR_INVALID_PDU;

    cfdp_pdu_header_t hdr;
    int hdr_len = cfdp_decode_header(pdu_buf, pdu_len, &hdr);
    if (hdr_len < 0) return (cfdp_status_t)hdr_len;

    /* Latch transaction identity on the first PDU of any kind */
    if (!r->tx_latched) {
        r->source_entity_id = hdr.source_entity_id;
        r->seq_num          = hdr.transaction_seq_num;
        r->tx_latched       = true;
    } else {
        if (hdr.source_entity_id    != r->source_entity_id ||
            hdr.transaction_seq_num != r->seq_num) {
            fprintf(stderr, "[RECV2] PDU from unknown transaction — ignored\n");
            return CFDP_OK;
        }
    }

    const uint8_t *data_field = pdu_buf + hdr_len;
    uint16_t       data_len   = hdr.data_field_len;

    if ((size_t)hdr_len + data_len > pdu_len) {
        fprintf(stderr, "[RECV2] data_field_len %u overruns PDU\n", data_len);
        return CFDP_ERR_INVALID_PDU;
    }

    if (hdr.pdu_type == CFDP_PDU_FILE_DATA) {
        return handle_file_data(r, data_field, data_len);
    }

    /* File directive PDU */
    if (data_len < 1u) return CFDP_ERR_INVALID_PDU;
    uint8_t directive_code = data_field[0];

    switch (directive_code) {
    case CFDP_DIRECTIVE_METADATA:
        return handle_metadata(r, data_field, data_len);

    case CFDP_DIRECTIVE_EOF:
        return handle_eof(r, data_field, data_len);

    case CFDP_DIRECTIVE_ACK: {
        cfdp_ack_pdu_t ack;
        if (cfdp2_decode_ack(data_field, data_len, &ack) < 0) return CFDP_OK;
        if (ack.acked_directive == CFDP_DIRECTIVE_FINISHED) {
            return handle_ack_finished(r);
        }
        fprintf(stderr, "[RECV2] Unexpected ACK for directive 0x%02X\n",
                (unsigned)ack.acked_directive);
        return CFDP_OK;
    }

    default:
        fprintf(stderr, "[RECV2] Unknown directive 0x%02X — ignored\n",
                directive_code);
        return CFDP_OK;
    }
}

cfdp_status_t cfdp2_receiver_run(cfdp2_receiver_t *r)
{
    uint8_t buf[CFDP_MAX_PDU_SIZE];

    while (r->state != CFDP2_RECV_FINISHED &&
           r->state != CFDP2_RECV_CANCELLED) {

        /* ---- Timer checks ------------------------------------------- */

        if (r->state == CFDP2_RECV_AWAIT_DATA) {
            time_t elapsed = time(NULL) - r->check_timer_start;
            if (elapsed >= CFDP_CHECK_TIMEOUT_S) {
                if (r->check_retry_count >= CFDP_CHECK_RETRY_LIMIT) {
                    fprintf(stderr,
                            "[RECV2] No data for %d NAK rounds — CANCELLED\n",
                            CFDP_CHECK_RETRY_LIMIT);
                    r->state = CFDP2_RECV_CANCELLED;
                    break;
                }
                fprintf(stderr,
                        "[RECV2] Check timer expired — resending NAK "
                        "(silent round %d/%d)\n",
                        r->check_retry_count + 1, CFDP_CHECK_RETRY_LIMIT);
                r->check_retry_count++;
                cfdp_status_t rc = send_nak(r);
                if (rc != CFDP_OK) return rc;
                r->check_timer_start = time(NULL);
            }
        }

        if (r->state == CFDP2_RECV_AWAIT_FINISHED_ACK) {
            time_t elapsed = time(NULL) - r->finished_sent_time;
            if (elapsed >= CFDP_ACK_TIMEOUT_S) {
                if (r->finished_retry_count >= CFDP_ACK_RETRY_LIMIT) {
                    /*
                     * The file is complete and checksum-verified; only the
                     * closing handshake is unconfirmed.  Keep the file
                     * rather than throwing away a good transfer.
                     */
                    fprintf(stderr,
                            "[RECV2] No ACK(Finished) after %d retries — "
                            "closing transaction anyway (file complete)\n",
                            CFDP_ACK_RETRY_LIMIT);
                    r->state = CFDP2_RECV_FINISHED;
                    break;
                }
                fprintf(stderr,
                        "[RECV2] Finished ACK timeout — resending Finished "
                        "(retry %d/%d)\n",
                        r->finished_retry_count + 1, CFDP_ACK_RETRY_LIMIT);
                r->finished_retry_count++;

                /* We only reach this state after a good checksum. */
                (void)send_finished_pdu(r, CFDP_COND_NO_ERROR,
                                        CFDP_DELIVERY_COMPLETE);
            }
        }

        /* ---- Receive next PDU ---------------------------------------- */

        int n = r->transport->recv(r->transport, buf, sizeof(buf));
        if (n < 0) {
            fprintf(stderr, "[RECV2] transport recv error: %d\n", n);
            return CFDP_ERR_TRANSPORT;
        }
        if (n == 0) continue;  /* timeout — loop and re-check timers */

        cfdp_status_t rc = cfdp2_receiver_ingest(r, buf, (size_t)n);
        /*
         * CFDP_ERR_CHECKSUM is returned by finish_transfer() when the
         * checksum fails.  We've already sent a Finished PDU with the
         * failure condition, so just propagate the error upward.
         */
        if (rc != CFDP_OK) return rc;
    }

    return (r->state == CFDP2_RECV_FINISHED) ? CFDP_OK : CFDP_ERR_CHECKSUM;
}

cfdp_status_t cfdp2_receiver_write_file(cfdp2_receiver_t *r)
{
    if (r->state != CFDP2_RECV_FINISHED) return CFDP_ERR_STATE;
    if (!r->rx_buf.data)                 return CFDP_ERR_STATE;

    int fd = open(r->dst_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("cfdp2_receiver_write_file: open");
        return CFDP_ERR_IO;
    }

    ssize_t written = write(fd, r->rx_buf.data, r->expected_file_size);
    close(fd);

    if (written != (ssize_t)r->expected_file_size) {
        perror("cfdp2_receiver_write_file: write");
        return CFDP_ERR_IO;
    }

    printf("[RECV2] File written to '%s' (%u bytes)\n",
           r->dst_filename, r->expected_file_size);
    return CFDP_OK;
}

void cfdp2_receiver_destroy(cfdp2_receiver_t *r)
{
    free(r->rx_buf.data);
    r->rx_buf.data     = NULL;
    r->rx_buf.capacity = 0;
    cfdp2_gap_list_free(&r->gap_list);
}

const char *cfdp2_recv_state_str(cfdp2_recv_state_t s)
{
    switch (s) {
    case CFDP2_RECV_IDLE:               return "IDLE";
    case CFDP2_RECV_AWAIT_METADATA:     return "AWAIT_METADATA";
    case CFDP2_RECV_RECEIVING:          return "RECEIVING";
    case CFDP2_RECV_SEND_EOF_ACK:       return "SEND_EOF_ACK";
    case CFDP2_RECV_AWAIT_DATA:         return "AWAIT_DATA";
    case CFDP2_RECV_SEND_FINISHED:      return "SEND_FINISHED";
    case CFDP2_RECV_AWAIT_FINISHED_ACK: return "AWAIT_FINISHED_ACK";
    case CFDP2_RECV_FINISHED:           return "FINISHED";
    case CFDP2_RECV_CANCELLED:          return "CANCELLED";
    default:                            return "UNKNOWN";
    }
}
