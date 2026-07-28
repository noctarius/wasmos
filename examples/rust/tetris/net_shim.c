/* net_shim.c - thin C transport adapter for the Rust Tetris app.
 *
 * The Rust side is #![no_std] with no heap and cannot re-implement the SPSC
 * byte-ring atomics that net-stack shares page-for-page. So the networking
 * transport lives here, reusing the battle-tested header-only helpers
 * (`wasmos/net.h`, `wasmos/ringbuf.h`) for the client path and hand-rolling the
 * passive-open (listen/accept) path that `net.h` does not provide. This object
 * is compiled to a wasm32 object and linked into the Rust cdylib exactly like
 * the coroutine/service-runtime cores are for the other Rust examples.
 *
 * It owns exactly one gameplay socket (`g_sock`). Rust drives it through a small
 * C ABI: `tnet_join` / `tnet_host` establish the peer connection, then the game
 * loop selects on `tnet_reply_ep()` alongside the compositor event endpoint and
 * uses `tnet_send` / `tnet_poll` for the non-blocking gameplay traffic. */
#include <stdint.h>

#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/net.h"
#include "wasmos/ringbuf.h"
#include "wasmos_driver_abi.h"

#define TNET_RING_CAP 4096u   /* per-direction data bytes, power of two */
#define TNET_LISTEN_CAP 256u  /* listen socket rings are unused; keep them tiny */

static wasmos_net_tcp_t g_sock; /* the one gameplay socket */
static int32_t g_ready = 0;     /* g_sock holds a live connected/accepted socket */
static int32_t g_reply_ep = -1; /* endpoint net-stack replies/doorbells arrive on */

/* Resolve the net.stack service endpoint, spinning until it registers. */
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

/* Compose a fresh 64-byte ring header for a ring the peer will attach to, and
 * write it into an (unmapped) xfer buffer via the copy path. Used only for the
 * listen socket's placeholder rings. */
static int32_t tnet_write_ring_header(int32_t bid, uint32_t capacity) {
    uint8_t hdr[WASMOS_RINGBUF_HDR_BYTES];
    for (uint32_t i = 0; i < sizeof(hdr); ++i) {
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

/* Fill a socket-open descriptor for a STREAM socket with the given ring grants. */
static void tnet_fill_desc(net_socket_open_descriptor_v1_t* desc, int32_t tx_bid, int32_t tx_grant,
                           int32_t rx_bid, int32_t rx_grant, uint32_t region) {
    for (uint32_t i = 0; i < sizeof(*desc); ++i) {
        ((uint8_t*)desc)[i] = 0u;
    }
    desc->version = NET_SOCKET_OPEN_DESCRIPTOR_VERSION;
    desc->bytes = (uint16_t)sizeof(*desc);
    desc->family = NET_SOCKET_AF_INET;
    desc->type = NET_SOCKET_STREAM;
    desc->flags = 0u;
    desc->tx_buffer_id = (uint32_t)tx_bid;
    desc->tx_borrow_id = (uint32_t)tx_grant;
    desc->tx_bytes = region;
    desc->rx_buffer_id = (uint32_t)rx_bid;
    desc->rx_borrow_id = (uint32_t)rx_grant;
    desc->rx_bytes = region;
}

/* Acquire a TX/RX ring pair, overlay both into linear memory, build the ring
 * headers in place, and borrow them read/write to net-stack. On success the
 * ringbuf handles are ready to read/write and the returned buffer ids own the
 * backing. */
static int32_t tnet_setup_mapped_rings(int32_t stack_ep, uint32_t cap, int32_t* tx_bid,
                                       int32_t* rx_bid, int32_t* tx_grant, int32_t* rx_grant,
                                       wasmos_ringbuf_t* tx, wasmos_ringbuf_t* rx) {
    uint32_t region = wasmos_ringbuf_bytes_for(cap);
    *tx_bid = wasmos_xfer_buffer_acquire((int32_t)region);
    *rx_bid = wasmos_xfer_buffer_acquire((int32_t)region);
    if (*tx_bid < 0 || *rx_bid < 0) {
        return -1;
    }
    int32_t tx_off = wasmos_xfer_buffer_map(*tx_bid);
    int32_t rx_off = wasmos_xfer_buffer_map(*rx_bid);
    if (tx_off < 0 || rx_off < 0) {
        return -1;
    }
    if (wasmos_ringbuf_init(tx, (void*)(uintptr_t)tx_off, region, cap) != 0 ||
        wasmos_ringbuf_init(rx, (void*)(uintptr_t)rx_off, region, cap) != 0) {
        return -1;
    }
    *tx_grant = wasmos_xfer_buffer_borrow(stack_ep, *tx_bid,
                                          WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    *rx_grant = wasmos_xfer_buffer_borrow(stack_ep, *rx_bid,
                                          WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (*tx_grant < 0 || *rx_grant < 0) {
        return -1;
    }
    return 0;
}

/* Client role: connect out to addr_no:port through the ready-made helper. */
int32_t tnet_join(int32_t proc_ep, uint32_t addr_no, uint32_t port) {
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
    if (wasmos_net_tcp_connect(&g_sock, stack_ep, g_reply_ep, addr_no, (uint16_t)port, TNET_RING_CAP,
                               request_id) != 0) {
        return -3;
    }
    g_ready = 1;
    return 0;
}

/* Server role: open + bind + listen + accept ONE peer. Blocks in ACCEPT until a
 * client connects (the reply is deferred by net-stack until then). */
int32_t tnet_host(int32_t proc_ep, uint32_t port) {
    int32_t request_id = 1;
    wasmos_ipc_message_t reply;
    net_socket_open_descriptor_v1_t desc;
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

    /* Listen socket: net-stack still wants a valid ring descriptor even though
     * the listener never carries data. Give it tiny placeholder rings. */
    int32_t l_tx = wasmos_xfer_buffer_acquire((int32_t)wasmos_ringbuf_bytes_for(TNET_LISTEN_CAP));
    int32_t l_rx = wasmos_xfer_buffer_acquire((int32_t)wasmos_ringbuf_bytes_for(TNET_LISTEN_CAP));
    int32_t l_desc = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (l_tx < 0 || l_rx < 0 || l_desc < 0) {
        return -3;
    }
    if (tnet_write_ring_header(l_tx, TNET_LISTEN_CAP) != 0 ||
        tnet_write_ring_header(l_rx, TNET_LISTEN_CAP) != 0) {
        return -3;
    }
    int32_t l_txg = wasmos_xfer_buffer_borrow(stack_ep, l_tx,
                                              WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t l_rxg = wasmos_xfer_buffer_borrow(stack_ep, l_rx,
                                              WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t l_descg = wasmos_xfer_buffer_borrow(stack_ep, l_desc, WASMOS_BUFFER_GRANT_READ);
    if (l_txg < 0 || l_rxg < 0 || l_descg < 0) {
        return -3;
    }
    tnet_fill_desc(&desc, l_tx, l_txg, l_rx, l_rxg, wasmos_ringbuf_bytes_for(TNET_LISTEN_CAP));
    if (wasmos_xfer_buffer_write(l_desc, addr_cast(int32_t, &desc), (int32_t)sizeof(desc), 0) != 0) {
        return -3;
    }
    int32_t rid = request_id++;
    if (wasmos_ipc_send(stack_ep, g_reply_ep, NET_IPC_SOCKET_OPEN, rid, l_desc, l_descg,
                        (int32_t)sizeof(desc), 0) != 0 ||
        wasmos_net__recv_reply(g_reply_ep, rid, &reply) != 0 || reply.type != NET_IPC_RESP ||
        (int32_t)reply.arg0 < 0) {
        return -4;
    }
    int32_t listen_id = reply.arg0;
    (void)wasmos_xfer_buffer_release(l_desc);

    rid = request_id++;
    if (wasmos_ipc_send(stack_ep, g_reply_ep, NET_IPC_BIND, rid, listen_id, (int32_t)port, 0, 0) !=
            0 ||
        wasmos_net__recv_reply(g_reply_ep, rid, &reply) != 0 || reply.type != NET_IPC_RESP) {
        return -5;
    }
    rid = request_id++;
    if (wasmos_ipc_send(stack_ep, g_reply_ep, NET_IPC_LISTEN, rid, listen_id, 0, 0, 0) != 0 ||
        wasmos_net__recv_reply(g_reply_ep, rid, &reply) != 0 || reply.type != NET_IPC_RESP) {
        return -6;
    }

    /* Accept slot: the accepted connection's real data rings. Overlay them so we
     * can drive them with the ringbuf helpers once a peer arrives. */
    int32_t a_tx, a_rx, a_txg, a_rxg;
    if (tnet_setup_mapped_rings(stack_ep, TNET_RING_CAP, &a_tx, &a_rx, &a_txg, &a_rxg, &g_sock.tx,
                                &g_sock.rx) != 0) {
        return -7;
    }
    int32_t a_desc = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (a_desc < 0) {
        return -7;
    }
    int32_t a_descg = wasmos_xfer_buffer_borrow(stack_ep, a_desc, WASMOS_BUFFER_GRANT_READ);
    if (a_descg < 0) {
        return -7;
    }
    tnet_fill_desc(&desc, a_tx, a_txg, a_rx, a_rxg, wasmos_ringbuf_bytes_for(TNET_RING_CAP));
    if (wasmos_xfer_buffer_write(a_desc, addr_cast(int32_t, &desc), (int32_t)sizeof(desc), 0) != 0) {
        return -7;
    }
    rid = request_id++;
    if (wasmos_ipc_send(stack_ep, g_reply_ep, NET_IPC_ACCEPT, rid, listen_id, a_desc, a_descg,
                        (int32_t)sizeof(desc)) != 0) {
        return -8;
    }
    /* Deferred: blocks here until a client connects (the accept reply is held
     * by net-stack until then). */
    if (wasmos_net__recv_reply(g_reply_ep, rid, &reply) != 0 || reply.type != NET_IPC_RESP ||
        (int32_t)reply.arg0 < 0) {
        return -9;
    }
    (void)wasmos_xfer_buffer_release(a_desc);

    g_sock.stack_ep = stack_ep;
    g_sock.reply_ep = g_reply_ep;
    g_sock.socket_id = (int32_t)reply.arg0;
    g_sock.tx_bid = a_tx;
    g_sock.rx_bid = a_rx;
    g_sock.desc_bid = -1;
    g_sock.tx_grant = a_txg;
    g_sock.rx_grant = a_rxg;
    g_sock.request_id = request_id;
    g_ready = 1;
    return 0;
}

int32_t tnet_reply_ep(void) {
    return g_reply_ep;
}

/* Queue `len` bytes to the peer (rings the TX doorbell). Blocks only on
 * ring-full backpressure, which the tiny fixed gameplay frames never hit. */
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
    wasmos_ipc_message_t scratch;
    if (!g_ready) {
        return -1;
    }
    while (wasmos_ipc_select_one(g_sock.reply_ep) == 1) {
        wasmos_ipc_message_read_last(&scratch);
    }
    uint32_t n = wasmos_ringbuf_read(&g_sock.rx, buf, (uint32_t)cap);
    if (n == 0u && (wasmos_ringbuf_flags(&g_sock.rx) & WASMOS_RINGBUF_FLAG_PEER_CLOSED) &&
        wasmos_ringbuf_is_empty(&g_sock.rx)) {
        return -2;
    }
    return (int32_t)n;
}

void tnet_close(void) {
    if (g_ready) {
        wasmos_net_tcp_close(&g_sock);
        g_ready = 0;
    }
}
