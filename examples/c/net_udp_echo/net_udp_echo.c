/* net_udp_echo - UDP socket-ring smoke client for the native net-stack.
 *
 * Demonstrates the socket data plane at its lowest level: instead of the
 * wasmos/net.h helpers, this app builds the wire structures itself, so it
 * doubles as documentation of the layout net-stack expects.
 *
 * The setup, which every socket follows:
 *   1. acquire three xfer buffers — a TX ring, an RX ring and a one-shot
 *      descriptor;
 *   2. write the SPSC ring header (magic 'WRNG', version, header size, data
 *      capacity) at offset 0 of each ring;
 *   3. borrow the rings to net-stack R|W and the descriptor R-only, keeping the
 *      grant handles the borrow returns;
 *   4. fill a net_socket_open_descriptor_v1_t with the address family, socket
 *      type and the (buffer id, grant, region size) triple for each ring, and
 *      send it with NET_IPC_SOCKET_OPEN.
 * The reply's arg0 is the socket id. Ownership stays with this process: the
 * rings are its buffers, net-stack only holds a borrow.
 *
 * Sending is: append a length-prefixed net_udp_datagram_record_v1_t (carrying
 * the destination address and port, since the socket is unconnected) after the
 * TX ring header, publish the new write offset into the header, then ring the
 * doorbell with NET_IPC_TX_NOTIFY. Receiving is the mirror, woken by an
 * unsolicited NET_IPC_RX_NOTIFY naming the socket id.
 *
 * Prints "[net-udp-echo] echo ok" and exits 0, or a step-specific line and 1.
 *
 * Preconditions: the "net.stack" service must be running with a configured
 * interface, and the host must answer UDP on 10.0.2.2:5555 (the QEMU SLIRP
 * gateway plus the echo server the test harness starts). */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define RING_BYTES 256u
#define RING_HEADER_BYTES 64u
#define RING_MAGIC 0x474E5257u /* 'WRNG' */

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
    for (int rounds = 0; rounds < 16; ++rounds) {
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
        puts("[net-udp-echo] setup failed");
        return 1;
    }
    for (int spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", request_id++);
        if (stack_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (stack_ep < 0) {
        puts("[net-udp-echo] no net.stack");
        return 1;
    }
    puts("[net-udp-echo] found net.stack");

    int32_t tx_bid = wasmos_xfer_buffer_acquire(RING_HEADER_BYTES + RING_BYTES);
    int32_t rx_bid = wasmos_xfer_buffer_acquire(RING_HEADER_BYTES + RING_BYTES);
    int32_t desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(net_socket_open_descriptor_v1_t));
    if (tx_bid < 0 || rx_bid < 0 || desc_bid < 0) {
        puts("[net-udp-echo] buffer setup failed");
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
        puts("[net-udp-echo] ring init failed");
        return 1;
    }
    int32_t tx_grant = wasmos_xfer_buffer_borrow(
        stack_ep, tx_bid, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t rx_grant = wasmos_xfer_buffer_borrow(
        stack_ep, rx_bid, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t desc_grant = wasmos_xfer_buffer_borrow(stack_ep, desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (tx_grant < 0 || rx_grant < 0 || desc_grant < 0) {
        puts("[net-udp-echo] grant failed");
        return 1;
    }
    uint8_t descriptor[sizeof(net_socket_open_descriptor_v1_t)] = {0};
    put_u16(descriptor, 0u, NET_SOCKET_OPEN_DESCRIPTOR_VERSION);
    put_u16(descriptor, 2u, sizeof(descriptor));
    put_u32(descriptor, 4u, NET_SOCKET_AF_INET);
    put_u32(descriptor, 8u, NET_SOCKET_DGRAM);
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
        puts("[net-udp-echo] open failed");
        return 1;
    }
    int32_t socket_id = message.arg0;
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_BIND, request_id, socket_id, 0u, 0u, 0) != 0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts("[net-udp-echo] bind failed");
        return 1;
    }
    static const uint8_t payload[] = "wasmos-udp-echo";
    uint8_t record[4 + sizeof(net_udp_datagram_record_v1_t) + sizeof(payload) - 1];
    uint32_t len = sizeof(payload) - 1u;
    uint32_t record_bytes = sizeof(net_udp_datagram_record_v1_t) + len;
    record[0] = (uint8_t)record_bytes;
    record[1] = (uint8_t)(record_bytes >> 8u);
    record[2] = 0u;
    record[3] = 0u;
    put_u16(record, 4u, NET_UDP_DATAGRAM_RECORD_VERSION);
    put_u16(record, 6u, NET_UDP_DATAGRAM_FLAG_DESTINATION);
    /* lwIP's ip_addr_set_ip4_u32 takes the network-order IPv4 word. */
    put_u32(record, 8u, 0x0202000Au);
    put_u16(record, 12u, 5555u);
    put_u16(record, 14u, len);
    for (uint32_t i = 0; i < len; ++i) {
        record[16u + i] = payload[i];
    }
    uint32_t write = sizeof(record);
    if (wasmos_xfer_buffer_write(tx_bid, addr_cast(int32_t, record), sizeof(record),
                                 RING_HEADER_BYTES) != 0 ||
        wasmos_xfer_buffer_write(tx_bid, addr_cast(int32_t, &write), sizeof(write), 16) != 0 ||
        wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_TX_NOTIFY, request_id++, socket_id, 0, 0, 0) !=
            0) {
        puts("[net-udp-echo] send failed");
        return 1;
    }
    for (int rounds = 0; rounds < 16; ++rounds) {
        if (recv_on(reply_ep, &message) != 0) {
            break;
        }
        if (message.type != NET_IPC_RX_NOTIFY || message.arg0 != (uint32_t)socket_id) {
            continue;
        }
        uint8_t response[sizeof(payload) - 1];
        uint8_t length_bytes[4];
        if (wasmos_xfer_buffer_read(rx_bid, addr_cast(int32_t, length_bytes), sizeof(length_bytes),
                                    RING_HEADER_BYTES) != 0 ||
            length_bytes[0] != len + sizeof(net_udp_datagram_record_v1_t) || length_bytes[1] != 0 ||
            length_bytes[2] != 0 || length_bytes[3] != 0 ||
            wasmos_xfer_buffer_read(rx_bid, addr_cast(int32_t, response), len,
                                    RING_HEADER_BYTES + 4u +
                                        sizeof(net_udp_datagram_record_v1_t)) != 0) {
            break;
        }
        for (uint32_t i = 0; i < len; ++i) {
            if (response[i] != payload[i]) {
                puts("[net-udp-echo] payload mismatch");
                return 1;
            }
        }
        puts("[net-udp-echo] echo ok");
        return 0;
    }
    puts("[net-udp-echo] no echo");
    return 1;
}
