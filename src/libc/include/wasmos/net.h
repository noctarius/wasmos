/* net.h - minimal net-stack client helpers for WASM apps.
 *
 * System-wide, dependency-light entry points for talking to the `net.stack`
 * service: DNS resolution plus TCP/TLS stream sockets over the zero-copy ring
 * data plane, so apps need not hand-roll raw NET_IPC_* traffic.
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
 * form lwIP uses) to *out_addr_no; returns a negative value on any failure (bad
 * args, transport error, NXDOMAIN, or timeout). */
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
    if (rc != 0 || reply.type != NET_IPC_RESP || (int32_t)reply.arg0 != WASMOS_ERR_NONE) {
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
 * doorbells (NET_IPC_TX_NOTIFY / NET_IPC_RX_NOTIFY) with net-stack. */
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
    /* Non-blocking handshake state (all zero / WASMOS_NET_HS_IDLE until a
     * *_begin call arms it; stepped by wasmos_net_tcp_advance on each doorbell). */
    int32_t hs_state;       /* WASMOS_NET_HS_* */
    int32_t hs_err;         /* last error code once hs_state == ERROR */
    int32_t hs_pending_rid; /* request_id of the in-flight step */
    int32_t hs_listen_id;   /* passive open: the listening socket id */
    int32_t hs_l_tx_bid;    /* passive open: listen placeholder ring bids */
    int32_t hs_l_rx_bid;
    uint32_t hs_cap;  /* per-direction data-ring capacity */
    uint32_t hs_port; /* bind (passive) / connect (active) port */
    uint32_t hs_addr; /* connect address, network order (active) */
} wasmos_net_tcp_t;

/* Handshake states for wasmos_net_tcp_advance (see wasmos_net_tcp_connect_begin /
 * wasmos_net_tcp_listen_begin). Active = client connect; passive = server accept. */
enum {
    WASMOS_NET_HS_IDLE = 0,
    WASMOS_NET_HS_C_OPEN,    /* active: awaiting SOCKET_OPEN reply */
    WASMOS_NET_HS_C_CONNECT, /* active: awaiting CONNECT reply (deferred) */
    WASMOS_NET_HS_L_OPEN,    /* passive: awaiting listen SOCKET_OPEN reply */
    WASMOS_NET_HS_L_BIND,    /* passive: awaiting BIND reply */
    WASMOS_NET_HS_L_LISTEN,  /* passive: awaiting LISTEN reply */
    WASMOS_NET_HS_L_ACCEPT,  /* passive: awaiting ACCEPT reply (deferred) */
    WASMOS_NET_HS_READY,     /* socket connected/accepted; g_sock live */
    WASMOS_NET_HS_ERROR,     /* handshake failed; see hs_err */
};

/* Distinct handshake error codes so a failure names the step that broke. */
#define WASMOS_NET_HS_ERR_SETUP (-2)   /* ring/desc buffer setup or send failed */
#define WASMOS_NET_HS_ERR_OPEN (-3)    /* SOCKET_OPEN reply was an error */
#define WASMOS_NET_HS_ERR_BIND (-4)    /* BIND reply was an error */
#define WASMOS_NET_HS_ERR_LISTEN (-5)  /* LISTEN reply was an error */
#define WASMOS_NET_HS_ERR_ACCEPT (-6)  /* ACCEPT reply was an error */
#define WASMOS_NET_HS_ERR_CONNECT (-7) /* CONNECT reply was an error */

/* Wait for any single message on `ep`, leaving it as the "last" message.
 * wasmos_ipc_select_one blocks in the kernel until one arrives, so the loop
 * only re-arms after a receive error: it yields and retries a bounded number of
 * times. Returns 0 on arrival, -1 once the retries are exhausted. */
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

/* Acquire a TX/RX data-ring pair, overlay both into linear memory, build the
 * ring headers in place, and borrow them read/write to `stack_ep`. On success
 * s->tx/s->rx are ready to read/write and s->tx_bid/rx_bid/tx_grant/rx_grant own
 * the backing. Returns 0 on success, -1 on any failure (caller cleans up). */
static inline int32_t wasmos_net__setup_rings(wasmos_net_tcp_t* s, int32_t stack_ep, uint32_t cap) {
    uint32_t region = wasmos_ringbuf_bytes_for(cap);
    int32_t tx_off;
    int32_t rx_off;
    s->tx_bid = wasmos_xfer_buffer_acquire((int32_t)region);
    s->rx_bid = wasmos_xfer_buffer_acquire((int32_t)region);
    if (s->tx_bid < 0 || s->rx_bid < 0) {
        return -1;
    }
    tx_off = wasmos_xfer_buffer_map(s->tx_bid);
    rx_off = wasmos_xfer_buffer_map(s->rx_bid);
    if (tx_off < 0 || rx_off < 0 ||
        wasmos_ringbuf_init(&s->tx, (void*)(uintptr_t)tx_off, region, cap) != 0 ||
        wasmos_ringbuf_init(&s->rx, (void*)(uintptr_t)rx_off, region, cap) != 0) {
        return -1;
    }
    s->tx_grant = wasmos_xfer_buffer_borrow(stack_ep, s->tx_bid,
                                            WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    s->rx_grant = wasmos_xfer_buffer_borrow(stack_ep, s->rx_bid,
                                            WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (s->tx_grant < 0 || s->rx_grant < 0) {
        return -1;
    }
    return 0;
}

/* Fill a STREAM socket-open descriptor for the given TX/RX ring grants. `region`
 * is the per-direction ring region size (wasmos_ringbuf_bytes_for(cap)). Taking
 * the ring ids explicitly lets both the data rings (s->tx_bid/…) and a listen
 * socket's placeholder rings share one builder. For a TLS socket
 * (NET_SOCKET_OPEN_FLAG_TLS) the SNI/verification hostname is copied in; `sni` is
 * ignored (may be NULL) for plain TCP. */
static inline void wasmos_net__fill_desc(net_socket_open_descriptor_v1_t* desc, int32_t tx_bid,
                                         int32_t tx_grant, int32_t rx_bid, int32_t rx_grant,
                                         uint32_t region, uint32_t open_flags, const char* sni) {
    uint32_t i;
    for (i = 0; i < sizeof(*desc); ++i) {
        ((uint8_t*)desc)[i] = 0u;
    }
    desc->version = NET_SOCKET_OPEN_DESCRIPTOR_VERSION;
    desc->bytes = (uint16_t)sizeof(*desc);
    desc->family = NET_SOCKET_AF_INET;
    desc->type = NET_SOCKET_STREAM;
    desc->flags = open_flags;
    desc->tx_buffer_id = (uint32_t)tx_bid;
    desc->tx_borrow_id = (uint32_t)tx_grant;
    desc->tx_bytes = region;
    desc->rx_buffer_id = (uint32_t)rx_bid;
    desc->rx_borrow_id = (uint32_t)rx_grant;
    desc->rx_bytes = region;
    desc->sni_len = 0u;
    if ((open_flags & NET_SOCKET_OPEN_FLAG_TLS) && sni != 0) {
        i = 0u;
        while (sni[i] != '\0' && i < NET_SOCKET_SNI_MAX - 1u) {
            desc->sni[i] = (uint8_t)sni[i];
            i++;
        }
        desc->sni_len = (uint16_t)i;
    }
}

/* Placeholder capacity for a listen socket's (never-used) rings; power of two. */
#define WASMOS_NET_LISTEN_CAP 256u

/* Compose a fresh 64-byte ring header into an (unmapped) xfer buffer via the copy
 * path. Used for a listen socket's placeholder rings, which never carry data. */
static inline int32_t wasmos_net__write_ring_header(int32_t bid, uint32_t capacity) {
    uint8_t hdr[WASMOS_RINGBUF_HDR_BYTES];
    uint32_t i;
    for (i = 0u; i < sizeof(hdr); ++i) {
        hdr[i] = 0u;
    }
    /* magic@0, version@4, hdr_bytes@6, capacity@8 (little-endian) */
    hdr[0] = (uint8_t)(WASMOS_RINGBUF_MAGIC & 0xFFu);
    hdr[1] = (uint8_t)((WASMOS_RINGBUF_MAGIC >> 8) & 0xFFu);
    hdr[2] = (uint8_t)((WASMOS_RINGBUF_MAGIC >> 16) & 0xFFu);
    hdr[3] = (uint8_t)((WASMOS_RINGBUF_MAGIC >> 24) & 0xFFu);
    hdr[4] = (uint8_t)(WASMOS_RINGBUF_VERSION & 0xFFu);
    hdr[6] = (uint8_t)(WASMOS_RINGBUF_HDR_BYTES & 0xFFu);
    hdr[7] = (uint8_t)((WASMOS_RINGBUF_HDR_BYTES >> 8) & 0xFFu);
    hdr[8] = (uint8_t)(capacity & 0xFFu);
    hdr[9] = (uint8_t)((capacity >> 8) & 0xFFu);
    hdr[10] = (uint8_t)((capacity >> 16) & 0xFFu);
    hdr[11] = (uint8_t)((capacity >> 24) & 0xFFu);
    return wasmos_xfer_buffer_write(bid, addr_cast(int32_t, hdr), (int32_t)sizeof(hdr), 0);
}

/* Reset a socket to the "nothing acquired" baseline: every buffer id / grant /
 * socket id is -1 and the handshake state is IDLE. Callers MUST run this before
 * any setup so wasmos_net_tcp_close never frees a stale/garbage id (matters for
 * static sockets that start zero-initialized, where id 0 is a valid buffer). */
static inline void wasmos_net__reset(wasmos_net_tcp_t* s, int32_t stack_ep, int32_t reply_ep,
                                     int32_t request_id_base) {
    s->stack_ep = stack_ep;
    s->reply_ep = reply_ep;
    s->socket_id = -1;
    s->tx_bid = -1;
    s->rx_bid = -1;
    s->desc_bid = -1;
    s->tx_grant = -1;
    s->rx_grant = -1;
    s->request_id = request_id_base;
    s->hs_state = WASMOS_NET_HS_IDLE;
    s->hs_err = 0;
    s->hs_pending_rid = 0;
    s->hs_listen_id = -1;
    s->hs_l_tx_bid = -1;
    s->hs_l_rx_bid = -1;
    s->hs_cap = 0u;
    s->hs_port = 0u;
    s->hs_addr = 0u;
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
    /* Listen placeholder rings are copy-poked (never overlaid), so release them. */
    if (s->hs_l_tx_bid >= 0) {
        (void)wasmos_xfer_buffer_release(s->hs_l_tx_bid);
        s->hs_l_tx_bid = -1;
    }
    if (s->hs_l_rx_bid >= 0) {
        (void)wasmos_xfer_buffer_release(s->hs_l_rx_bid);
        s->hs_l_rx_bid = -1;
    }
    s->socket_id = -1;
}

/* Shared implementation for the plain-TCP and TLS connect helpers. `open_flags`
 * is written into the socket-open descriptor (NET_SOCKET_OPEN_FLAG_TLS selects a
 * TLS stream socket). `sni` is the server hostname for TLS certificate/hostname
 * verification; it is written into the descriptor's sni field and ignored (may
 * be NULL) for plain TCP. The two paths are otherwise identical. */
static inline int32_t wasmos_net__connect_flags(wasmos_net_tcp_t* s, int32_t stack_ep,
                                                int32_t reply_ep, uint32_t addr_no, uint16_t port,
                                                uint32_t ring_capacity, int32_t request_id_base,
                                                uint32_t open_flags, const char* sni) {
    wasmos_ipc_message_t reply;
    net_socket_open_descriptor_v1_t desc;
    uint32_t region;
    int32_t desc_grant;
    int32_t rid;
    if (s == 0 || stack_ep < 0 || reply_ep < 0 || !wasmos_ringbuf_is_pow2(ring_capacity)) {
        return -1;
    }
    wasmos_net__reset(s, stack_ep, reply_ep, request_id_base);

    region = wasmos_ringbuf_bytes_for(ring_capacity);
    /* Overlay both rings into linear memory and build the ring headers in place
     * (the app owns/creates both rings; net-stack attaches to the same pages). */
    s->desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (wasmos_net__setup_rings(s, stack_ep, ring_capacity) != 0 || s->desc_bid < 0) {
        wasmos_net_tcp_close(s);
        return -1;
    }
    desc_grant = wasmos_xfer_buffer_borrow(stack_ep, s->desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (desc_grant < 0) {
        wasmos_net_tcp_close(s);
        return -1;
    }
    wasmos_net__fill_desc(&desc, s->tx_bid, s->tx_grant, s->rx_bid, s->rx_grant, region, open_flags,
                          sni);
    rid = s->request_id++;
    if (wasmos_xfer_buffer_write(s->desc_bid, addr_cast(int32_t, &desc), (int32_t)sizeof(desc),
                                 0) != 0 ||
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
        (int32_t)reply.arg0 != WASMOS_ERR_NONE) {
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
                                     request_id_base, 0u, 0);
}

/* Same as wasmos_net_tcp_connect but wraps the stream in TLS (net-stack creates
 * an altcp_tls pcb). net-stack validates the server certificate chain against
 * its CA trust store and checks `sni` against the certificate CN/SAN (also sent
 * as the SNI extension). `sni` MUST be the server hostname (or the IP literal
 * for an IP-based connection); an empty/NULL sni is rejected by net-stack for a
 * TLS socket. The connect reply is deferred until the TLS handshake completes
 * (it fails if verification fails). wasmos_net_tcp_send/recv/close work the same
 * on the result — payload is plaintext to the app and encrypted on the wire by
 * net-stack. */
static inline int32_t wasmos_net_tls_connect(wasmos_net_tcp_t* s, int32_t stack_ep,
                                             int32_t reply_ep, uint32_t addr_no, uint16_t port,
                                             uint32_t ring_capacity, int32_t request_id_base,
                                             const char* sni) {
    return wasmos_net__connect_flags(s, stack_ep, reply_ep, addr_no, port, ring_capacity,
                                     request_id_base, NET_SOCKET_OPEN_FLAG_TLS, sni);
}

/* --- Non-blocking handshake (event-loop driven) -------------------------- *
 *
 * The blocking connect/listen above stall the process in-hostcall waiting for
 * net-stack's deferred replies, which freezes an app that also renders a window.
 * The pair below splits a handshake into a fire-and-return `*_begin` plus
 * `wasmos_net_tcp_advance`, a doorbell handler the app calls from its existing
 * event loop whenever the reply endpoint wakes it. No busy-poll, no blocking:
 * every net-stack reply is an IPC doorbell on `reply_ep`, and the loop simply
 * steps the state machine as each arrives. */

static inline int32_t wasmos_net__hs_err_for(int32_t state) {
    switch (state) {
    case WASMOS_NET_HS_C_OPEN:
    case WASMOS_NET_HS_L_OPEN:
        return WASMOS_NET_HS_ERR_OPEN;
    case WASMOS_NET_HS_L_BIND:
        return WASMOS_NET_HS_ERR_BIND;
    case WASMOS_NET_HS_L_LISTEN:
        return WASMOS_NET_HS_ERR_LISTEN;
    case WASMOS_NET_HS_L_ACCEPT:
        return WASMOS_NET_HS_ERR_ACCEPT;
    case WASMOS_NET_HS_C_CONNECT:
        return WASMOS_NET_HS_ERR_CONNECT;
    default:
        return WASMOS_NET_HS_ERR_SETUP;
    }
}

/* Tear down and latch the handshake into ERROR with a step-specific code. */
static inline int32_t wasmos_net__hs_fail(wasmos_net_tcp_t* s, int32_t code) {
    wasmos_net_tcp_close(s);
    s->hs_state = WASMOS_NET_HS_ERROR;
    s->hs_err = code;
    return code;
}

/* Client role, step 1: overlay the data rings and send SOCKET_OPEN, then return
 * immediately. Drive to completion with wasmos_net_tcp_advance. `flags`/`sni`
 * mirror wasmos_net_tls_connect (pass 0/NULL for plain TCP). Returns 0 armed, or
 * a negative WASMOS_NET_HS_ERR_* if the initial setup/send failed. */
static inline int32_t wasmos_net_tcp_connect_begin(wasmos_net_tcp_t* s, int32_t stack_ep,
                                                   int32_t reply_ep, uint32_t addr_no,
                                                   uint16_t port, uint32_t ring_capacity,
                                                   int32_t request_id_base, uint32_t flags,
                                                   const char* sni) {
    net_socket_open_descriptor_v1_t desc;
    uint32_t region;
    int32_t desc_grant;
    if (s == 0 || stack_ep < 0 || reply_ep < 0 || !wasmos_ringbuf_is_pow2(ring_capacity)) {
        return WASMOS_NET_HS_ERR_SETUP;
    }
    wasmos_net__reset(s, stack_ep, reply_ep, request_id_base);
    s->hs_cap = ring_capacity;
    s->hs_port = port;
    s->hs_addr = addr_no;
    region = wasmos_ringbuf_bytes_for(ring_capacity);
    s->desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (wasmos_net__setup_rings(s, stack_ep, ring_capacity) != 0 || s->desc_bid < 0) {
        return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
    }
    desc_grant = wasmos_xfer_buffer_borrow(stack_ep, s->desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (desc_grant < 0) {
        return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
    }
    wasmos_net__fill_desc(&desc, s->tx_bid, s->tx_grant, s->rx_bid, s->rx_grant, region, flags,
                          sni);
    s->hs_pending_rid = s->request_id++;
    if (wasmos_xfer_buffer_write(s->desc_bid, addr_cast(int32_t, &desc), (int32_t)sizeof(desc),
                                 0) != 0 ||
        wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_SOCKET_OPEN, s->hs_pending_rid, s->desc_bid,
                        desc_grant, (int32_t)sizeof(desc), 0) != 0) {
        return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
    }
    s->hs_state = WASMOS_NET_HS_C_OPEN;
    return 0;
}

/* Server role, step 1: open a listen socket (placeholder rings) and send its
 * SOCKET_OPEN, then return immediately. Drive with wasmos_net_tcp_advance, which
 * chains BIND -> LISTEN -> ACCEPT and finalizes the accepted socket. Returns 0
 * armed or a negative WASMOS_NET_HS_ERR_* on setup/send failure. */
static inline int32_t wasmos_net_tcp_listen_begin(wasmos_net_tcp_t* s, int32_t stack_ep,
                                                  int32_t reply_ep, uint16_t port,
                                                  uint32_t ring_capacity, int32_t request_id_base) {
    net_socket_open_descriptor_v1_t desc;
    uint32_t l_region = wasmos_ringbuf_bytes_for(WASMOS_NET_LISTEN_CAP);
    int32_t l_txg;
    int32_t l_rxg;
    int32_t desc_grant;
    if (s == 0 || stack_ep < 0 || reply_ep < 0 || !wasmos_ringbuf_is_pow2(ring_capacity)) {
        return WASMOS_NET_HS_ERR_SETUP;
    }
    wasmos_net__reset(s, stack_ep, reply_ep, request_id_base);
    s->hs_cap = ring_capacity;
    s->hs_port = port;
    s->hs_l_tx_bid = wasmos_xfer_buffer_acquire((int32_t)l_region);
    s->hs_l_rx_bid = wasmos_xfer_buffer_acquire((int32_t)l_region);
    s->desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (s->hs_l_tx_bid < 0 || s->hs_l_rx_bid < 0 || s->desc_bid < 0 ||
        wasmos_net__write_ring_header(s->hs_l_tx_bid, WASMOS_NET_LISTEN_CAP) != 0 ||
        wasmos_net__write_ring_header(s->hs_l_rx_bid, WASMOS_NET_LISTEN_CAP) != 0) {
        return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
    }
    l_txg = wasmos_xfer_buffer_borrow(stack_ep, s->hs_l_tx_bid,
                                      WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    l_rxg = wasmos_xfer_buffer_borrow(stack_ep, s->hs_l_rx_bid,
                                      WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    desc_grant = wasmos_xfer_buffer_borrow(stack_ep, s->desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (l_txg < 0 || l_rxg < 0 || desc_grant < 0) {
        return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
    }
    wasmos_net__fill_desc(&desc, s->hs_l_tx_bid, l_txg, s->hs_l_rx_bid, l_rxg, l_region, 0u, 0);
    s->hs_pending_rid = s->request_id++;
    if (wasmos_xfer_buffer_write(s->desc_bid, addr_cast(int32_t, &desc), (int32_t)sizeof(desc),
                                 0) != 0 ||
        wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_SOCKET_OPEN, s->hs_pending_rid, s->desc_bid,
                        desc_grant, (int32_t)sizeof(desc), 0) != 0) {
        return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
    }
    s->hs_state = WASMOS_NET_HS_L_OPEN;
    return 0;
}

/* Doorbell handler for a non-blocking handshake armed by *_begin. Call it from
 * the app's event loop whenever `s->reply_ep` wakes it. It consumes queued
 * messages and steps the state machine; a content-free RX_NOTIFY that a fast
 * peer races ahead of the completion reply is acknowledged (the payload is
 * already in the RX ring), never dropped as if it were a reply. Returns 1 when
 * the socket is connected/accepted (ready to send/recv), 0 while still
 * handshaking (wait for the next doorbell), or a negative WASMOS_NET_HS_ERR_*. */
static inline int32_t wasmos_net_tcp_advance(wasmos_net_tcp_t* s) {
    if (s == 0) {
        return WASMOS_NET_HS_ERR_SETUP;
    }
    if (s->hs_state == WASMOS_NET_HS_READY) {
        return 1;
    }
    if (s->hs_state == WASMOS_NET_HS_ERROR) {
        return s->hs_err;
    }
    while (wasmos_ipc_drain(s->reply_ep) > 0) {
        int32_t type = wasmos_ipc_last_field(0);
        int32_t rid = wasmos_ipc_last_field(1);
        int32_t arg0 = wasmos_ipc_last_field(2);
        if (rid != s->hs_pending_rid) {
            /* Not this step's reply. During a handshake the only other traffic is
             * a data-arrival doorbell (RX_NOTIFY, request_id 0) that a fast peer
             * queued right behind the accept/connect completion; its payload is
             * already sitting in the RX ring, so consuming it here loses nothing
             * and the caller's loop still reads the ring. Keep draining toward
             * the awaited reply. */
            continue;
        }
        /* A deferred step (CONNECT/ACCEPT) can fail with NET_IPC_ERROR; an
         * immediate step (OPEN/BIND/LISTEN) reports failure the same way. */
        if (type != NET_IPC_RESP) {
            return wasmos_net__hs_fail(s, wasmos_net__hs_err_for(s->hs_state));
        }
        switch (s->hs_state) {
        case WASMOS_NET_HS_C_OPEN:
            if (arg0 < 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_OPEN);
            }
            s->socket_id = arg0;
            (void)wasmos_xfer_buffer_release(s->desc_bid);
            s->desc_bid = -1;
            s->hs_pending_rid = s->request_id++;
            if (wasmos_ipc_send(s->stack_ep, s->reply_ep, NET_IPC_CONNECT, s->hs_pending_rid,
                                (uint32_t)s->socket_id, (int32_t)s->hs_port, (int32_t)s->hs_addr,
                                0) != 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_CONNECT);
            }
            s->hs_state = WASMOS_NET_HS_C_CONNECT;
            break;
        case WASMOS_NET_HS_C_CONNECT:
            if (arg0 != WASMOS_ERR_NONE) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_CONNECT);
            }
            s->hs_state = WASMOS_NET_HS_READY;
            return 1;
        case WASMOS_NET_HS_L_OPEN:
            if (arg0 < 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_OPEN);
            }
            s->hs_listen_id = arg0;
            (void)wasmos_xfer_buffer_release(s->desc_bid);
            s->desc_bid = -1;
            s->hs_pending_rid = s->request_id++;
            if (wasmos_ipc_send(s->stack_ep, s->reply_ep, NET_IPC_BIND, s->hs_pending_rid,
                                s->hs_listen_id, (int32_t)s->hs_port, 0, 0) != 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_BIND);
            }
            s->hs_state = WASMOS_NET_HS_L_BIND;
            break;
        case WASMOS_NET_HS_L_BIND:
            s->hs_pending_rid = s->request_id++;
            if (wasmos_ipc_send(s->stack_ep, s->reply_ep, NET_IPC_LISTEN, s->hs_pending_rid,
                                s->hs_listen_id, 0, 0, 0) != 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_LISTEN);
            }
            s->hs_state = WASMOS_NET_HS_L_LISTEN;
            break;
        case WASMOS_NET_HS_L_LISTEN: {
            /* Listener is live: overlay the accepted connection's real data
             * rings and post the (deferred) accept slot. */
            net_socket_open_descriptor_v1_t desc;
            int32_t desc_grant;
            if (wasmos_net__setup_rings(s, s->stack_ep, s->hs_cap) != 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
            }
            s->desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
            if (s->desc_bid < 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
            }
            desc_grant =
                wasmos_xfer_buffer_borrow(s->stack_ep, s->desc_bid, WASMOS_BUFFER_GRANT_READ);
            if (desc_grant < 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
            }
            wasmos_net__fill_desc(&desc, s->tx_bid, s->tx_grant, s->rx_bid, s->rx_grant,
                                  wasmos_ringbuf_bytes_for(s->hs_cap), 0u, 0);
            s->hs_pending_rid = s->request_id++;
            if (wasmos_xfer_buffer_write(s->desc_bid, addr_cast(int32_t, &desc),
                                         (int32_t)sizeof(desc), 0) != 0 ||
                wasmos_ipc_send(s->stack_ep, s->reply_ep, NET_IPC_ACCEPT, s->hs_pending_rid,
                                s->hs_listen_id, s->desc_bid, desc_grant,
                                (int32_t)sizeof(desc)) != 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_ACCEPT);
            }
            s->hs_state = WASMOS_NET_HS_L_ACCEPT;
            break;
        }
        case WASMOS_NET_HS_L_ACCEPT:
            if (arg0 < 0) {
                return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_ACCEPT);
            }
            s->socket_id = arg0;
            (void)wasmos_xfer_buffer_release(s->desc_bid);
            s->desc_bid = -1;
            s->hs_state = WASMOS_NET_HS_READY;
            return 1;
        default:
            return wasmos_net__hs_fail(s, WASMOS_NET_HS_ERR_SETUP);
        }
        /* Stepped one state; loop to consume the next reply if it is already
         * queued, otherwise the drain returns 0 and the caller waits for the
         * next doorbell. */
    }
    return 0;
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
         * notify plus a yield lets it make room before the retry. */
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
