#ifndef CFDP_H
#define CFDP_H

/*
 * cfdp.h — CCSDS File Delivery Protocol (CFDP) core definitions
 *
 * Implements CCSDS 727.0-B-5 (Blue Book)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Protocol limits and defaults
 * ---------------------------------------------------------------------- */

#define CFDP_MAX_ENTITY_ID_LEN   4   /* bytes; we support 1, 2, or 4      */
#define CFDP_MAX_SEQ_NUM_LEN     4   /* bytes; we support 1, 2, or 4      */
#define CFDP_MAX_FILENAME_LEN  255
#define CFDP_MAX_PDU_SIZE     4096   /* bytes, transport MTU               */
#define CFDP_FILE_DATA_LIMIT  (CFDP_MAX_PDU_SIZE - 32)  /* conservative   */
#define CFDP_MODULAR_SUM_INIT    0u

/* -----------------------------------------------------------------------
 * PDU Direction / Type
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_DIR_TOWARD_RECEIVER = 0,
    CFDP_DIR_TOWARD_SENDER   = 1,
} cfdp_direction_t;

typedef enum {
    CFDP_PDU_FILE_DATA  = 0,
    CFDP_PDU_FILE_DIRECTIVE = 1,
} cfdp_pdu_type_t;

typedef enum {
    CFDP_TX_MODE_ACKNOWLEDGED   = 0,   /* Class 2 */
    CFDP_TX_MODE_UNACKNOWLEDGED = 1,   /* Class 1 */
} cfdp_tx_mode_t;

/* -----------------------------------------------------------------------
 * Directive codes (CCSDS 727.0-B-5 Table 5-4)
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_DIRECTIVE_EOF      = 0x04,
    CFDP_DIRECTIVE_FINISHED = 0x05,
    CFDP_DIRECTIVE_ACK      = 0x06,
    CFDP_DIRECTIVE_METADATA = 0x07,
    CFDP_DIRECTIVE_NAK      = 0x08,
    CFDP_DIRECTIVE_PROMPT   = 0x09,
    CFDP_DIRECTIVE_KEEPALIVE= 0x0C,
} cfdp_directive_code_t;

/* -----------------------------------------------------------------------
 * Condition codes (Table 5-3)
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_COND_NO_ERROR                  = 0x00,
    CFDP_COND_ACK_LIMIT_REACHED         = 0x01,
    CFDP_COND_KEEPALIVE_LIMIT           = 0x02,
    CFDP_COND_INVALID_TX_MODE           = 0x03,
    CFDP_COND_FILESTORE_REJECTION       = 0x04,
    CFDP_COND_FILE_CHECKSUM_FAILURE     = 0x05,
    CFDP_COND_FILE_SIZE_ERROR           = 0x06,
    CFDP_COND_NAK_LIMIT_REACHED         = 0x07,
    CFDP_COND_INACTIVITY_DETECTED       = 0x08,
    CFDP_COND_INVALID_FILE_STRUCTURE    = 0x09,
    CFDP_COND_CHECK_LIMIT_REACHED       = 0x0A,
    CFDP_COND_UNSUPPORTED_CHECKSUM_TYPE = 0x0B,
    CFDP_COND_CANCEL_RECV_BY_SENDER     = 0x0E,
    CFDP_COND_CANCEL_RECV_BY_RECEIVER   = 0x0F,
} cfdp_condition_code_t;

/* -----------------------------------------------------------------------
 * Checksum types
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_CHECKSUM_MODULAR = 0x00,   /* Modular (additive) 32-bit           */
    CFDP_CHECKSUM_NULL    = 0x0F,   /* No checksum                         */
} cfdp_checksum_type_t;

/* -----------------------------------------------------------------------
 * Common PDU fixed header (variable-length entity IDs and seq nums)
 *
 * Wire layout (CCSDS 727.0-B-5 5.1):
 *
 *   Octet 0:   [3:version][1:pdu_type][1:direction][1:tx_mode]
 *              [1:crc_flag][1:large_file]
 *   Octet 1-2: PDU data field length (big-endian)
 *   Octet 3:   [1:segmentation_ctrl][3:entity_id_len-1][1:segment_metadata]
 *              [3:seq_num_len-1]
 *   Octet 4..4+entity_id_len-1:                    source entity ID
 *   Octet ..+seq_num_len:                          transaction sequence num
 *   Octet ..+entity_id_len:                        destination entity ID
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t  version;           /* Must be 1                               */
    cfdp_pdu_type_t   pdu_type;
    cfdp_direction_t  direction;
    cfdp_tx_mode_t    tx_mode;
    bool     crc_flag;
    bool     large_file;        /* 0 = 32-bit file sizes, 1 = 64-bit       */
    uint16_t data_field_len;    /* bytes in PDU data field                 */
    uint8_t  entity_id_len;     /* 1, 2, or 4                              */
    uint8_t  seq_num_len;       /* 1, 2, or 4                              */
    uint32_t source_entity_id;
    uint32_t dest_entity_id;
    uint32_t transaction_seq_num;
} cfdp_pdu_header_t;

/* -----------------------------------------------------------------------
 * Metadata PDU (5.2.5)
 * ---------------------------------------------------------------------- */

typedef struct {
    bool     closure_requested;
    cfdp_checksum_type_t checksum_type;
    uint32_t file_size;         /* 0 if unknown / unbounded                */
    char     src_filename[CFDP_MAX_FILENAME_LEN + 1];
    char     dst_filename[CFDP_MAX_FILENAME_LEN + 1];
} cfdp_metadata_pdu_t;

/* -----------------------------------------------------------------------
 * File Data PDU (5.2.1)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t offset;            /* Byte offset into file                   */
    const uint8_t *data;        /* Pointer into caller-owned buffer        */
    uint16_t data_len;
} cfdp_file_data_pdu_t;

/* -----------------------------------------------------------------------
 * EOF PDU (5.2.4)
 * ---------------------------------------------------------------------- */

typedef struct {
    cfdp_condition_code_t condition_code;
    uint32_t checksum;
    uint32_t file_size;
} cfdp_eof_pdu_t;

/* -----------------------------------------------------------------------
 * Error codes returned by this library
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_OK              =  0,
    CFDP_ERR_BUF_TOO_SMALL = -1,
    CFDP_ERR_INVALID_PDU   = -2,
    CFDP_ERR_FILENAME_LEN  = -3,
    CFDP_ERR_IO            = -4,
    CFDP_ERR_CHECKSUM      = -5,
    CFDP_ERR_STATE         = -6,
    CFDP_ERR_TRANSPORT     = -7,
} cfdp_status_t;

/* -----------------------------------------------------------------------
 * Transport abstraction — swap UDP for anything (loopback, serial, etc.)
 * ---------------------------------------------------------------------- */

typedef struct cfdp_transport cfdp_transport_t;

struct cfdp_transport {
    /* Send len bytes from buf. Returns CFDP_OK or CFDP_ERR_TRANSPORT */
    cfdp_status_t (*send)(cfdp_transport_t *t,
                          const uint8_t *buf, size_t len);
    /* Receive up to max_len bytes into buf. Returns bytes received or <0*/
    int           (*recv)(cfdp_transport_t *t,
                          uint8_t *buf, size_t max_len);
    void *priv;   /* Implementation-specific state                        */
};

/* -----------------------------------------------------------------------
 * Sender state machine
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_SENDER_IDLE,
    CFDP_SENDER_SEND_METADATA,
    CFDP_SENDER_SEND_FILE_DATA,
    CFDP_SENDER_SEND_EOF,
    CFDP_SENDER_FINISHED,
    CFDP_SENDER_CANCELLED,
} cfdp_sender_state_t;

typedef struct {
    cfdp_sender_state_t state;

    /* Transaction parameters */
    uint32_t source_entity_id;
    uint32_t dest_entity_id;
    uint32_t seq_num;
    uint8_t  entity_id_len;     /* bytes */
    uint8_t  seq_num_len;       /* bytes */

    /* File transfer state */
    char     src_filename[CFDP_MAX_FILENAME_LEN + 1];
    char     dst_filename[CFDP_MAX_FILENAME_LEN + 1];
    int      file_fd;
    uint32_t file_size;
    uint32_t bytes_sent;
    uint32_t checksum;

    /* Tuning */
    uint16_t segment_size;      /* bytes per File Data PDU payload */

    cfdp_transport_t *transport;
} cfdp_sender_t;

/* -----------------------------------------------------------------------
 * Receiver state machine
 * ---------------------------------------------------------------------- */

typedef enum {
    CFDP_RECV_IDLE,
    CFDP_RECV_AWAIT_METADATA,
    CFDP_RECV_RECEIVING,
    CFDP_RECV_AWAIT_EOF,
    CFDP_RECV_CHECKSUM_VERIFY,
    CFDP_RECV_FINISHED,
    CFDP_RECV_CANCELLED,
} cfdp_recv_state_t;

/*
 * In-memory reassembly buffer for received file data.
 * For large files a real implementation would memory-map the output file,
 * but keeping it in a malloc'd buffer keeps the code self-contained.
 */
typedef struct {
    uint8_t *data;
    uint32_t capacity;
    uint32_t received_bytes;    /* total bytes written so far */
} cfdp_rx_buffer_t;

typedef struct {
    cfdp_recv_state_t state;

    /* Peer transaction identification */
    uint32_t source_entity_id;
    uint32_t dest_entity_id;
    uint32_t seq_num;
    uint8_t  entity_id_len;
    uint8_t  seq_num_len;

    /* Metadata received */
    bool     got_metadata;
    char     dst_filename[CFDP_MAX_FILENAME_LEN + 1];
    uint32_t expected_file_size;
    uint32_t expected_checksum;
    cfdp_checksum_type_t checksum_type;

    /* Reassembly */
    cfdp_rx_buffer_t rx_buf;

    cfdp_transport_t *transport;
} cfdp_receiver_t;

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/* --- PDU serialization ------------------------------------------------ */

/**
 * Serialize a PDU fixed header into buf.
 * Returns number of bytes written, or negative cfdp_status_t on error.
 */
int cfdp_encode_header(const cfdp_pdu_header_t *hdr,
                       uint8_t *buf, size_t buf_len);

/**
 * Deserialize a PDU fixed header from buf.
 * Returns number of bytes consumed, or negative cfdp_status_t on error.
 */
int cfdp_decode_header(const uint8_t *buf, size_t buf_len,
                       cfdp_pdu_header_t *hdr);

/**
 * Serialize a Metadata PDU data field (after the fixed header).
 * Returns bytes written or negative error.
 */
int cfdp_encode_metadata(const cfdp_metadata_pdu_t *meta,
                         uint8_t *buf, size_t buf_len);

int cfdp_decode_metadata(const uint8_t *buf, size_t buf_len,
                         cfdp_metadata_pdu_t *meta);

/**
 * Serialize / deserialize a File Data PDU data field.
 * Note: cfdp_decode_file_data sets pdu->data to point INTO buf; do not
 * free buf while pdu->data is in use.
 */
int cfdp_encode_file_data(const cfdp_file_data_pdu_t *pdu,
                          uint8_t *buf, size_t buf_len);

int cfdp_decode_file_data(const uint8_t *buf, size_t buf_len,
                          cfdp_file_data_pdu_t *pdu);

int cfdp_encode_eof(const cfdp_eof_pdu_t *eof,
                    uint8_t *buf, size_t buf_len);

int cfdp_decode_eof(const uint8_t *buf, size_t buf_len,
                    cfdp_eof_pdu_t *eof);

/* --- Checksum --------------------------------------------------------- */

/**
 * Accumulate CFDP modular (additive 32-bit) checksum over len bytes.
 * Start with sum = CFDP_MODULAR_SUM_INIT.
 * Safe to call multiple times over successive chunks.
 */
uint32_t cfdp_checksum_modular(uint32_t sum,
                               const uint8_t *data, size_t len);

/* --- Sender ----------------------------------------------------------- */

cfdp_status_t cfdp_sender_init(cfdp_sender_t *s,
                               uint32_t source_entity_id,
                               uint32_t dest_entity_id,
                               uint32_t seq_num,
                               uint8_t  entity_id_len,
                               uint8_t  seq_num_len,
                               uint16_t segment_size,
                               cfdp_transport_t *transport);

/**
 * Begin sending a file. Opens src_path, reads metadata, enters the
 * SEND_METADATA state.
 */
cfdp_status_t cfdp_sender_start(cfdp_sender_t *s,
                                const char *src_path,
                                const char *dst_filename);

/**
 * Drive the sender state machine one step.
 * Call repeatedly until it returns CFDP_OK and s->state == FINISHED.
 * Returns CFDP_ERR_STATE if called when idle or already finished.
 */
cfdp_status_t cfdp_sender_step(cfdp_sender_t *s);

/**
 * Convenience: run sender to completion (blocks).
 */
cfdp_status_t cfdp_sender_run(cfdp_sender_t *s);

void cfdp_sender_destroy(cfdp_sender_t *s);

/* --- Receiver --------------------------------------------------------- */

cfdp_status_t cfdp_receiver_init(cfdp_receiver_t *r,
                                 uint32_t dest_entity_id,
                                 uint8_t  entity_id_len,
                                 uint8_t  seq_num_len,
                                 cfdp_transport_t *transport);

/**
 * Feed one raw PDU (already received from transport) into the receiver.
 * The receiver updates its state machine and writes data to its buffer.
 */
cfdp_status_t cfdp_receiver_ingest(cfdp_receiver_t *r,
                                   const uint8_t *pdu_buf,
                                   size_t pdu_len);

/**
 * Convenience: block on transport->recv until the transfer is complete.
 * Calls cfdp_receiver_ingest for each received PDU.
 */
cfdp_status_t cfdp_receiver_run(cfdp_receiver_t *r);

/**
 * After CFDP_RECV_FINISHED, write the reassembled file to disk.
 */
cfdp_status_t cfdp_receiver_write_file(cfdp_receiver_t *r);

void cfdp_receiver_destroy(cfdp_receiver_t *r);

/* --- UDP transport ---------------------------------------------------- */

typedef struct {
    cfdp_transport_t base;   /* Must be first — cast-compatible */
    int      sock_fd;
    /* Peer address (send target) */
    uint16_t peer_port;
    char     peer_addr[64];
    /* Loss simulation: drop_pct in [0,100] */
    int      drop_pct;
} cfdp_udp_transport_t;

cfdp_status_t cfdp_udp_transport_init(cfdp_udp_transport_t *t,
                                      const char *bind_addr,
                                      uint16_t    bind_port,
                                      const char *peer_addr,
                                      uint16_t    peer_port,
                                      int         drop_pct);

void cfdp_udp_transport_destroy(cfdp_udp_transport_t *t);

/* --- Utility ---------------------------------------------------------- */

const char *cfdp_status_str(cfdp_status_t s);
const char *cfdp_sender_state_str(cfdp_sender_state_t s);
const char *cfdp_recv_state_str(cfdp_recv_state_t s);

#endif /* CFDP_H */