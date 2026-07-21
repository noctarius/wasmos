/* net_tcp_server - passive-open TCP echo server for the native net-stack.
 *
 * Opens a stream socket, binds a port, listens, and posts one accept slot (a
 * second client-owned TX/RX ring pair). When a peer connects, net-stack pairs
 * the connection with the posted rings and answers the deferred NET_IPC_ACCEPT
 * with the accepted socket id. The server then echoes the first inbound segment
 * back through the accepted socket's TX ring.
 *
 * The host reaches the guest listener through a QEMU SLIRP hostfwd rule (see
 * tests/test_net_stack_tcp_server_e2e.py). */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define RING_BYTES 256u
#define RING_HEADER_BYTES 64u
#define RING_MAGIC 0x474E5257u /* 'WRNG' */
#define RING_OFF_WRITE 16u
#define LISTEN_PORT 5571u

typedef struct {
    int32_t tx_bid;
    int32_t rx_bid;
    int32_t desc_bid;
    int32_t tx_grant;
    int32_t rx_grant;
    int32_t desc_grant;
} sock_bufs_t;

static void put_u32(uint8_t* out, uint32_t offset, uint32_t value) {
    out[offset] = (uint8_t)value;
    out[offset + 1u] = (uint8_t)(value >> 8u);
    out[offset + 2u] = (uint8_t)(value >> 16u);
    out[offset + 3u] = (uint8_t)(value >> 24u);
}

static void put_u16(uint8_t* out, uint32_t offset, uint16_t value) {
    out[offset] = (uint8_t)value;
    out[offset + 1u] = (uint8_t)(value >> 8u);
}

static int recv_on(int32_t ep, wasmos_ipc_message_t* message) {
    for (int spin = 0; spin < 400000; ++spin) {
        if (wasmos_ipc_select_one(ep) == 1) {
            wasmos_ipc_message_read_last(message);
            return 0;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

static int recv_reply(int32_t ep, int32_t request_id, wasmos_ipc_message_t* message) {
    for (int rounds = 0; rounds < 64; ++rounds) {
        if (recv_on(ep, message) != 0) {
            return -1;
        }
        if (message->request_id == request_id) {
            return 0;
        }
    }
    return -1;
}

/* Acquire a TX/RX ring pair plus a descriptor buffer, initialize the ring
 * headers, borrow all three to net-stack, and fill in the open descriptor. */
static int prepare_bufs(int32_t stack_ep, sock_bufs_t* b) {
    uint8_t ring_header[RING_HEADER_BYTES] = {0};
    uint8_t desc[sizeof(net_socket_open_descriptor_v1_t)] = {0};
    b->tx_bid = wasmos_xfer_buffer_acquire(RING_HEADER_BYTES + RING_BYTES);
    b->rx_bid = wasmos_xfer_buffer_acquire(RING_HEADER_BYTES + RING_BYTES);
    b->desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(net_socket_open_descriptor_v1_t));
    if (b->tx_bid < 0 || b->rx_bid < 0 || b->desc_bid < 0) {
        return -1;
    }
    put_u32(ring_header, 0u, RING_MAGIC);
    put_u16(ring_header, 4u, 1u);
    put_u16(ring_header, 6u, RING_HEADER_BYTES);
    put_u32(ring_header, 8u, RING_BYTES);
    if (wasmos_xfer_buffer_write(b->tx_bid, addr_cast(int32_t, ring_header), sizeof(ring_header),
                                 0) != 0 ||
        wasmos_xfer_buffer_write(b->rx_bid, addr_cast(int32_t, ring_header), sizeof(ring_header),
                                 0) != 0) {
        return -1;
    }
    b->tx_grant = wasmos_xfer_buffer_borrow(stack_ep, b->tx_bid,
                                            WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    b->rx_grant = wasmos_xfer_buffer_borrow(stack_ep, b->rx_bid,
                                            WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    b->desc_grant = wasmos_xfer_buffer_borrow(stack_ep, b->desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (b->tx_grant < 0 || b->rx_grant < 0 || b->desc_grant < 0) {
        return -1;
    }
    put_u16(desc, 0u, NET_SOCKET_OPEN_DESCRIPTOR_VERSION);
    put_u16(desc, 2u, sizeof(desc));
    put_u32(desc, 4u, NET_SOCKET_AF_INET);
    put_u32(desc, 8u, NET_SOCKET_STREAM);
    put_u32(desc, 20u, (uint32_t)b->tx_bid);
    put_u32(desc, 24u, (uint32_t)b->tx_grant);
    put_u32(desc, 28u, RING_HEADER_BYTES + RING_BYTES);
    put_u32(desc, 32u, (uint32_t)b->rx_bid);
    put_u32(desc, 36u, (uint32_t)b->rx_grant);
    put_u32(desc, 40u, RING_HEADER_BYTES + RING_BYTES);
    if (wasmos_xfer_buffer_write(b->desc_bid, addr_cast(int32_t, desc), sizeof(desc), 0) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t request_id = 1;
    int32_t stack_ep = -1;
    wasmos_ipc_message_t message;
    sock_bufs_t listen_bufs;
    sock_bufs_t accept_bufs;
    int32_t listen_id;
    int32_t accept_id;
    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[net-tcp-srv] setup failed");
        return 1;
    }
    for (int spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", request_id++);
        if (stack_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (stack_ep < 0) {
        puts("[net-tcp-srv] no net.stack");
        return 1;
    }

    if (prepare_bufs(stack_ep, &listen_bufs) != 0) {
        puts("[net-tcp-srv] listen buffer setup failed");
        return 1;
    }
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_SOCKET_OPEN, request_id, listen_bufs.desc_bid,
                        listen_bufs.desc_grant, sizeof(net_socket_open_descriptor_v1_t), 0) != 0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP ||
        (int32_t)message.arg0 < 0) {
        puts("[net-tcp-srv] open failed");
        return 1;
    }
    listen_id = message.arg0;
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_BIND, request_id, listen_id, LISTEN_PORT, 0u,
                        0) != 0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts("[net-tcp-srv] bind failed");
        return 1;
    }
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_LISTEN, request_id, listen_id, 0u, 0u, 0) !=
            0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts("[net-tcp-srv] listen failed");
        return 1;
    }

    /* Post one accept slot (rings for the future connection), then announce. */
    if (prepare_bufs(stack_ep, &accept_bufs) != 0) {
        puts("[net-tcp-srv] accept buffer setup failed");
        return 1;
    }
    int32_t accept_rid = request_id++;
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_ACCEPT, accept_rid, listen_id,
                        accept_bufs.desc_bid, accept_bufs.desc_grant,
                        sizeof(net_socket_open_descriptor_v1_t)) != 0) {
        puts("[net-tcp-srv] accept post failed");
        return 1;
    }
    puts("[net-tcp-srv] listening");
    if (recv_reply(reply_ep, accept_rid, &message) != 0 || message.type != NET_IPC_RESP ||
        (int32_t)message.arg0 < 0) {
        puts("[net-tcp-srv] accept failed");
        return 1;
    }
    accept_id = message.arg0;
    puts("[net-tcp-srv] accepted");

    /* Echo the first inbound segment: read the accepted RX ring, write the same
     * bytes into the accepted TX ring, and ring the TX doorbell. */
    for (int rounds = 0; rounds < 64; ++rounds) {
        if (recv_on(reply_ep, &message) != 0) {
            break;
        }
        if (message.type != NET_IPC_RX_NOTIFY || message.arg0 != (uint32_t)accept_id) {
            continue;
        }
        uint32_t rx_write = 0u;
        if (wasmos_xfer_buffer_read(accept_bufs.rx_bid, addr_cast(int32_t, &rx_write),
                                    sizeof(rx_write), RING_OFF_WRITE) != 0) {
            break;
        }
        if (rx_write == 0u) {
            continue;
        }
        if (rx_write > RING_BYTES) {
            rx_write = RING_BYTES;
        }
        uint8_t buf[RING_BYTES];
        if (wasmos_xfer_buffer_read(accept_bufs.rx_bid, addr_cast(int32_t, buf), rx_write,
                                    RING_HEADER_BYTES) != 0 ||
            wasmos_xfer_buffer_write(accept_bufs.tx_bid, addr_cast(int32_t, buf), rx_write,
                                     RING_HEADER_BYTES) != 0 ||
            wasmos_xfer_buffer_write(accept_bufs.tx_bid, addr_cast(int32_t, &rx_write),
                                     sizeof(rx_write), RING_OFF_WRITE) != 0 ||
            wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_TX_NOTIFY, request_id++, accept_id, 0, 0,
                            0) != 0) {
            break;
        }
        puts("[net-tcp-srv] echoed");
        return 0;
    }
    puts("[net-tcp-srv] no data");
    return 1;
}
