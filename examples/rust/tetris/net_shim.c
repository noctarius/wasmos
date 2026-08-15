/* net_shim.c - thin C transport adapter for the Rust Tetris app.
 *
 * The Rust side is #![no_std] with no heap and cannot re-implement the SPSC
 * byte-ring atomics that net-stack shares page-for-page. So the networking
 * transport lives here, but it is a *thin* adapter: the connect (client) and
 * listen/accept (server) handshakes, plus the ring setup, are the shared
 * non-blocking `wasmos_net_*` helpers in `wasmos/net.h`. This shim only owns the
 * single gameplay socket and its reply endpoint, and exposes a small C ABI the
 * Rust game loop drives.
 *
 * Both roles are non-blocking: `tnet_host_begin` / `tnet_join_begin` arm a
 * handshake and return immediately; the game loop then calls `tnet_net_advance`
 * whenever the reply endpoint (`tnet_reply_ep()`) doorbell wakes it, until the
 * socket is READY. Gameplay traffic uses `tnet_send` / `tnet_poll`. Nothing here
 * blocks the process, so the window keeps rendering "waiting for opponent". */
#include <stdint.h>

#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/net.h"
#include "wasmos/ringbuf.h"

#define TNET_RING_CAP 4096u /* per-direction data bytes, power of two */

static wasmos_net_tcp_t g_sock; /* the one gameplay socket */
static int32_t g_ready = 0;     /* g_sock holds a live connected/accepted socket */
static int32_t g_reply_ep = -1; /* endpoint net-stack replies/doorbells arrive on */

/* Resolve the net.stack service endpoint, spinning until it registers. This is a
 * process-manager lookup of a service that sysinit brings up before this app, so
 * it returns promptly; it is not the peer/network wait. */
static int32_t tnet_lookup_stack(int32_t proc_ep, int32_t reply_ep, int32_t* request_id) {
    int32_t stack_ep = -1;
    for (int32_t spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", (*request_id)++);
        if (stack_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    return stack_ep;
}

/* Ensure the reply endpoint exists and resolve net.stack. Returns the stack
 * endpoint (>= 0) or a negative code on failure. */
static int32_t tnet_bootstrap(int32_t proc_ep) {
    int32_t request_id = 1;
    if (g_reply_ep < 0) {
        g_reply_ep = wasmos_ipc_create_endpoint();
    }
    if (g_reply_ep < 0) {
        return -1;
    }
    int32_t stack_ep = tnet_lookup_stack(proc_ep, g_reply_ep, &request_id);
    if (stack_ep < 0) {
        return -2;
    }
    return stack_ep;
}

/* Client role: arm a non-blocking connect to addr_no:port. Returns 0 armed, or a
 * negative code (drive to completion with tnet_net_advance). */
int32_t tnet_join_begin(int32_t proc_ep, uint32_t addr_no, uint32_t port) {
    int32_t stack_ep = tnet_bootstrap(proc_ep);
    if (stack_ep < 0) {
        return stack_ep;
    }
    return wasmos_net_tcp_connect_begin(
        &g_sock, stack_ep, g_reply_ep, addr_no, (uint16_t)port, TNET_RING_CAP, 1, 0u, 0);
}

/* Server role: arm a non-blocking listen/accept on `port`. Returns 0 armed, or a
 * negative code (drive to completion with tnet_net_advance). */
int32_t tnet_host_begin(int32_t proc_ep, uint32_t port) {
    int32_t stack_ep = tnet_bootstrap(proc_ep);
    if (stack_ep < 0) {
        return stack_ep;
    }
    return wasmos_net_tcp_listen_begin(
        &g_sock, stack_ep, g_reply_ep, (uint16_t)port, TNET_RING_CAP, 1);
}

/* Doorbell handler for the armed handshake. Call from the game loop whenever the
 * reply endpoint wakes it. Returns 1 when the match socket is ready (g_sock
 * live), 0 while still handshaking, or a negative code on failure. */
int32_t tnet_net_advance(void) {
    int32_t rc = wasmos_net_tcp_advance(&g_sock);
    if (rc == 1) {
        g_ready = 1;
    }
    return rc;
}

/* The endpoint net-stack replies and doorbells arrive on, for the caller to add
 * to its selector. Returns -1 before the first tnet_join_begin/tnet_host_begin
 * creates it. */
int32_t tnet_reply_ep(void) {
    return g_reply_ep;
}

/* Queue `len` bytes to the peer and ring the TX doorbell. Does not return until
 * every byte is in the TX ring: a full ring is retried behind sched_yield, so a
 * peer that stops draining stalls the caller here. The 4 KiB ring holds many
 * gameplay frames, which keeps that path cold in practice. */
int32_t tnet_send(const void* data, int32_t len) {
    if (!g_ready) {
        return -1;
    }
    return wasmos_net_tcp_send(&g_sock, data, len);
}

/* Non-blocking receive: drain any pending net-stack doorbells/replies that woke
 * the selector, then copy up to `cap` bytes out of the RX ring. Returns the
 * count read (0 when nothing is queued), or -2 when the peer has closed and the
 * ring is drained. */
int32_t tnet_poll(void* buf, int32_t cap) {
    if (!g_ready) {
        return -1;
    }
    /* WARP-safe non-blocking drain: ipc_drain never blocks (ipc_select_one does).
     * The doorbells carry no payload; the data is already in the RX ring. */
    while (wasmos_ipc_drain(g_sock.reply_ep) > 0) {
        /* no-op: consume the wakeup */
    }
    uint32_t n = wasmos_ringbuf_read(&g_sock.rx, buf, (uint32_t)cap);
    if (n == 0u && (wasmos_ringbuf_flags(&g_sock.rx) & WASMOS_RINGBUF_FLAG_PEER_CLOSED) &&
        wasmos_ringbuf_is_empty(&g_sock.rx)) {
        return -2;
    }
    return (int32_t)n;
}

/* Close the gameplay socket and mark it unusable, so later tnet_send/tnet_poll
 * calls return -1. Idempotent, and a no-op when no socket was ever ready. The
 * reply endpoint is left in place. */
void tnet_close(void) {
    if (g_ready) {
        wasmos_net_tcp_close(&g_sock);
        g_ready = 0;
    }
}
