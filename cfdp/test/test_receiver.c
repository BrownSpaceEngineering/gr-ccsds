/*
 * test_receiver.c — Class 1 receiver test process
 *
 * Usage:
 *   ./test_receiver
 *
 * Listens on 127.0.0.1:5001 for an incoming CFDP Class 1 transfer,
 * reassembles the file, verifies the checksum, and writes it to disk.
 *
 * Run this process first, then start test_sender in another terminal.
 */

#include "cfdp.h"

#include <stdio.h>
#include <stdlib.h>

#define RECEIVER_PORT 5001
#define SENDER_PORT   5000

int main(void)
{
    printf("=== CFDP Class 1 Receiver ===\n");
    printf("Listening on port %d ...\n\n", RECEIVER_PORT);

    /* --- Set up UDP transport --- */
    cfdp_udp_transport_t udp_transport;
    cfdp_status_t rc = cfdp_udp_transport_init(
        &udp_transport,
        NULL,           /* bind addr: INADDR_ANY */
        RECEIVER_PORT,
        "127.0.0.1",   /* peer addr (for completeness; recv is passive) */
        SENDER_PORT,
        0              /* no loss injection on receiver side */
    );
    if (rc != CFDP_OK) {
        fprintf(stderr, "Transport init failed: %s\n", cfdp_status_str(rc));
        return 1;
    }

    /* --- Initialize receiver --- */
    cfdp_receiver_t receiver;
    rc = cfdp_receiver_init(
        &receiver,
        /*dest_entity_id=*/ 2,
        /*entity_id_len=*/  1,
        /*seq_num_len=*/    1,
        (cfdp_transport_t *)&udp_transport
    );
    if (rc != CFDP_OK) {
        fprintf(stderr, "Receiver init failed: %s\n", cfdp_status_str(rc));
        cfdp_udp_transport_destroy(&udp_transport);
        return 1;
    }

    /* --- Run until complete --- */
    rc = cfdp_receiver_run(&receiver);
    if (rc != CFDP_OK) {
        fprintf(stderr, "Receiver run failed: %s\n", cfdp_status_str(rc));
        goto cleanup;
    }

    /* --- Write output file --- */
    rc = cfdp_receiver_write_file(&receiver);
    if (rc != CFDP_OK) {
        fprintf(stderr, "Write file failed: %s\n", cfdp_status_str(rc));
        goto cleanup;
    }

    printf("\n[RECV] Transfer complete. State: %s\n",
           cfdp_recv_state_str(receiver.state));

cleanup:
    cfdp_receiver_destroy(&receiver);
    cfdp_udp_transport_destroy(&udp_transport);
    return (rc == CFDP_OK) ? 0 : 1;
}