/* net_udp_echo - UDP socket-ring smoke client for the native net-stack. */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define RING_BYTES 256u
#define RING_HEADER_BYTES 64u
#define RING_MAGIC 0x474E5257u /* 'WRNG' */

typedef struct __attribute__((packed, aligned(64))) {
    uint32_t magic;
    uint16_t version;
    uint16_t hdr_bytes;
    uint32_t capacity;
    uint32_t flags;
    uint32_t write;
    uint32_t pad_w[7];
    uint32_t read;
    uint32_t pad_r[3];
} ring_header_t;

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
    ring_header_t header = {RING_MAGIC, 1u, RING_HEADER_BYTES,
                            RING_BYTES, 0u, 0u, {0u}, 0u, {0u}};
    if (wasmos_xfer_buffer_write(tx_bid, addr_cast(int32_t, &header), sizeof(header), 0) != 0 ||
        wasmos_xfer_buffer_write(rx_bid, addr_cast(int32_t, &header), sizeof(header), 0) != 0) {
        puts("[net-udp-echo] ring init failed");
        return 1;
    }
    int32_t tx_grant = wasmos_xfer_buffer_borrow(stack_ep, tx_bid,
                                                  WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t rx_grant = wasmos_xfer_buffer_borrow(stack_ep, rx_bid,
                                                  WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    int32_t desc_grant = wasmos_xfer_buffer_borrow(stack_ep, desc_bid, WASMOS_BUFFER_GRANT_READ);
    if (tx_grant < 0 || rx_grant < 0 || desc_grant < 0) {
        puts("[net-udp-echo] grant failed");
        return 1;
    }
    net_socket_open_descriptor_v1_t descriptor = {
        NET_SOCKET_OPEN_DESCRIPTOR_VERSION,
        sizeof(net_socket_open_descriptor_v1_t),
        NET_SOCKET_AF_INET,
        NET_SOCKET_DGRAM,
        0u,
        0u,
        (uint32_t)tx_bid,
        (uint32_t)tx_grant,
        RING_HEADER_BYTES + RING_BYTES,
        (uint32_t)rx_bid,
        (uint32_t)rx_grant,
        RING_HEADER_BYTES + RING_BYTES,
    };
    if (wasmos_xfer_buffer_write(desc_bid, addr_cast(int32_t, &descriptor), sizeof(descriptor), 0) !=
            0 ||
        wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_SOCKET_OPEN, request_id, desc_bid, desc_grant,
                        sizeof(descriptor), 0) != 0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP ||
        (int32_t)message.arg0 < 0) {
        puts("[net-udp-echo] open failed");
        return 1;
    }
    int32_t socket_id = message.arg0;
    /* lwIP's ip_addr_set_ip4_u32 takes the network-order IPv4 word. */
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_CONNECT, request_id, socket_id, 5555u,
                        0x0202000Au, 0) != 0 ||
        recv_reply(reply_ep, request_id++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts("[net-udp-echo] connect failed");
        return 1;
    }
    static const uint8_t payload[] = "wasmos-udp-echo";
    uint8_t record[4 + sizeof(payload) - 1];
    uint32_t len = sizeof(payload) - 1u;
    record[0] = (uint8_t)len;
    record[1] = 0u;
    record[2] = 0u;
    record[3] = 0u;
    for (uint32_t i = 0; i < len; ++i) {
        record[4u + i] = payload[i];
    }
    uint32_t write = sizeof(record);
    if (wasmos_xfer_buffer_write(tx_bid, addr_cast(int32_t, record), sizeof(record), RING_HEADER_BYTES) !=
            0 ||
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
            length_bytes[0] != len || length_bytes[1] != 0 || length_bytes[2] != 0 ||
            length_bytes[3] != 0 ||
            wasmos_xfer_buffer_read(rx_bid, addr_cast(int32_t, response), len,
                                    RING_HEADER_BYTES + 4u) != 0) {
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
