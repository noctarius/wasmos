#include "socket.h"

#include <stdint.h>
#include <string.h>

#include "test_shuffle.h"

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
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) == WASMOS_ERR_NONE);
    CHECK(pool.sockets[id].state == NET_SOCKET_OPEN);
    CHECK(pool.sockets[id].tx_ring.hdr == tx.hdr);
    CHECK(pool.sockets[id].rx_ring.hdr == rx.hdr);
    CHECK(net_socket_bind(&pool, 102u, id, 1234u, 0u) == WASMOS_ERR_NET_INVALID);
    CHECK(net_socket_bind(&pool, 101u, id, 1234u, 0u) == WASMOS_ERR_NONE);
    CHECK(net_socket_listen(&pool, 101u, id) == WASMOS_ERR_NONE);
    CHECK(net_socket_connect(&pool, 101u, id, 80u, 0x01020304u) == WASMOS_ERR_NET_INVALID);
    CHECK(net_socket_close(&pool, 102u, id) == WASMOS_ERR_NET_DENIED);
    CHECK(net_socket_close(&pool, 101u, id) == WASMOS_ERR_NONE);
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
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) ==
          WASMOS_ERR_NET_INVALID);
    wasmos_ringbuf_t tx, rx;
    CHECK(wasmos_ringbuf_init(&tx, tx_region, sizeof(tx_region), CAPACITY) == 0);
    CHECK(wasmos_ringbuf_init(&rx, rx_region, sizeof(rx_region), CAPACITY) == 0);
    descriptor.version++;
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) ==
          WASMOS_ERR_NET_INVALID);
    descriptor = valid_descriptor();
    descriptor.rx_borrow_id = 0u;
    CHECK(net_socket_open(&pool, 101u, &descriptor, tx_region, rx_region, &id) ==
          WASMOS_ERR_NET_INVALID);
    return 0;
}

/* A stream connect enters CONNECTING (the TCP handshake is still in flight),
 * while a datagram connect is immediately CONNECTED. */
static int test_connect_state_depends_on_type(void) {
    net_socket_pool_t pool;
    net_socket_pool_init(&pool);
    /* The two regions are file statics shared with the other cases, which run in
     * a shuffled order and leave behind PEER_CLOSED flags or a zeroed header.
     * Only the stamped header matters here, so the handles are discarded --
     * net_socket_open attaches its own. */
    CHECK(wasmos_ringbuf_init(&(wasmos_ringbuf_t){0}, tx_region, sizeof(tx_region), CAPACITY) == 0);
    CHECK(wasmos_ringbuf_init(&(wasmos_ringbuf_t){0}, rx_region, sizeof(rx_region), CAPACITY) == 0);

    net_socket_open_descriptor_v1_t stream = valid_descriptor();
    uint32_t sid = 0;
    CHECK(net_socket_open(&pool, 200u, &stream, tx_region, rx_region, &sid) == WASMOS_ERR_NONE);
    CHECK(net_socket_bind(&pool, 200u, sid, 1000u, 0u) == WASMOS_ERR_NONE);
    CHECK(net_socket_connect(&pool, 200u, sid, 80u, 0x01020304u) == WASMOS_ERR_NONE);
    CHECK(pool.sockets[sid].state == NET_SOCKET_CONNECTING);
    CHECK(pool.sockets[sid].remote_port == 80u);
    CHECK(pool.sockets[sid].remote_addr_v4 == 0x01020304u);
    CHECK(net_socket_close(&pool, 200u, sid) == WASMOS_ERR_NONE);

    net_socket_open_descriptor_v1_t dgram = valid_descriptor();
    dgram.type = NET_SOCKET_DGRAM;
    uint32_t did = 0;
    CHECK(net_socket_open(&pool, 200u, &dgram, tx_region, rx_region, &did) == WASMOS_ERR_NONE);
    CHECK(net_socket_connect(&pool, 200u, did, 53u, 0x08080808u) == WASMOS_ERR_NONE);
    CHECK(pool.sockets[did].state == NET_SOCKET_CONNECTED);
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_open_lifecycle_and_owner_check),
        WASMOS_TEST_CASE(test_rejects_bad_descriptor_or_ring),
        WASMOS_TEST_CASE(test_connect_state_depends_on_type),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
