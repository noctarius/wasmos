/* net.h - minimal net-stack client helpers for WASM apps.
 *
 * System-wide, dependency-light entry points for talking to the `net.stack`
 * service. Currently just DNS resolution; socket helpers may grow here so apps
 * stop hand-rolling raw NET_IPC_* traffic.
 */
#ifndef WASMOS_NET_H
#define WASMOS_NET_H

#include <stdint.h>

#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/ringbuf.h"
#include "wasmos_driver_abi.h"

/* Resolve `hostname` to an IPv4 address through net.stack (NET_IPC_RESOLVE).
 *
 * Synchronous from the caller's view: the request is sent and this blocks on
 * `reply_ep` for the reply. net-stack itself never blocks - it defers the reply
 * until lwIP's DNS callback fires, so a slow lookup does not stall the stack.
 * `request_id` must be unique on `reply_ep`. On success returns 0 and writes the
 * resolved address as a network-order IPv4 word (octet a in the low byte, the
 * form `wasmos_ipc`/lwIP use) to *out_addr_no; returns a negative value on any
 * failure (bad args, transport error, NXDOMAIN, or timeout). */
static inline int32_t wasmos_net_resolve(int32_t stack_ep, int32_t reply_ep, const char* hostname,
                                         int32_t request_id, uint32_t* out_addr_no) {
    wasmos_ipc_message_t reply;
    int32_t len = 0;
    int32_t bid;
    int32_t grant;
    int32_t rc;
    if (stack_ep < 0 || reply_ep < 0 || hostname == 0) {
        return -1;
    }
    while (hostname[len] != '\0') {
        len++;
    }
    if (len <= 0) {
        return -1;
    }
    bid = wasmos_xfer_buffer_acquire(len);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, hostname), len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    grant = wasmos_xfer_buffer_borrow(stack_ep, bid, WASMOS_BUFFER_GRANT_READ);
    if (grant < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    /* net-stack maps the name, copies it, and unmaps before it can reply, so the
     * buffer is safe to release once the (possibly deferred) reply arrives. */
    rc = wasmos_ipc_call(stack_ep, reply_ep, NET_IPC_RESOLVE, request_id, bid, grant, len, 0,
                         &reply);
    (void)wasmos_xfer_buffer_release(bid);
    if (rc != 0 || reply.type != NET_IPC_RESP || (int32_t)reply.arg0 != NET_STATUS_OK) {
        return -1;
    }
    if (out_addr_no != 0) {
        *out_addr_no = reply.arg1;
    }
    return 0;
}

/* --- TCP stream sockets over the zero-copy ring data plane ---------------- *
 *
 * A connected TCP socket whose TX/RX byte rings (wasmos/ringbuf.h) are the app's
 * own xfer-buffer objects, overlaid into linear memory with wasmos_xfer_buffer_map
 * and borrowed to net.stack (docs/architecture/22). Payload never travels in IPC:
 * the app writes/reads the rings in place and only exchanges lightweight
 * doorbells (NET_IPC_TX_NOTIFY / NET_IPC_RX_NOTIFY) with net-stack. This is the
 * designed data plane, replacing the copy-based xfer_buffer_read/write poking. */
typedef struct {
    int32_t stack_ep;
    int32_t reply_ep;
    int32_t socket_id;
    int32_t tx_bid;
    int32_t rx_bid;
    int32_t desc_bid;
    int32_t tx_grant;
    int32_t rx_grant;
    int32_t request_id;
    wasmos_ringbuf_t tx; /* app is producer */
    wasmos_ringbuf_t rx; /* app is consumer */
} wasmos_net_tcp_t;

/* Wait for any single message to arrive on `ep` (bounded spin+yield), leaving it
 * as the "last" message. Returns 0 on arrival, -1 if it never came. */
static inline int32_t wasmos_net__recv_on(int32_t ep, wasmos_ipc_message_t* out) {
    for (int32_t spin = 0; spin < 300000; ++spin) {
        if (wasmos_ipc_select_one(ep) == 1) {
            wasmos_ipc_message_read_last(out);
            return 0;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

/* Wait for the reply that matches `request_id`, dropping unrelated messages. */
static inline int32_t wasmos_net__recv_reply(int32_t ep, int32_t request_id,
                                             wasmos_ipc_message_t* out) {
    for (int32_t round = 0; round < 64; ++round) {
        if (wasmos_net__recv_on(ep, out) != 0) {
            return -1;
        }
        if (out->request_id == request_id) {
            return 0;
        }
    }
    return -1;
}

/* Release every resource a socket holds. Safe to call on a partially-set-up
 * socket (fields are -1 until acquired). The ring buffers are unmapped but NOT
 * released: wasm3's unmap does not restore the prior linear-memory PTEs, so
 * freeing the backing here could leave it mapped; process reap reclaims both.
 * TODO(net-tcp): release rings once xfer_buffer_unmap restores PTEs on wasm3. */
static inline void wasmos_net_tcp_close(wasmos_net_tcp_t* s) {
    if (s == 0) {
        return;
    }
    if (s->socket_id >= 0 && s->stack_ep >= 0) {
        (void)wasmos_ipc_send(s->stack_ep, s->reply_ep, NET_IPC_CLOSE, s->request_id++,
                              (uint32_t)s->socket_id, 0, 0, 0);
    }
    if (s->tx_bid >= 0) {
        (void)wasmos_xfer_buffer_unmap(s->tx_bid);
    }
    if (s->rx_bid >= 0) {
        (void)wasmos_xfer_buffer_unmap(s->rx_bid);
    }
    if (s->desc_bid >= 0) {
        (void)wasmos_xfer_buffer_release(s->desc_bid); /* not overlaid; safe to free */
        s->desc_bid = -1;
    }
    s->socket_id = -1;
}

/* Shared implementation for the plain-TCP and TLS connect helpers. `open_flags`
 * is written into the socket-open descriptor (NET_SOCKET_OPEN_FLAG_TLS selects a
 * TLS stream socket); everything else is identical. */
static inline int32_t wasmos_net__connect_flags(wasmos_net_tcp_t* s, int32_t stack_ep,
                                                int32_t reply_ep, uint32_t addr_no, uint16_t port,
                                                uint32_t ring_capacity, int32_t request_id_base,
                                                uint32_t open_flags) {
    wasmos_ipc_message_t reply;
    net_socket_open_descriptor_v1_t desc;
    uint32_t region;
    int32_t tx_off;
    int32_t rx_off;
    int32_t desc_grant;
    int32_t rid;
    if (s == 0 || stack_ep < 0 || reply_ep < 0 || !wasmos_ringbuf_is_pow2(ring_capacity)) {
        return -1;
    }
    s->stack_ep = stack_ep;
    s->reply_ep = reply_ep;
    s->socket_id = -1;
    s->tx_bid = -1;
    s->rx_bid = -1;
    s->desc_bid = -1;
    s->tx_grant = -1;
    s->rx_grant = -1;
    s->request_id = request_id_base;

    region = wasmos_ringbuf_bytes_for(ring_capacity);
    s->tx_bid = wasmos_xfer_buffer_acquire((int32_t)region);
    s->rx_bid = wasmos_xfer_buffer_acquire((int32_t)region);
    s->desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (s->tx_bid < 0 || s->rx_bid < 0 || s->desc_bid < 0) {
        wasmos_net_tcp_close(s);
        return -1;
    }
    /* Overlay both rings into linear memory and build the ring headers in place
     * (the app owns/creates both rings; net-stack attaches to the same pages). */
    tx_off = wasmos_xfer_buffer_map(s->tx_bid);
    rx_off = wasmos_xfer_buffer_map(s->rx_bid);
    if (tx_off < 0 || rx_off < 0 ||
        wasmos_ringbuf_init(&s->tx, (void*)(uintptr_t)tx_off, region, ring_capacity) != 0 ||
        wasmos_ringbuf_init(&s->rx, (void*)(uintptr_t)rx_off, region, ring_capacity) != 0) {
        wasmos_net_tcp_close(s);
        return -1;
    }
    s->tx_grant = wasmos_xfer_buffer_borrow(stack_ep, s->tx_bid,
                                            WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    s->rx_grant = wasmos_xfer_buffer_borrow(stack_ep, s->rx_bid,
                                            WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    desc_grant = wasmos_xfer_buffer_borrow(stack_ep, s->desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (s->tx_grant < 0 || s->rx_grant < 0 || desc_grant < 0) {
        wasmos_net_tcp_close(s);
        return -1;
    }
    for (uint32_t i = 0; i < sizeof(desc); ++i) {
        ((uint8_t*)&desc)[i] = 0u;
    }
    desc.version = NET_SOCKET_OPEN_DESCRIPTOR_VERSION;
    desc.bytes = (uint16_t)sizeof(desc);
    desc.family = NET_SOCKET_AF_INET;
    desc.type = NET_SOCKET_STREAM;
    desc.flags = open_flags;
    desc.tx_buffer_id = (uint32_t)s->tx_bid;
    desc.tx_borrow_id = (uint32_t)s->tx_grant;
    desc.tx_bytes = region;
    desc.rx_buffer_id = (uint32_t)s->rx_bid;
    desc.rx_borrow_id = (uint32_t)s->rx_grant;
    desc.rx_bytes = region;
    rid = s->request_id++;
    if (wasmos_xfer_buffer_write(s->desc_bid, addr_cast(int32_t, &desc), (int32_t)sizeof(desc), 0) !=
            0 ||
        wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_SOCKET_OPEN, rid, s->desc_bid, desc_grant,
                        (int32_t)sizeof(desc), 0) != 0 ||
        wasmos_net__recv_reply(reply_ep, rid, &reply) != 0 || reply.type != NET_IPC_RESP ||
        (int32_t)reply.arg0 < 0) {
        wasmos_net_tcp_close(s);
        return -1;
    }
    s->socket_id = (int32_t)reply.arg0;
    /* net-stack has copied the descriptor; the ring grants stay live but the
     * descriptor buffer is done with. */
    (void)wasmos_xfer_buffer_release(s->desc_bid);
    s->desc_bid = -1;

    rid = s->request_id++;
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_CONNECT, rid, (uint32_t)s->socket_id, port,
                        addr_no, 0) != 0 ||
        wasmos_net__recv_reply(reply_ep, rid, &reply) != 0 || reply.type != NET_IPC_RESP ||
        (int32_t)reply.arg0 != NET_STATUS_OK) {
        wasmos_net_tcp_close(s);
        return -1;
    }
    return 0;
}

/* Open a TCP socket and connect to `addr_no`:`port` (addr_no is a network-order
 * IPv4 word, octet a in the low byte — the form wasmos_net_resolve yields).
 * `ring_capacity` is the per-direction data-region size and MUST be a power of
 * two; ~16-64 KiB is typical. `request_id_base` seeds the socket's private id
 * counter on `reply_ep`. Returns 0 connected, -1 on any failure (the socket is
 * cleaned up). TCP connect is asynchronous in the stack, so this blocks on
 * `reply_ep` until the handshake completes. */
static inline int32_t wasmos_net_tcp_connect(wasmos_net_tcp_t* s, int32_t stack_ep,
                                             int32_t reply_ep, uint32_t addr_no, uint16_t port,
                                             uint32_t ring_capacity, int32_t request_id_base) {
    return wasmos_net__connect_flags(s, stack_ep, reply_ep, addr_no, port, ring_capacity,
                                     request_id_base, 0u);
}

/* Same as wasmos_net_tcp_connect but wraps the stream in TLS (net-stack creates
 * an altcp_tls pcb). Milestone B is NO certificate verification: the handshake
 * is encrypted but the server certificate/hostname are not validated. The
 * connect reply is deferred until the TLS handshake completes. The subsequent
 * wasmos_net_tcp_send/recv/close calls are unchanged — payload is plaintext to
 * the app and encrypted on the wire by net-stack. */
static inline int32_t wasmos_net_tls_connect(wasmos_net_tcp_t* s, int32_t stack_ep,
                                             int32_t reply_ep, uint32_t addr_no, uint16_t port,
                                             uint32_t ring_capacity, int32_t request_id_base) {
    return wasmos_net__connect_flags(s, stack_ep, reply_ep, addr_no, port, ring_capacity,
                                     request_id_base, NET_SOCKET_OPEN_FLAG_TLS);
}

/* Send all `len` bytes, blocking (yield) on ring-full backpressure until they
 * are queued. Returns the count sent (== len) or -1 on a bad socket. */
static inline int32_t wasmos_net_tcp_send(wasmos_net_tcp_t* s, const void* data, int32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    int32_t done = 0;
    if (s == 0 || s->socket_id < 0 || len < 0) {
        return -1;
    }
    while (done < len) {
        uint32_t n = wasmos_ringbuf_write(&s->tx, p + done, (uint32_t)(len - done));
        if (n != 0u) {
            done += (int32_t)n;
        }
        /* Doorbell net-stack to drain the TX ring; when the ring was full the
         * notify plus a yield lets it make room before we retry. */
        (void)wasmos_ipc_send(s->stack_ep, s->reply_ep, NET_IPC_TX_NOTIFY, s->request_id++,
                              (uint32_t)s->socket_id, 0, 0, 0);
        if (n == 0u) {
            (void)wasmos_sched_yield();
        }
    }
    return done;
}

/* Read up to `cap` bytes from the RX ring. Returns the count read (> 0), 0 at
 * end of stream (peer closed and the ring is drained), or -1 on a bad socket or
 * a stall with no data. Blocks (waits on NET_IPC_RX_NOTIFY) when the ring is
 * momentarily empty but the stream is still open. */
static inline int32_t wasmos_net_tcp_recv(wasmos_net_tcp_t* s, void* buf, int32_t cap) {
    wasmos_ipc_message_t msg;
    if (s == 0 || s->socket_id < 0 || cap <= 0) {
        return -1;
    }
    for (;;) {
        uint32_t n = wasmos_ringbuf_read(&s->rx, buf, (uint32_t)cap);
        if (n != 0u) {
            return (int32_t)n;
        }
        if ((wasmos_ringbuf_flags(&s->rx) & WASMOS_RINGBUF_FLAG_PEER_CLOSED) &&
            wasmos_ringbuf_is_empty(&s->rx)) {
            return 0; /* end of stream */
        }
        /* Empty but open: wait for a wakeup, then re-read. The doorbell is edge
         * triggered, but a notify that races this arm is queued on reply_ep and
         * returns immediately, so no wakeup is lost. */
        if (wasmos_net__recv_on(s->reply_ep, &msg) != 0) {
            if (!wasmos_ringbuf_is_empty(&s->rx)) {
                continue;
            }
            return (wasmos_ringbuf_flags(&s->rx) & WASMOS_RINGBUF_FLAG_PEER_CLOSED) ? 0 : -1;
        }
    }
}

#endif /* WASMOS_NET_H */
