#ifndef WASMOS_NET_STACK_SOCKET_H
#define WASMOS_NET_STACK_SOCKET_H

#include <stdint.h>

#include "wasmos/ringbuf.h"
#include "wasmos_driver_abi.h"

#define NET_SOCKET_MAX 32u

typedef enum {
    NET_SOCKET_FREE = 0,
    NET_SOCKET_OPEN,
    NET_SOCKET_BOUND,
    NET_SOCKET_CONNECTED,
    NET_SOCKET_LISTENING,
    NET_SOCKET_CLOSING
} net_socket_state_t;

typedef struct {
    net_socket_state_t state;
    uint32_t owner_endpoint;
    uint32_t family;
    uint32_t type;
    uint32_t stack_id;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_addr_v4;
    uint32_t remote_addr_v4;
    uint32_t tx_buffer_id;
    uint32_t tx_borrow_id;
    uint32_t rx_buffer_id;
    uint32_t rx_borrow_id;
    void* pcb; /* struct udp_pcb* or struct tcp_pcb*, owned by net-stack */
    wasmos_ringbuf_t tx_ring;
    wasmos_ringbuf_t rx_ring;
} net_socket_t;

typedef struct {
    net_socket_t sockets[NET_SOCKET_MAX];
} net_socket_pool_t;

void net_socket_pool_init(net_socket_pool_t* pool);
int32_t net_socket_open(net_socket_pool_t* pool, uint32_t owner_endpoint,
                        const net_socket_open_descriptor_v1_t* descriptor, void* tx_base,
                        void* rx_base, uint32_t* out_socket_id);
int32_t net_socket_bind(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id,
                        uint16_t port, uint32_t addr_v4);
int32_t net_socket_connect(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id,
                           uint16_t port, uint32_t addr_v4);
int32_t net_socket_listen(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id);
int32_t net_socket_close(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id);

#endif
