#include "socket.h"

static net_socket_t* socket_owned(net_socket_pool_t* pool, uint32_t owner_endpoint,
                                  uint32_t socket_id) {
    if (pool == 0 || owner_endpoint == 0u || socket_id >= NET_SOCKET_MAX)
        return 0;
    net_socket_t* socket = &pool->sockets[socket_id];
    if (socket->state == NET_SOCKET_FREE || socket->owner_endpoint != owner_endpoint)
        return 0;
    return socket;
}

void net_socket_pool_init(net_socket_pool_t* pool) {
    if (pool != 0)
        __builtin_memset(pool, 0, sizeof(*pool));
}

int32_t net_socket_open(net_socket_pool_t* pool, uint32_t owner_endpoint,
                        const net_socket_open_descriptor_v1_t* descriptor, void* tx_base,
                        void* rx_base, uint32_t* out_socket_id) {
    if (pool == 0 || descriptor == 0 || out_socket_id == 0 || owner_endpoint == 0u)
        return WASMOS_ERR_NET_INVALID;
    if (descriptor->version != NET_SOCKET_OPEN_DESCRIPTOR_VERSION ||
        descriptor->bytes != sizeof(*descriptor) ||
        (descriptor->family != NET_SOCKET_AF_INET && descriptor->family != NET_SOCKET_AF_INET6) ||
        (descriptor->type != NET_SOCKET_STREAM && descriptor->type != NET_SOCKET_DGRAM) ||
        descriptor->tx_buffer_id == 0u || descriptor->tx_borrow_id == 0u ||
        descriptor->rx_buffer_id == 0u || descriptor->rx_borrow_id == 0u || tx_base == 0 ||
        rx_base == 0) {
        return WASMOS_ERR_NET_INVALID;
    }
    for (uint32_t id = 0; id < NET_SOCKET_MAX; ++id) {
        net_socket_t* socket = &pool->sockets[id];
        if (socket->state != NET_SOCKET_FREE)
            continue;
        if (wasmos_ringbuf_attach(&socket->tx_ring, tx_base, descriptor->tx_bytes) != 0 ||
            wasmos_ringbuf_attach(&socket->rx_ring, rx_base, descriptor->rx_bytes) != 0) {
            return WASMOS_ERR_NET_INVALID;
        }
        socket->state = NET_SOCKET_OPEN;
        socket->owner_endpoint = owner_endpoint;
        socket->family = descriptor->family;
        socket->type = descriptor->type;
        socket->stack_id = descriptor->stack_id;
        socket->tx_buffer_id = descriptor->tx_buffer_id;
        socket->tx_borrow_id = descriptor->tx_borrow_id;
        socket->rx_buffer_id = descriptor->rx_buffer_id;
        socket->rx_borrow_id = descriptor->rx_borrow_id;
        /* Copy the SNI/verification hostname (bounded, NUL terminated) so the
         * pcb-open path can hand it to mbedTLS. */
        socket->sni_len = 0u;
        if (descriptor->sni_len > 0u) {
            uint16_t n = descriptor->sni_len;
            if (n > NET_SOCKET_SNI_MAX - 1u)
                n = NET_SOCKET_SNI_MAX - 1u;
            for (uint16_t i = 0u; i < n; ++i)
                socket->sni[i] = descriptor->sni[i];
            socket->sni[n] = 0u;
            socket->sni_len = n;
        }
        *out_socket_id = id;
        return WASMOS_ERR_NONE;
    }
    return WASMOS_ERR_NET_NO_MEM;
}

int32_t net_socket_bind(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id,
                        uint16_t port, uint32_t addr_v4) {
    net_socket_t* socket = socket_owned(pool, owner_endpoint, socket_id);
    if (socket == 0 || socket->state != NET_SOCKET_OPEN)
        return WASMOS_ERR_NET_INVALID;
    socket->local_port = port;
    socket->local_addr_v4 = addr_v4;
    socket->state = NET_SOCKET_BOUND;
    return WASMOS_ERR_NONE;
}

int32_t net_socket_connect(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id,
                           uint16_t port, uint32_t addr_v4) {
    net_socket_t* socket = socket_owned(pool, owner_endpoint, socket_id);
    if (socket == 0 || (socket->state != NET_SOCKET_OPEN && socket->state != NET_SOCKET_BOUND))
        return WASMOS_ERR_NET_INVALID;
    socket->remote_port = port;
    socket->remote_addr_v4 = addr_v4;
    /* A datagram socket is connected immediately; a stream socket enters
     * CONNECTING until its TCP handshake completes in a lwIP callback. */
    socket->state =
        (socket->type == NET_SOCKET_STREAM) ? NET_SOCKET_CONNECTING : NET_SOCKET_CONNECTED;
    return WASMOS_ERR_NONE;
}

int32_t net_socket_listen(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id) {
    net_socket_t* socket = socket_owned(pool, owner_endpoint, socket_id);
    if (socket == 0 || socket->type != NET_SOCKET_STREAM || socket->state != NET_SOCKET_BOUND)
        return WASMOS_ERR_NET_INVALID;
    socket->state = NET_SOCKET_LISTENING;
    return WASMOS_ERR_NONE;
}

int32_t net_socket_close(net_socket_pool_t* pool, uint32_t owner_endpoint, uint32_t socket_id) {
    net_socket_t* socket = socket_owned(pool, owner_endpoint, socket_id);
    if (socket == 0)
        return WASMOS_ERR_NET_DENIED;
    wasmos_ringbuf_set_flags(&socket->tx_ring, WASMOS_RINGBUF_FLAG_PEER_CLOSED);
    wasmos_ringbuf_set_flags(&socket->rx_ring, WASMOS_RINGBUF_FLAG_PEER_CLOSED);
    __builtin_memset(socket, 0, sizeof(*socket));
    return WASMOS_ERR_NONE;
}
