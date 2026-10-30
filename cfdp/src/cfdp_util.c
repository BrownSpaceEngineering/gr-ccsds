/*
 * cfdp_util.c — Diagnostic string helpers
 */

#include "cfdp.h"

const char *cfdp_status_str(cfdp_status_t s)
{
    switch (s) {
    case CFDP_OK:               return "OK";
    case CFDP_ERR_BUF_TOO_SMALL: return "ERR_BUF_TOO_SMALL";
    case CFDP_ERR_INVALID_PDU:  return "ERR_INVALID_PDU";
    case CFDP_ERR_FILENAME_LEN: return "ERR_FILENAME_LEN";
    case CFDP_ERR_IO:           return "ERR_IO";
    case CFDP_ERR_CHECKSUM:     return "ERR_CHECKSUM";
    case CFDP_ERR_STATE:        return "ERR_STATE";
    case CFDP_ERR_TRANSPORT:    return "ERR_TRANSPORT";
    default:                    return "ERR_UNKNOWN";
    }
}

const char *cfdp_sender_state_str(cfdp_sender_state_t s)
{
    switch (s) {
    case CFDP_SENDER_IDLE:          return "IDLE";
    case CFDP_SENDER_SEND_METADATA: return "SEND_METADATA";
    case CFDP_SENDER_SEND_FILE_DATA:return "SEND_FILE_DATA";
    case CFDP_SENDER_SEND_EOF:      return "SEND_EOF";
    case CFDP_SENDER_FINISHED:      return "FINISHED";
    case CFDP_SENDER_CANCELLED:     return "CANCELLED";
    default:                        return "UNKNOWN";
    }
}

const char *cfdp_recv_state_str(cfdp_recv_state_t s)
{
    switch (s) {
    case CFDP_RECV_IDLE:            return "IDLE";
    case CFDP_RECV_AWAIT_METADATA:  return "AWAIT_METADATA";
    case CFDP_RECV_RECEIVING:       return "RECEIVING";
    case CFDP_RECV_AWAIT_EOF:       return "AWAIT_EOF";
    case CFDP_RECV_CHECKSUM_VERIFY: return "CHECKSUM_VERIFY";
    case CFDP_RECV_FINISHED:        return "FINISHED";
    case CFDP_RECV_CANCELLED:       return "CANCELLED";
    default:                        return "UNKNOWN";
    }
}