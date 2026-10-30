/*
 * cfdp_transport_udp.c — UDP-based CFDP transport with simulated loss
 *
 * Uses POSIX UDP sockets. The sender binds to a local port and sends to
 * a peer address; the receiver binds to its own port and listens.
 *
 * Packet loss is simulated by randomly dropping send() calls, controlled
 * by drop_pct (0 = no loss, 100 = drop everything).
 *
 * recv() uses a 2-second timeout (SO_RCVTIMEO) so the receiver run loop
 * can check state without blocking forever.
 */

#include "cfdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* Retrieve the UDP-specific private state from the transport */
#define UDP_PRIV(t) ((cfdp_udp_priv_t *)(t)->priv)

typedef struct {
    int      sock_fd;
    struct sockaddr_in peer_addr;
    int      drop_pct;
    unsigned rng_state;   /* xorshift32 state */
} cfdp_udp_priv_t;

/* -----------------------------------------------------------------------
 * Tiny xorshift32 PRNG — no stdlib rand() dependency
 * ---------------------------------------------------------------------- */

static uint32_t xorshift32(unsigned *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = (unsigned)x;
    return x;
}

/* Returns true if this packet should be dropped */
static bool should_drop(cfdp_udp_priv_t *p)
{
    if (p->drop_pct <= 0)   return false;
    if (p->drop_pct >= 100) return true;
    uint32_t r = xorshift32(&p->rng_state) % 100u;
    return (int)r < p->drop_pct;
}

/* -----------------------------------------------------------------------
 * Transport callbacks
 * ---------------------------------------------------------------------- */

static cfdp_status_t udp_send(cfdp_transport_t *t,
                              const uint8_t *buf, size_t len)
{
    cfdp_udp_priv_t *p = UDP_PRIV(t);

    if (should_drop(p)) {
        printf("[TRANSPORT] *** DROPPED PDU (%zu bytes) ***\n", len);
        return CFDP_OK;   /* simulate silent loss */
    }

    ssize_t sent = sendto(p->sock_fd, buf, len, 0,
                          (struct sockaddr *)&p->peer_addr,
                          sizeof(p->peer_addr));
    if (sent < 0) {
        perror("udp_send: sendto");
        return CFDP_ERR_TRANSPORT;
    }
    return CFDP_OK;
}

static int udp_recv(cfdp_transport_t *t, uint8_t *buf, size_t max_len)
{
    cfdp_udp_priv_t *p = UDP_PRIV(t);

    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    ssize_t n = recvfrom(p->sock_fd, buf, max_len, 0,
                         (struct sockaddr *)&src_addr, &src_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* timeout — not fatal */
        }
        perror("udp_recv: recvfrom");
        return CFDP_ERR_TRANSPORT;
    }
    return (int)n;
}

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

cfdp_status_t cfdp_udp_transport_init(cfdp_udp_transport_t *t,
                                      const char *bind_addr,
                                      uint16_t    bind_port,
                                      const char *peer_addr,
                                      uint16_t    peer_port,
                                      int         drop_pct)
{
    /* Allocate private state */
    cfdp_udp_priv_t *p = (cfdp_udp_priv_t *)calloc(1, sizeof(*p));
    if (!p) return CFDP_ERR_IO;

    /* Seed PRNG with time + pointer (good enough for simulation) */
    p->rng_state = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)p;
    if (p->rng_state == 0) p->rng_state = 0xDEADBEEFu;

    p->drop_pct = drop_pct;

    /* Create UDP socket */
    p->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->sock_fd < 0) {
        perror("cfdp_udp_transport_init: socket");
        free(p);
        return CFDP_ERR_TRANSPORT;
    }

    /* Enable address reuse (handy for rapid test restarts) */
    int opt = 1;
    setsockopt(p->sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Receive timeout: 2 seconds */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(p->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Bind to local address */
    struct sockaddr_in local = { .sin_family = AF_INET };
    local.sin_port = htons(bind_port);
    if (bind_addr && strcmp(bind_addr, "") != 0) {
        inet_pton(AF_INET, bind_addr, &local.sin_addr);
    } else {
        local.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(p->sock_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("cfdp_udp_transport_init: bind");
        close(p->sock_fd);
        free(p);
        return CFDP_ERR_TRANSPORT;
    }

    /* Store peer address */
    memset(&p->peer_addr, 0, sizeof(p->peer_addr));
    p->peer_addr.sin_family = AF_INET;
    p->peer_addr.sin_port   = htons(peer_port);
    if (peer_addr && strcmp(peer_addr, "") != 0) {
        inet_pton(AF_INET, peer_addr, &p->peer_addr.sin_addr);
    } else {
        p->peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    /* Wire up the transport interface */
    t->base.send = udp_send;
    t->base.recv = udp_recv;
    t->base.priv = p;

    printf("[TRANSPORT] UDP initialized: bind=0.0.0.0:%u  peer=%s:%u  "
           "drop=%d%%\n",
           bind_port,
           peer_addr ? peer_addr : "127.0.0.1",
           peer_port,
           drop_pct);

    return CFDP_OK;
}

void cfdp_udp_transport_destroy(cfdp_udp_transport_t *t)
{
    cfdp_udp_priv_t *p = UDP_PRIV(&t->base);
    if (p) {
        if (p->sock_fd >= 0) close(p->sock_fd);
        free(p);
        t->base.priv = NULL;
    }
}