/*
 * test_receiver2.c — CFDP Class 2 receiver test
 */

#include "cfdp_class2.h"

#include <stdio.h>
#include <stdlib.h>

#define RECEIVER_PORT 5001
#define SENDER_PORT   5000

int main(void)
{
    printf("=== CFDP Class 2 Receiver ===\n");
    printf("Listening on port %d ...\n\n", RECEIVER_PORT);

    /* --- UDP transport --- */
    cfdp_udp_transport_t udp_transport;

    cfdp_status_t rc = cfdp_udp_transport_init(
        &udp_transport,
        NULL,
        RECEIVER_PORT,
        "127.0.0.1",
        SENDER_PORT,
        0
    );

    if (rc != CFDP_OK) {
        fprintf(stderr, "Transport init failed: %s\n",
                cfdp_status_str(rc));
        return 1;
    }

    /* --- Receiver --- */
    cfdp2_receiver_t receiver;

    rc = cfdp2_receiver_init(
        &receiver,
        2,      /* destination entity */
        1,      /* entity ID length */
        1,      /* seq num length */
        (cfdp_transport_t *)&udp_transport
    );

    if (rc != CFDP_OK) {
        fprintf(stderr, "Receiver init failed: %s\n",
                cfdp_status_str(rc));
        goto cleanup_transport;
    }

    rc = cfdp2_receiver_run(&receiver);

    if (rc != CFDP_OK) {
        fprintf(stderr, "Receiver run failed: %s\n",
                cfdp_status_str(rc));
        goto cleanup_receiver;
    }

    rc = cfdp2_receiver_write_file(&receiver);

    if (rc != CFDP_OK) {
        fprintf(stderr, "Write file failed: %s\n",
                cfdp_status_str(rc));
        goto cleanup_receiver;
    }

    printf("\n[RECEIVER2] Transfer complete. State: %s\n",
           cfdp2_recv_state_str(receiver.state));

cleanup_receiver:
    cfdp2_receiver_destroy(&receiver);

cleanup_transport:
    cfdp_udp_transport_destroy(&udp_transport);

    return (rc == CFDP_OK) ? 0 : 1;
}