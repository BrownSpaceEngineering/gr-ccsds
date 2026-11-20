/*
 * test_sender.c — Class 1 sender test process
 *
 * Usage:
 *   ./test_sender <file_to_send> [drop_pct]
 *
 * Sends the specified file to the receiver listening on 127.0.0.1:5001.
 * The sender binds to port 5000 and can simulate packet loss with
 * drop_pct (0-100, default 0).
 *
 * Run alongside test_receiver in a second terminal.
 */

#include "cfdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENDER_PORT   5000
#define RECEIVER_PORT 5001
#define PEER_ADDR     "127.0.0.1"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file_to_send> [drop_pct]\n", argv[0]);
        return 1;
    }

    const char *src_file = argv[1];
    int drop_pct = (argc >= 3) ? atoi(argv[2]) : 0;

    printf("=== CFDP Class 1 Sender ===\n");
    printf("File:     %s\n", src_file);
    printf("Sending to %s:%d  (bind port %d)\n",
           PEER_ADDR, RECEIVER_PORT, SENDER_PORT);
    printf("Loss sim: %d%%\n\n", drop_pct);

    /* --- Set up UDP transport --- */
    cfdp_udp_transport_t udp_transport;
    cfdp_status_t rc = cfdp_udp_transport_init(
        &udp_transport,
        NULL,           /* bind addr: INADDR_ANY */
        SENDER_PORT,
        PEER_ADDR,
        RECEIVER_PORT,
        drop_pct
    );
    if (rc != CFDP_OK) {
        fprintf(stderr, "Transport init failed: %s\n", cfdp_status_str(rc));
        return 1;
    }

    /* --- Initialize sender --- */
    cfdp_sender_t sender;
    rc = cfdp_sender_init(
        &sender,
        /*source_entity_id=*/ 1,
        /*dest_entity_id=*/   2,
        /*seq_num=*/           1,
        /*entity_id_len=*/     1,
        /*seq_num_len=*/       1,
        /*segment_size=*/    512,
        (cfdp_transport_t *)&udp_transport
    );
    if (rc != CFDP_OK) {
        fprintf(stderr, "Sender init failed: %s\n", cfdp_status_str(rc));
        cfdp_udp_transport_destroy(&udp_transport);
        return 1;
    }

    /* --- Start the transfer --- */
    rc = cfdp_sender_start(&sender, src_file, "received_file.bin");
    if (rc != CFDP_OK) {
        fprintf(stderr, "Sender start failed: %s\n", cfdp_status_str(rc));
        goto cleanup;
    }

    /* --- Run to completion --- */
    rc = cfdp_sender_run(&sender);
    if (rc != CFDP_OK) {
        fprintf(stderr, "Sender run failed: %s\n", cfdp_status_str(rc));
        goto cleanup;
    }

    printf("\n[SENDER] Transfer complete. State: %s\n",
           cfdp_sender_state_str(sender.state));

cleanup:
    cfdp_sender_destroy(&sender);
    cfdp_udp_transport_destroy(&udp_transport);
    return (rc == CFDP_OK) ? 0 : 1;
}