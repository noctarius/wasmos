#include "socket.h"

#include <stdint.h>
#include <string.h>

#define CAPACITY 64u

static uint8_t tx_region[WASMOS_RINGBUF_HDR_BYTES + CAPACITY] __attribute__((aligned(64)));
static uint8_t rx_region[WASMOS_RINGBUF_HDR_BYTES + CAPACITY] __attribute__((aligned(64)));

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

static net_socket_open_descriptor_v1_t valid_descriptor(void) {
    net_socket_open_descriptor_v1_t descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.version = NET_SOCKET_OPEN_DESCRIPTOR_VERSION;
    descriptor.bytes = sizeof(descriptor);
    descriptor.family = NET_SOCKET_AF_INET;
    descriptor.type = NET_SOCKET_STREAM;
    descriptor.tx_buffer_id = 11u;
    descriptor.tx_borrow_id = 12u;
    descriptor.tx_bytes = sizeof(tx_region);
    descriptor.rx_buffer_id = 13u;
    descriptor.rx_borrow_id = 14u;
    descriptor.rx_bytes = sizeof(rx_region);
    return descriptor;
}

static int test_open_lifecycle_and_owner_check(void) {
    net_socket_pool_t pool;
    net_socket_pool_init(&pool);
    wasmos_ringbuf_t tx, rx;
    CHECK(wasmos_ringbuf_init(&tx, tx_region, sizeof(tx_region), CAPACITY) == 0);
    CHECK(wasmos_ringbuf_init(&rx, rx_region, sizeof(rx_region), CAPACITY) == 0);
    net_socket_open_descriptor_v1_t descriptor = valid_descriptor();
    uint32_t id = 0;
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) == NET_STATUS_OK);
    CHECK(pool.sockets[id].state == NET_SOCKET_OPEN);
    CHECK(pool.sockets[id].tx_ring.hdr == tx.hdr);
    CHECK(pool.sockets[id].rx_ring.hdr == rx.hdr);
    CHECK(net_socket_bind(&pool, 102u, id, 1234u, 0u) == NET_STATUS_INVALID);
    CHECK(net_socket_bind(&pool, 101u, id, 1234u, 0u) == NET_STATUS_OK);
    CHECK(net_socket_listen(&pool, 101u, id) == NET_STATUS_OK);
    CHECK(net_socket_connect(&pool, 101u, id, 80u, 0x01020304u) == NET_STATUS_INVALID);
    CHECK(net_socket_close(&pool, 102u, id) == NET_STATUS_DENIED);
    CHECK(net_socket_close(&pool, 101u, id) == NET_STATUS_OK);
    CHECK(pool.sockets[id].state == NET_SOCKET_FREE);
    CHECK((wasmos_ringbuf_flags(&tx) & WASMOS_RINGBUF_FLAG_PEER_CLOSED) != 0u);
    CHECK((wasmos_ringbuf_flags(&rx) & WASMOS_RINGBUF_FLAG_PEER_CLOSED) != 0u);
    return 0;
}

static int test_rejects_bad_descriptor_or_ring(void) {
    net_socket_pool_t pool;
    net_socket_pool_init(&pool);
    net_socket_open_descriptor_v1_t descriptor = valid_descriptor();
    uint32_t id = 0;
    memset(tx_region, 0, sizeof(tx_region));
    memset(rx_region, 0, sizeof(rx_region));
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) == NET_STATUS_INVALID);
    wasmos_ringbuf_t tx, rx;
    CHECK(wasmos_ringbuf_init(&tx, tx_region, sizeof(tx_region), CAPACITY) == 0);
    CHECK(wasmos_ringbuf_init(&rx, rx_region, sizeof(rx_region), CAPACITY) == 0);
    descriptor.version++;
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) == NET_STATUS_INVALID);
    descriptor = valid_descriptor();
    descriptor.rx_borrow_id = 0u;
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) == NET_STATUS_INVALID);
    return 0;
}

int main(void) {
    int rc = test_open_lifecycle_and_owner_check();
    if (rc != 0)
        return rc;
    return test_rejects_bad_descriptor_or_ring();
}
