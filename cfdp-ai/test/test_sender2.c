/*
 * test_sender2.c — CFDP Class 2 sender test
 */

#include "cfdp_class2.h"

#include <stdio.h>
#include <stdlib.h>

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

    printf("=== CFDP Class 2 Sender ===\n");
    printf("File: %s\n", src_file);
    printf("Loss simulation: %d%%\n\n", drop_pct);

    /* --- UDP transport --- */
    cfdp_udp_transport_t udp_transport;

    cfdp_status_t rc = cfdp_udp_transport_init(
        &udp_transport,
        NULL,
        SENDER_PORT,
        PEER_ADDR,
        RECEIVER_PORT,
        drop_pct
    );

    if (rc != CFDP_OK) {
        fprintf(stderr, "Transport init failed: %s\n",
                cfdp_status_str(rc));
        return 1;
    }

    /* --- Sender --- */
    cfdp2_sender_t sender;

    rc = cfdp2_sender_init(
        &sender,
        1,      /* source entity */
        2,      /* dest entity */
        1,      /* transaction seq num */
        1,      /* entity ID length */
        1,      /* seq num length */
        512,    /* segment size */
        (cfdp_transport_t *)&udp_transport
    );

    if (rc != CFDP_OK) {
        fprintf(stderr, "Sender init failed: %s\n",
                cfdp_status_str(rc));
        goto cleanup_transport;
    }

    rc = cfdp2_sender_start(
        &sender,
        src_file,
        "received_file.bin"
    );

    if (rc != CFDP_OK) {
        fprintf(stderr, "Sender start failed: %s\n",
                cfdp_status_str(rc));
        goto cleanup_sender;
    }

    rc = cfdp2_sender_run(&sender);

    if (rc != CFDP_OK) {
        fprintf(stderr, "Sender run failed: %s\n",
                cfdp_status_str(rc));
        goto cleanup_sender;
    }

    printf("\n[SENDER2] Transfer complete. State: %s\n",
           cfdp2_sender_state_str(sender.state));

cleanup_sender:
    cfdp2_sender_destroy(&sender);

cleanup_transport:
    cfdp_udp_transport_destroy(&udp_transport);

    return (rc == CFDP_OK) ? 0 : 1;
}