/*
 * cfdp_receiver.c — Class 1 (Unacknowledged) receiver state machine
 *
 * State transitions:
 *
 *   IDLE: cfdp_receiver_init() -> AWAIT_METADATA
 *
 *   AWAIT_METADATA: ingest(Metadata PDU) -> RECEIVING
 *
 *   RECEIVING: ingest(File Data PDU) -> stays RECEIVING, ingest(EOF PDU) -> CHECKSUM_VERIFY
 *
 *   CHECKSUM_VERIFY: checksum OK  -> FINISHED, checksum bad -> CANCELLED
 *
 *   FINISHED / CANCELLED — terminal
 *
 * The receiver accumulates all file data in a malloc'd buffer keyed by
 * byte offset, tolerating out-of-order File Data PDUs (within the buffer
 * size). After FINISHED, call cfdp_receiver_write_file() to flush to disk.
 */

#include "cfdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* -----------------------------------------------------------------------
 * Forward declarations for PDU dispatch helpers
 * ---------------------------------------------------------------------- */

static cfdp_status_t handle_metadata(cfdp_receiver_t *r,
                                     const uint8_t *data, uint16_t len);
static cfdp_status_t handle_file_data(cfdp_receiver_t *r,
                                      const uint8_t *data, uint16_t len);
static cfdp_status_t handle_eof(cfdp_receiver_t *r,
                                const uint8_t *data, uint16_t len);

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

cfdp_status_t cfdp_receiver_init(cfdp_receiver_t *r,
                                 uint32_t dest_entity_id,
                                 uint8_t  entity_id_len,
                                 uint8_t  seq_num_len,
                                 cfdp_transport_t *transport)
{
    memset(r, 0, sizeof(*r));
    r->state          = CFDP_RECV_AWAIT_METADATA;
    r->dest_entity_id = dest_entity_id;
    r->entity_id_len  = entity_id_len;
    r->seq_num_len    = seq_num_len;
    r->transport      = transport;
    return CFDP_OK;
}

cfdp_status_t cfdp_receiver_ingest(cfdp_receiver_t *r,
                                   const uint8_t *pdu_buf,
                                   size_t pdu_len)
{
    if (pdu_len < 4u) return CFDP_ERR_INVALID_PDU;

    /* Decode the fixed header */
    cfdp_pdu_header_t hdr;
    int hdr_len = cfdp_decode_header(pdu_buf, pdu_len, &hdr);
    if (hdr_len < 0) return (cfdp_status_t)hdr_len;

    /* On the very first PDU, latch the source entity and seq num */
    if (!r->got_metadata) {
        r->source_entity_id = hdr.source_entity_id;
        r->seq_num          = hdr.transaction_seq_num;
    } else {
        /* Reject PDUs that don't belong to our active transaction */
        if (hdr.source_entity_id != r->source_entity_id ||
            hdr.transaction_seq_num != r->seq_num) {
            fprintf(stderr, "[RECV] PDU from unknown transaction — ignored\n");
            return CFDP_OK;
        }
    }

    const uint8_t *data_field = pdu_buf + hdr_len;
    uint16_t       data_len   = hdr.data_field_len;

    /* Sanity: data field must fit in the buffer we received */
    if ((size_t)hdr_len + data_len > pdu_len) {
        fprintf(stderr, "[RECV] data_field_len %u overruns PDU\n", data_len);
        return CFDP_ERR_INVALID_PDU;
    }

    /* Dispatch by PDU type */
    if (hdr.pdu_type == CFDP_PDU_FILE_DIRECTIVE) {
        if (data_len < 1u) return CFDP_ERR_INVALID_PDU;
        uint8_t directive_code = data_field[0];
        switch (directive_code) {
        case CFDP_DIRECTIVE_METADATA:
            return handle_metadata(r, data_field, data_len);
        case CFDP_DIRECTIVE_EOF:
            return handle_eof(r, data_field, data_len);
        default:
            fprintf(stderr, "[RECV] Unexpected directive 0x%02X in Class 1\n",
                    directive_code);
            return CFDP_OK;
        }
    } else {
        /* CFDP_PDU_FILE_DATA */
        return handle_file_data(r, data_field, data_len);
    }
}

cfdp_status_t cfdp_receiver_run(cfdp_receiver_t *r)
{
    uint8_t buf[CFDP_MAX_PDU_SIZE];
    while (r->state != CFDP_RECV_FINISHED &&
           r->state != CFDP_RECV_CANCELLED) {
        int n = r->transport->recv(r->transport, buf, sizeof(buf));
        if (n < 0) {
            fprintf(stderr, "[RECV] transport recv error: %d\n", n);
            return CFDP_ERR_TRANSPORT;
        }
        if (n == 0) continue;  /* timeout — keep waiting */

        cfdp_status_t rc = cfdp_receiver_ingest(r, buf, (size_t)n);
        if (rc != CFDP_OK) return rc;
    }
    return (r->state == CFDP_RECV_FINISHED) ? CFDP_OK : CFDP_ERR_CHECKSUM;
}

cfdp_status_t cfdp_receiver_write_file(cfdp_receiver_t *r)
{
    if (r->state != CFDP_RECV_FINISHED) return CFDP_ERR_STATE;
    if (!r->rx_buf.data)                return CFDP_ERR_STATE;

    int fd = open(r->dst_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("cfdp_receiver_write_file: open");
        return CFDP_ERR_IO;
    }

    ssize_t written = write(fd, r->rx_buf.data, r->expected_file_size);
    close(fd);

    if (written != (ssize_t)r->expected_file_size) {
        perror("cfdp_receiver_write_file: write");
        return CFDP_ERR_IO;
    }

    printf("[RECV] File written to '%s' (%u bytes)\n",
           r->dst_filename, r->expected_file_size);
    return CFDP_OK;
}

void cfdp_receiver_destroy(cfdp_receiver_t *r)
{
    free(r->rx_buf.data);
    r->rx_buf.data     = NULL;
    r->rx_buf.capacity = 0;
}

/* -----------------------------------------------------------------------
 * PDU dispatch handlers
 * ---------------------------------------------------------------------- */

static cfdp_status_t handle_metadata(cfdp_receiver_t *r,
                                     const uint8_t *data, uint16_t len)
{
    if (r->state != CFDP_RECV_AWAIT_METADATA) {
        fprintf(stderr, "[RECV] Duplicate Metadata PDU — ignored\n");
        return CFDP_OK;
    }

    cfdp_metadata_pdu_t meta;
    int rc = cfdp_decode_metadata(data, len, &meta);
    if (rc < 0) return (cfdp_status_t)rc;

    r->expected_file_size = meta.file_size;
    r->checksum_type      = meta.checksum_type;
    strncpy(r->dst_filename, meta.dst_filename, CFDP_MAX_FILENAME_LEN);

    printf("[RECV] Got Metadata: src='%s' dst='%s' size=%u\n",
           meta.src_filename, r->dst_filename, r->expected_file_size);

    /* Allocate reassembly buffer */
    if (meta.file_size > 0) {
        r->rx_buf.data = (uint8_t *)calloc(1, meta.file_size);
        if (!r->rx_buf.data) {
            fprintf(stderr, "[RECV] OOM allocating %u bytes\n",
                    meta.file_size);
            return CFDP_ERR_IO;
        }
        r->rx_buf.capacity = meta.file_size;
    }

    r->got_metadata = true;
    r->state        = CFDP_RECV_RECEIVING;
    return CFDP_OK;
}

static cfdp_status_t handle_file_data(cfdp_receiver_t *r,
                                      const uint8_t *data, uint16_t len)
{
    if (r->state != CFDP_RECV_RECEIVING) {
        fprintf(stderr, "[RECV] File Data PDU in unexpected state %d\n",
                r->state);
        return CFDP_OK;
    }

    cfdp_file_data_pdu_t fd;
    int rc = cfdp_decode_file_data(data, len, &fd);
    if (rc < 0) return (cfdp_status_t)rc;

    /* Bounds check */
    if ((uint32_t)(fd.offset + fd.data_len) > r->rx_buf.capacity) {
        fprintf(stderr, "[RECV] File Data beyond allocated buffer "
                "(offset=%u, len=%u, cap=%u)\n",
                fd.offset, fd.data_len, r->rx_buf.capacity);
        return CFDP_ERR_INVALID_PDU;
    }

    /* Copy data into reassembly buffer at the correct offset */
    memcpy(r->rx_buf.data + fd.offset, fd.data, fd.data_len);
    r->rx_buf.received_bytes += fd.data_len;

    printf("[RECV] Got File Data: offset=%-6u len=%-4u  (%u bytes total)\n",
           fd.offset, fd.data_len, r->rx_buf.received_bytes);

    return CFDP_OK;
}

static cfdp_status_t handle_eof(cfdp_receiver_t *r,
                                const uint8_t *data, uint16_t len)
{
    if (r->state != CFDP_RECV_RECEIVING) {
        fprintf(stderr, "[RECV] EOF PDU in unexpected state %d\n", r->state);
        return CFDP_OK;
    }

    cfdp_eof_pdu_t eof;
    int rc = cfdp_decode_eof(data, len, &eof);
    if (rc < 0) return (cfdp_status_t)rc;

    printf("[RECV] Got EOF: condition=%d, checksum=0x%08X, size=%u\n",
           eof.condition_code, eof.checksum, eof.file_size);

    r->expected_checksum = eof.checksum;
    r->state = CFDP_RECV_CHECKSUM_VERIFY;

    /* ---- Verify checksum ---- */
    uint32_t computed = CFDP_MODULAR_SUM_INIT;
    if (r->rx_buf.data && r->expected_file_size > 0) {
        computed = cfdp_checksum_modular(computed,
                                         r->rx_buf.data,
                                         r->expected_file_size);
    }

    if (computed != r->expected_checksum) {
        fprintf(stderr,
                "[RECV] CHECKSUM MISMATCH: computed=0x%08X expected=0x%08X\n",
                computed, r->expected_checksum);
        r->state = CFDP_RECV_CANCELLED;
        return CFDP_ERR_CHECKSUM;
    }

    printf("[RECV] Checksum OK (0x%08X). Transfer complete.\n", computed);
    r->state = CFDP_RECV_FINISHED;
    return CFDP_OK;
}