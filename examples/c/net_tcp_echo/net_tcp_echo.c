/* net_tcp_echo - TCP socket-ring smoke client for the native net-stack.
 *
 * Opens a stream socket backed by client-owned TX/RX byte rings, connects to
 * the QEMU SLIRP host echo server, streams a payload through the TX ring, and
 * verifies the echo arrives in the RX ring. TCP connect is asynchronous in the
 * stack, so the NET_IPC_CONNECT reply only lands once the handshake completes. */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define RING_BYTES 256u
#define RING_HEADER_BYTES 64u
#define RING_MAGIC 0x474E5257u /* 'WRNG' */
/* Producer write-index offset in the ring header (see wasmos/ringbuf.h). */
#define RING_OFF_WRITE 16u
/* SLIRP host gateway 10.0.2.2 as the network-order IPv4 word, TCP echo port. */
#define ECHO_ADDR_V4 0x0202000Au
#define ECHO_PORT 5556u

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
    for (int spin = 0; spin < 300000; ++spin) {
        if (wasmos_ipc_select_one(ep) == 1) {
            wasmos_ipc_message_read_last(message);
            return 0;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

static int recv_reply(int32_t ep, int32_t request_id, wasmos_ipc_message_t* message) {
    for (int rounds = 0; rounds < 32; ++rounds) {
        if (recv_on(ep, message) != 0) {
            return -1;
        }
        if (message->request_id == request_id) {
            return 0;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t request_id = 1;
    int32_t stack_ep = -1;
    wasmos_ipc_message_t message;
    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[net-tcp-echo] setup failed");
        return 1;
    }
    for (int spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", request_id++);
        if (stack_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (stack_ep < 0) {
        puts("[net-tcp-echo] no net.stack");
        return 1;
    }
    puts("[net-tcp-echo] found net.stack");

    int32_t tx_bid = wasmos_xfer_buffer_acquire(RING_HEADER_BYTES + RING_BYTES);
    int32_t rx_bid = wasmos_xfer_buffer_acquire(RING_HEADER_BYTES + RING_BYTES);
    int32_t desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(net_socket_open_descriptor_v1_t));
    if (tx_bid < 0 || rx_bid < 0 || desc_bid < 0) {
        puts("[net-tcp-echo] buffer setup failed");
        return 1;
    }
    uint8_t ring_header[RING_HEADER_BYTES] = {0};
    put_u32(ring_header, 0u, RING_MAGIC);
    put_u16(ring_header, 4u, 1u);
    put_u16(ring_header, 6u, RING_HEADER_BYTES);
    put_u32(ring_header, 8u, RING_BYTES);
    if (wasmos_xfer_buffer_write(tx_bid, addr_cast(int32_t, ring_header), sizeof(ring_header), 0) !=
            0 ||
        wasmos_xfer_buffer_write(rx_bid, addr_cast(int32_t, ring_header), sizeof(ring_header), 0) !=
            0) {
        puts("[net-tcp-echo] ring init failed");
        return 1;
    }
    int32_t tx_grant = wasmos_xfer_buffer_borrow(
        stack_ep, tx_bid, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t rx_grant = wasmos_xfer_buffer_borrow(
        stack_ep, rx_bid, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t desc_grant = wasmos_xfer_buffer_borrow(stack_ep, desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (tx_grant < 0 || rx_grant < 0 || desc_grant < 0) {
        puts("[net-tcp-echo] grant failed");
        return 1;
    }
    uint8_t descriptor[sizeof(net_socket_open_descriptor_v1_t)] = {0};
    put_u16(descriptor, 0u, NET_SOCKET_OPEN_DESCRIPTOR_VERSION);
    put_u16(descriptor, 2u, sizeof(descriptor));
    put_u32(descriptor, 4u, NET_SOCKET_AF_INET);
    put_u32(descriptor, 8u, NET_SOCKET_STREAM);
    put_u32(descriptor, 20u, (uint32_t)tx_bid);
    put_u32(descriptor, 24u, (uint32_t)tx_grant);
    put_u32(descriptor, 28u, RING_HEADER_BYTES + RING_BYTES);
    put_u32(descriptor, 32u, (uint32_t)rx_bid);
    put_u32(descriptor, 36u, (uint32_t)rx_grant);
    put_u32(descriptor, 40u, RING_HEADER_BYTES + RING_BYTES);
    if (wasmos_xfer_buffer_write(desc_bid, addr_cast(int32_t, descriptor), sizeof(descriptor), 0) !=
            0 ||
        wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_SOCKET_OPEN, request_id, desc_bid, desc_grant,
                        sizeof(descriptor), 0) != 0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP ||
        (int32_t)message.arg0 < 0) {
        puts("[net-tcp-echo] open failed");
        return 1;
    }
    int32_t socket_id = message.arg0;

    /* Connect: the reply is deferred until the TCP handshake resolves. */
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_CONNECT, request_id, socket_id, ECHO_PORT,
                        ECHO_ADDR_V4, 0) != 0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP ||
        (int32_t)message.arg0 != NET_STATUS_OK) {
        puts("[net-tcp-echo] connect failed");
        return 1;
    }
    puts("[net-tcp-echo] connected");

    /* Stream the payload: raw bytes into the TX ring's data region, then bump
     * the producer write index and ring the doorbell. */
    static const uint8_t payload[] = "wasmos-tcp-echo";
    uint32_t len = sizeof(payload) - 1u;
    uint32_t write_index = len;
    if (wasmos_xfer_buffer_write(tx_bid, addr_cast(int32_t, payload), len, RING_HEADER_BYTES) !=
            0 ||
        wasmos_xfer_buffer_write(tx_bid, addr_cast(int32_t, &write_index), sizeof(write_index),
                                 RING_OFF_WRITE) != 0 ||
        wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_TX_NOTIFY, request_id++, socket_id, 0, 0, 0) !=
            0) {
        puts("[net-tcp-echo] send failed");
        return 1;
    }

    for (int rounds = 0; rounds < 32; ++rounds) {
        if (recv_on(reply_ep, &message) != 0) {
            break;
        }
        if (message.type != NET_IPC_RX_NOTIFY || message.arg0 != (uint32_t)socket_id) {
            continue;
        }
        uint32_t rx_write = 0u;
        if (wasmos_xfer_buffer_read(rx_bid, addr_cast(int32_t, &rx_write), sizeof(rx_write),
                                    RING_OFF_WRITE) != 0) {
            break;
        }
        if (rx_write < len) {
            /* Echo may stream in more than one segment; wait for the rest. */
            continue;
        }
        uint8_t response[sizeof(payload) - 1];
        if (wasmos_xfer_buffer_read(rx_bid, addr_cast(int32_t, response), len, RING_HEADER_BYTES) !=
            0) {
            break;
        }
        for (uint32_t i = 0; i < len; ++i) {
            if (response[i] != payload[i]) {
                puts("[net-tcp-echo] payload mismatch");
                return 1;
            }
        }
        puts("[net-tcp-echo] echo ok");
        return 0;
    }
    puts("[net-tcp-echo] no echo");
    return 1;
}
