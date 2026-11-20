#ifndef CFDP_CLASS2_H
#define CFDP_CLASS2_H

/*
 * cfdp_class2.h — Class 2 (Acknowledged) CFDP extensions
 *
 * Builds on cfdp.h
 *
 * New PDU types (CCSDS 727.0-B-5 5.2):
 *   Finished PDU  (5.2.3) — receiver → sender, signals end of transaction
 *   ACK PDU       (5.2.4) — acknowledges EOF or Finished
 *   NAK PDU       (5.2.6) — receiver → sender, lists missing byte ranges
 *
 * Class 2 protocol flow (single-hop, immediate-NAK variant):
 *
 *   Sender                              Receiver
 *   ------                              --------
 *   Metadata PDU ---------------------->
 *   File Data PDUs (sequential) ------->
 *   EOF PDU --------------------------->
 *                       <--------------  ACK(EOF)
 *                       <--------------  NAK (missing segments, if any)
 *   [retransmit missing segments] ----->
 *                       <--------------  Finished PDU
 *   ACK(Finished) --------------------->
 *   [FINISHED]                          [FINISHED]
 *
 * Retransmission uses deferred-NAK: the receiver waits for EOF before
 * issuing a single consolidated NAK.
 */

#include "cfdp.h"
#include <time.h>

/* -----------------------------------------------------------------------
 * Class 2 limits
 * ---------------------------------------------------------------------- */

/*
 * Maximum number of missing-segment gap pairs in a single NAK PDU.
 * Each pair is two 32-bit offsets (8 bytes). 64 gaps × 8 = 512 bytes of NAK payload
 *
 * This is a *per-PDU* limit only. The number of gaps an entity can track
 * in memory (cfdp2_gap_list_t) is independent and grows with the file;
 * when there are more gaps than fit in one NAK the receiver sends several.
 */
#define CFDP_MAX_NAK_GAPS      64

/*
 * Initial capacity of a gap list.  Lists grow by doubling as needed.
 * 8 bytes per gap, so 256 gaps = 2 KiB.
 */
#define CFDP_GAP_LIST_INITIAL  256

/* Retransmission / ACK timer defaults (seconds) */
#define CFDP_ACK_TIMEOUT_S      5
#define CFDP_ACK_RETRY_LIMIT    3

/*
 * How long the receiver waits for straggling data after EOF (seconds)
 * before re-issuing its NAK.  The retry counter resets every time new
 * data arrives, so the limit bounds *consecutive silent* rounds rather
 * than total NAK rounds — a lossy but live link keeps going.
 */
#define CFDP_CHECK_TIMEOUT_S    5
#define CFDP_CHECK_RETRY_LIMIT  3

/*
 * Minimum spacing between "please resend Metadata" NAKs the receiver
 * emits while it is getting data for a transaction it has no Metadata
 * for (seconds).
 */
#define CFDP_METADATA_NAK_INTERVAL_S 1

/* -----------------------------------------------------------------------
 * Finished PDU (5.2.3)
 *
 * Wire layout (data field, after directive code byte 0x05):
 *   Byte 1: [7-4: condition_code][3: delivery_code][2-0: file_status]
 *
 * delivery_code: 0 = data complete, 1 = data incomplete
 * file_status:   0 = discarded on error, 1 = discarded by user request,
 *                2 = retained in filestore, 3 = file status unreported
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_DELIVERY_COMPLETE   = 0,
    CFDP_DELIVERY_INCOMPLETE = 1,
} cfdp_delivery_code_t;

typedef enum {
    CFDP_FILESTATUS_DISCARDED_ERROR   = 0,
    CFDP_FILESTATUS_DISCARDED_USER    = 1,
    CFDP_FILESTATUS_RETAINED          = 2,
    CFDP_FILESTATUS_UNREPORTED        = 3,
} cfdp_file_status_t;

typedef struct {
    cfdp_condition_code_t  condition_code;
    cfdp_delivery_code_t   delivery_code;
    cfdp_file_status_t     file_status;
} cfdp_finished_pdu_t;

/* -----------------------------------------------------------------------
 * ACK PDU (5.2.4)
 *
 * Wire layout (data field, after directive code byte 0x06):
 *   Byte 1: [7-4: directive_code_being_acked][3-2: directive_subtype]
 *           [1-0: spare]
 *   Byte 2: [7-4: condition_code][3: spare][2-0: transaction_status]
 *
 * directive_subtype: 0 for ACK(EOF), 1 for ACK(Finished) per Blue Book.
 * transaction_status: 0=undefined, 1=active, 2=terminated, 3=unrecognised.
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_TX_STATUS_UNDEFINED    = 0,
    CFDP_TX_STATUS_ACTIVE       = 1,
    CFDP_TX_STATUS_TERMINATED   = 2,
    CFDP_TX_STATUS_UNRECOGNISED = 3,
} cfdp_transaction_status_t;

typedef struct {
    cfdp_directive_code_t      acked_directive;   /* EOF or Finished */
    uint8_t                    directive_subtype;  /* 0 or 1 */
    cfdp_condition_code_t      condition_code;
    cfdp_transaction_status_t  transaction_status;
} cfdp_ack_pdu_t;

/* -----------------------------------------------------------------------
 * NAK PDU (5.2.6)
 *
 * Wire layout (data field, after directive code byte 0x08):
 *   Bytes 1-4:  scope_start (uint32, big-endian) — start of NAK scope
 *   Bytes 5-8:  scope_end   (uint32, big-endian) — end of NAK scope
 *   Then, for each missing segment:
 *     Bytes +0..+3: segment_offset_start (uint32, big-endian)
 *     Bytes +4..+7: segment_offset_end   (uint32, big-endian)
 *
 * scope covers [scope_start, scope_end); each gap pair covers
 * [segment_offset_start, segment_offset_end).
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t start;   /* inclusive byte offset */
    uint32_t end;     /* exclusive byte offset  */
} cfdp_nak_gap_t;

typedef struct {
    uint32_t        scope_start;
    uint32_t        scope_end;
    uint32_t        gap_count;
    cfdp_nak_gap_t  gaps[CFDP_MAX_NAK_GAPS];
} cfdp_nak_pdu_t;

/* -----------------------------------------------------------------------
 * Class 2 PDU encode / decode
 * ---------------------------------------------------------------------- */

/**
 * Serialize a Finished PDU data field (including directive code byte).
 * Returns bytes written or negative cfdp_status_t on error.
 */
int cfdp2_encode_finished(const cfdp_finished_pdu_t *fin,
                          uint8_t *buf, size_t buf_len);

int cfdp2_decode_finished(const uint8_t *buf, size_t buf_len,
                          cfdp_finished_pdu_t *fin);

/**
 * Serialize an ACK PDU data field (including directive code byte 0x06).
 */
int cfdp2_encode_ack(const cfdp_ack_pdu_t *ack,
                     uint8_t *buf, size_t buf_len);

int cfdp2_decode_ack(const uint8_t *buf, size_t buf_len,
                     cfdp_ack_pdu_t *ack);

/**
 * Serialize a NAK PDU data field (including directive code byte 0x08).
 * Returns bytes written or negative cfdp_status_t on error.
 * Will return CFDP_ERR_BUF_TOO_SMALL if gap_count exceeds CFDP_MAX_NAK_GAPS.
 */
int cfdp2_encode_nak(const cfdp_nak_pdu_t *nak,
                     uint8_t *buf, size_t buf_len);

int cfdp2_decode_nak(const uint8_t *buf, size_t buf_len,
                     cfdp_nak_pdu_t *nak);

/* -----------------------------------------------------------------------
 * Gap list — growable, sorted array of missing byte ranges
 *
 * Used by the receiver to track what has NOT arrived yet, and by the
 * sender to accumulate the gaps reported across one or more NAK PDUs.
 * Entries are kept sorted and non-overlapping.
 *
 * Storage is heap-allocated and grows by doubling.  If an allocation
 * fails the list is left unchanged and the operation reports failure;
 * callers fall back to conservative behaviour (over-NAK / over-send)
 * so correctness never depends on a successful grow.
 * ---------------------------------------------------------------------- */

typedef struct {
    cfdp_nak_gap_t *gaps;
    uint32_t        count;
    uint32_t        capacity;
} cfdp2_gap_list_t;

/** Allocate initial storage. Returns CFDP_ERR_IO on OOM. */
cfdp_status_t cfdp2_gap_list_init(cfdp2_gap_list_t *gl);

void cfdp2_gap_list_free(cfdp2_gap_list_t *gl);

/** Reset to a single gap [0, file_size), or empty if file_size == 0. */
cfdp_status_t cfdp2_gap_list_reset(cfdp2_gap_list_t *gl, uint32_t file_size);

/** Remove [recv_start, recv_end) from the list (data arrived). */
cfdp_status_t cfdp2_gap_list_mark_received(cfdp2_gap_list_t *gl,
                                           uint32_t recv_start,
                                           uint32_t recv_end);

/** Add [start, end) to the list, merging with overlapping/adjacent gaps. */
cfdp_status_t cfdp2_gap_list_add(cfdp2_gap_list_t *gl,
                                 uint32_t start, uint32_t end);

/* -----------------------------------------------------------------------
 * Class 2 Sender
 *
 * The Class 2 sender extends the Class 1 sender_t state with fields
 * needed for acknowledgment handling and selective retransmission.
 * We embed a cfdp_sender_t as the first member so a pointer to
 * cfdp2_sender_t can be cast to cfdp_sender_t when calling helpers
 * that only inspect the base fields.
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP2_SENDER_IDLE,
    CFDP2_SENDER_SEND_METADATA,
    CFDP2_SENDER_SEND_FILE_DATA,
    CFDP2_SENDER_SEND_EOF,
    CFDP2_SENDER_AWAIT_EOF_ACK,       /* waiting for receiver's ACK(EOF) */
    CFDP2_SENDER_RETRANSMIT,          /* replaying missing segments       */
    CFDP2_SENDER_AWAIT_FINISHED,      /* waiting for Finished PDU         */
    CFDP2_SENDER_FINISHED,
    CFDP2_SENDER_CANCELLED,
} cfdp2_sender_state_t;

typedef struct {
    /* ---- base Class 1 fields (keep first for cast-compatibility) ---- */
    cfdp2_sender_state_t state;

    uint32_t source_entity_id;
    uint32_t dest_entity_id;
    uint32_t seq_num;
    uint8_t  entity_id_len;
    uint8_t  seq_num_len;

    char     src_filename[CFDP_MAX_FILENAME_LEN + 1];
    char     dst_filename[CFDP_MAX_FILENAME_LEN + 1];
    int      file_fd;
    uint32_t file_size;
    uint32_t bytes_sent;
    uint32_t checksum;
    uint16_t segment_size;

    cfdp_transport_t *transport;

    /* ---- Class 2 additions ---- */

    /*
     * Pending NAK gaps received from the receiver.
     * Populated (merged) from every NAK PDU; consumed during RETRANSMIT.
     */
    cfdp2_gap_list_t nak_gaps;
    uint32_t        retransmit_gap_idx;   /* which gap we're currently replaying */
    uint32_t        retransmit_offset;    /* current seek position within the gap */

    /* Receiver asked for Metadata again (NAK gap [0,0) per 4.6.4.3). */
    bool            metadata_requested;

    /* ACK / retry timer state */
    time_t   eof_sent_time;     /* wall time when last EOF was sent */
    int      eof_retry_count;

    time_t   finished_wait_start; /* wall time when we entered AWAIT_FINISHED */
} cfdp2_sender_t;

cfdp_status_t cfdp2_sender_init(cfdp2_sender_t *s,
                                uint32_t source_entity_id,
                                uint32_t dest_entity_id,
                                uint32_t seq_num,
                                uint8_t  entity_id_len,
                                uint8_t  seq_num_len,
                                uint16_t segment_size,
                                cfdp_transport_t *transport);

/**
 * Begin sending a file. Opens src_path and transitions to SEND_METADATA.
 */
cfdp_status_t cfdp2_sender_start(cfdp2_sender_t *s,
                                 const char *src_path,
                                 const char *dst_filename);

/**
 * Drive the sender state machine one step.
 * In AWAIT_EOF_ACK and AWAIT_FINISHED states this also polls
 * transport->recv() (non-blocking) for incoming control PDUs.
 * Call repeatedly until s->state == CFDP2_SENDER_FINISHED.
 */
cfdp_status_t cfdp2_sender_step(cfdp2_sender_t *s);

/** Convenience: loop cfdp2_sender_step until finished or error. */
cfdp_status_t cfdp2_sender_run(cfdp2_sender_t *s);

void cfdp2_sender_destroy(cfdp2_sender_t *s);

const char *cfdp2_sender_state_str(cfdp2_sender_state_t s);

/* -----------------------------------------------------------------------
 * Class 2 Receiver
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP2_RECV_IDLE,
    CFDP2_RECV_AWAIT_METADATA,
    CFDP2_RECV_RECEIVING,
    CFDP2_RECV_SEND_EOF_ACK,     /* transient: send ACK(EOF), then NAK   */
    CFDP2_RECV_AWAIT_DATA,       /* waiting for retransmitted segments    */
    CFDP2_RECV_SEND_FINISHED,    /* transient: send Finished PDU          */
    CFDP2_RECV_AWAIT_FINISHED_ACK,
    CFDP2_RECV_FINISHED,
    CFDP2_RECV_CANCELLED,
} cfdp2_recv_state_t;

typedef struct {
    cfdp2_recv_state_t state;

    uint32_t source_entity_id;
    uint32_t dest_entity_id;
    uint32_t seq_num;
    uint8_t  entity_id_len;
    uint8_t  seq_num_len;

    bool     tx_latched;      /* source id / seq num learned from first PDU */
    bool     got_metadata;    /* Metadata PDU actually received            */
    bool     got_eof;         /* EOF received (file size + checksum known) */
    time_t   last_metadata_nak_time;
    char     dst_filename[CFDP_MAX_FILENAME_LEN + 1];
    uint32_t expected_file_size;
    uint32_t expected_checksum;
    cfdp_checksum_type_t checksum_type;

    /* Reassembly buffer (same strategy as Class 1 receiver) */
    cfdp_rx_buffer_t rx_buf;

    /* Missing-data tracking */
    cfdp2_gap_list_t gap_list;

    /*
     * Check timer: started when EOF is received. If the gap list is not
     * empty after the timer fires, we send a repeat NAK and restart.
     * After CFDP_CHECK_RETRY_LIMIT expirations we cancel.
     */
    time_t   check_timer_start;
    int      check_retry_count;

    /*
     * Finished ACK timer: after sending Finished we wait for ACK(Finished).
     * If it doesn't arrive we resend Finished.
     */
    time_t   finished_sent_time;
    int      finished_retry_count;

    cfdp_transport_t *transport;
} cfdp2_receiver_t;

cfdp_status_t cfdp2_receiver_init(cfdp2_receiver_t *r,
                                  uint32_t dest_entity_id,
                                  uint8_t  entity_id_len,
                                  uint8_t  seq_num_len,
                                  cfdp_transport_t *transport);

/**
 * Feed one raw PDU into the receiver state machine.
 * Unlike the Class 1 version this may also call transport->send()
 * internally to emit ACK / NAK / Finished PDUs in response.
 */
cfdp_status_t cfdp2_receiver_ingest(cfdp2_receiver_t *r,
                                    const uint8_t *pdu_buf,
                                    size_t pdu_len);

/**
 * Drive the receiver: poll transport->recv(), ingest PDUs, handle timers.
 * Blocks until transfer is complete or an unrecoverable error occurs.
 */
cfdp_status_t cfdp2_receiver_run(cfdp2_receiver_t *r);

/** After CFDP2_RECV_FINISHED, flush the reassembly buffer to disk. */
cfdp_status_t cfdp2_receiver_write_file(cfdp2_receiver_t *r);

void cfdp2_receiver_destroy(cfdp2_receiver_t *r);

const char *cfdp2_recv_state_str(cfdp2_recv_state_t s);

#endif /* CFDP_CLASS2_H */