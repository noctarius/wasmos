/* Host unit test for the transport-neutral vring core (wasmos/vring.h).
 * Exercises layout, descriptor alloc/free, chained descriptors, publish/kick,
 * used-ring consumption, and consumer-side validation. No device or QEMU — the
 * "device" side is simulated in-process by reading the avail ring and writing
 * the used ring. */

#include "wasmos/vring.h"

#include <stdint.h>
#include <string.h>

#include "test_shuffle.h"

/* A case returns 0 when it passes and its failing __LINE__ as an opaque marker
 * otherwise; wasmos_test_run_all stops at the first non-zero and prints the
 * marker with the shuffled order that produced it. */

/* Queue geometry. QNUM is the descriptor count, which the split-ring layout
 * requires to be a power of two; 8 is small enough that the free list is
 * exhausted in a handful of allocations. QALIGN is the legacy virtio ring
 * alignment: the used ring starts at the next QALIGN boundary after the
 * descriptor table plus the avail ring, so the padding between them is what
 * vring_size accounts for. */
#define QNUM 8u
#define QALIGN 4096u

/* Doorbell stand-in for the backend's real notify (a PCI QUEUE_NOTIFY write or
 * an IPC notification): it only counts invocations. Nothing resets the counter,
 * so a case zeroes g_notify_calls itself after installing the callback. */
static int g_notify_calls;
static void count_notify(void* user) {
    (void)user;
    g_notify_calls++;
}

/* Backing store for one queue, sized generously and page-aligned. */
static uint8_t g_region[64 * 1024] __attribute__((aligned(4096)));

/* Simulate the device consuming one available buffer and completing it: pull
 * the head from the avail ring and append a used element reporting `wrote`
 * bytes. Mirrors what a real virtio device does to the shared rings. */
static void device_complete_one(vring_t* vq, uint16_t avail_slot, uint32_t wrote) {
    uint16_t head = vq->avail_ring[avail_slot % vq->num];
    uint16_t uidx = *vq->used_idx;
    vq->used_ring[uidx % vq->num].id = head;
    vq->used_ring[uidx % vq->num].len = wrote;
    vring_mb();
    *vq->used_idx = (uint16_t)(uidx + 1);
}

static int test_size_and_layout(void) {
    /* vring_size is align(desc+avail, align) + used. */
    uint64_t desc = (uint64_t)QNUM * 16u;                /* 128 */
    uint64_t avail = 2u + 2u + (uint64_t)QNUM * 2u + 2u; /* 22  */
    uint64_t used = 2u + 2u + (uint64_t)QNUM * 8u + 2u;  /* 70  */
    uint64_t expect = vring_align_up(desc + avail, QALIGN) + used;
    if (vring_size(QNUM, QALIGN) != expect)
        return __LINE__;

    vring_t vq;
    if (vring_layout(&vq, g_region, 0x1000000ULL, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;
    if (vq.num != QNUM)
        return __LINE__;
    if (vq.num_free != QNUM)
        return __LINE__;
    if (vq.last_used_idx != 0)
        return __LINE__;
    if (*vq.avail_idx != 0 || *vq.used_idx != 0)
        return __LINE__;
    /* The layout must fit the region: the descriptor table is its lowest field
     * and the used ring its highest, so bounding those two bounds all of it. */
    if ((uint8_t*)vq.desc < g_region)
        return __LINE__;
    if ((uint8_t*)vq.used_ring + QNUM * 8u > g_region + sizeof(g_region))
        return __LINE__;
    /* used ring must be align-aligned relative to the region base. */
    if ((((uint8_t*)vq.used_flags - g_region) % QALIGN) != 0)
        return __LINE__;
    return 0;
}

static int test_reject_bad_params(void) {
    vring_t vq;
    /* num not a power of two. */
    if (vring_layout(&vq, g_region, 0, sizeof(g_region), 3, QALIGN) == 0)
        return __LINE__;
    /* region too small. */
    if (vring_layout(&vq, g_region, 0, 8, QNUM, QALIGN) == 0)
        return __LINE__;
    return 0;
}

static int test_publish_kick_consume(void) {
    vring_t vq;
    if (vring_layout(&vq, g_region, 0x2000000ULL, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;
    g_notify_calls = 0;
    vring_set_notify(&vq, count_notify, 0);

    /* Publish two device-writable RX buffers. */
    int32_t d0 = vring_alloc_desc(&vq, 0x2000000ULL + 0x100, 2048, VRING_DESC_F_WRITE);
    int32_t d1 = vring_alloc_desc(&vq, 0x2000000ULL + 0x900, 2048, VRING_DESC_F_WRITE);
    if (d0 < 0 || d1 < 0)
        return __LINE__;
    if (vq.num_free != QNUM - 2)
        return __LINE__;
    if (vq.desc[d0].addr != 0x2000000ULL + 0x100)
        return __LINE__;
    if (vq.desc[d0].len != 2048)
        return __LINE__;
    if ((vq.desc[d0].flags & VRING_DESC_F_WRITE) == 0)
        return __LINE__;

    vring_publish(&vq, (uint16_t)d0);
    vring_publish(&vq, (uint16_t)d1);
    if (*vq.avail_idx != 2)
        return __LINE__;
    if (vq.avail_ring[0] != (uint16_t)d0 || vq.avail_ring[1] != (uint16_t)d1)
        return __LINE__;

    if (vring_has_used(&vq))
        return __LINE__; /* nothing completed yet */
    vring_kick(&vq);
    if (g_notify_calls != 1)
        return __LINE__;

    /* Device completes the first buffer with 1500 bytes. */
    device_complete_one(&vq, 0, 1500);
    if (!vring_has_used(&vq))
        return __LINE__;
    uint32_t len = 0;
    int32_t got = vring_get_used(&vq, &len);
    if (got != d0)
        return __LINE__;
    if (len != 1500)
        return __LINE__;
    if (vring_has_used(&vq))
        return __LINE__; /* only one was completed */

    /* Recycle the consumed descriptor. */
    uint16_t before = vq.num_free;
    vring_free_desc(&vq, (uint16_t)got);
    if (vq.num_free != before + 1)
        return __LINE__;

    /* Device completes the second buffer. */
    device_complete_one(&vq, 1, 64);
    got = vring_get_used(&vq, &len);
    if (got != d1 || len != 64)
        return __LINE__;
    return 0;
}

static int test_free_list_exhaustion(void) {
    vring_t vq;
    if (vring_layout(&vq, g_region, 0, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;
    for (uint16_t i = 0; i < QNUM; ++i) {
        if (vring_alloc_desc(&vq, 0x1000ULL * (i + 1), 16, 0) < 0)
            return __LINE__;
    }
    if (vq.num_free != 0)
        return __LINE__;
    if (vring_alloc_desc(&vq, 0xdead, 16, 0) != -1)
        return __LINE__; /* full */
    /* Free one and confirm the slot becomes allocatable again. */
    vring_free_desc(&vq, 3);
    if (vring_alloc_desc(&vq, 0xbeef, 16, 0) < 0)
        return __LINE__;
    return 0;
}

static int test_consumer_validation(void) {
    vring_t vq;
    if (vring_layout(&vq, g_region, 0, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;
    /* Inject a malicious/corrupt used element with an out-of-range id. */
    vq.used_ring[0].id = QNUM + 5; /* invalid: >= num */
    vq.used_ring[0].len = 0x7fffffff;
    vring_mb();
    *vq.used_idx = 1;
    uint32_t len = 12345;
    int32_t got = vring_get_used(&vq, &len);
    if (got != -1)
        return __LINE__; /* rejected, not trusted */
    if (vq.last_used_idx != 1)
        return __LINE__; /* but still advanced past it */
    return 0;
}

/* A chain is what a request spanning several buffers looks like on the ring: a
 * virtio-blk request is a header the device reads, a data buffer, and a status
 * byte the device writes. The three properties that make it a chain are the
 * NEXT flag on every entry but the last, links that walk head to tail, and
 * per-entry direction flags. */
static int test_alloc_chain_links_and_flags(void) {
    vring_t vq;
    if (vring_layout(&vq, g_region, 0x3000000ULL, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;

    const vring_buf_t bufs[3] = {
        {0x3000000ULL + 0x00, 16, 0},                   /* header, device reads */
        {0x3000000ULL + 0x40, 512, VRING_DESC_F_WRITE}, /* data, device writes */
        {0x3000000ULL + 0x300, 1, VRING_DESC_F_WRITE},  /* status, device writes */
    };
    int32_t head = vring_alloc_chain(&vq, bufs, 3);
    if (head < 0)
        return __LINE__;
    if (vq.num_free != QNUM - 3)
        return __LINE__;

    uint16_t idx = (uint16_t)head;
    for (uint16_t i = 0; i < 3; ++i) {
        if (idx >= QNUM)
            return __LINE__;
        if (vq.desc[idx].addr != bufs[i].addr || vq.desc[idx].len != bufs[i].len)
            return __LINE__;
        if ((vq.desc[idx].flags & VRING_DESC_F_WRITE) != bufs[i].flags)
            return __LINE__;
        /* Every entry but the last continues the chain. */
        uint16_t want_next = (i + 1u < 3u) ? VRING_DESC_F_NEXT : 0u;
        if ((vq.desc[idx].flags & VRING_DESC_F_NEXT) != want_next)
            return __LINE__;
        idx = vq.desc[idx].next;
    }
    return 0;
}

/* Freeing a chain must return EVERY descriptor, not just the head: a leak here
 * shrinks the queue by two descriptors per request until it stops accepting
 * any, which reads as a device that stopped responding. */
static int test_free_chain_returns_every_descriptor(void) {
    vring_t vq;
    if (vring_layout(&vq, g_region, 0, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;
    const vring_buf_t bufs[3] = {
        {0x100, 16, 0}, {0x200, 512, VRING_DESC_F_WRITE}, {0x300, 1, VRING_DESC_F_WRITE}};

    for (int round = 0; round < 4; ++round) {
        int32_t head = vring_alloc_chain(&vq, bufs, 3);
        if (head < 0)
            return __LINE__;
        if (vq.num_free != QNUM - 3)
            return __LINE__;
        vring_free_chain(&vq, (uint16_t)head);
        /* Full capacity again, round after round — so the free list is intact
         * and not merely long enough to survive one pass. */
        if (vq.num_free != QNUM)
            return __LINE__;
    }
    return 0;
}

static int test_alloc_chain_rejects_bad_counts(void) {
    vring_t vq;
    if (vring_layout(&vq, g_region, 0, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;
    const vring_buf_t bufs[VRING_MAX_CHAIN + 1] = {{0x100, 16, 0}};

    if (vring_alloc_chain(&vq, bufs, 0) != -1)
        return __LINE__; /* empty chain */
    if (vring_alloc_chain(&vq, bufs, VRING_MAX_CHAIN + 1) != -1)
        return __LINE__; /* longer than the walk bound */
    if (vring_alloc_chain(&vq, 0, 3) != -1)
        return __LINE__; /* no buffers */
    if (vq.num_free != QNUM)
        return __LINE__; /* a refusal consumes nothing */

    /* A chain longer than the free list is backpressure, and must also consume
     * nothing — a partial allocation would strand the descriptors it took. */
    if (vring_alloc_chain(&vq, bufs, 6) < 0)
        return __LINE__;
    if (vq.num_free != QNUM - 6)
        return __LINE__;
    if (vring_alloc_chain(&vq, bufs, 3) != -1)
        return __LINE__;
    if (vq.num_free != QNUM - 6)
        return __LINE__;
    return 0;
}

/* The full driver-side cycle for a chained request: publish, the device reports
 * the HEAD on the used ring, and the whole chain goes back to the free list. */
static int test_chain_round_trip(void) {
    vring_t vq;
    if (vring_layout(&vq, g_region, 0, sizeof(g_region), QNUM, QALIGN) != 0)
        return __LINE__;
    const vring_buf_t bufs[3] = {
        {0x100, 16, 0}, {0x200, 512, VRING_DESC_F_WRITE}, {0x300, 1, VRING_DESC_F_WRITE}};
    int32_t head = vring_alloc_chain(&vq, bufs, 3);
    if (head < 0)
        return __LINE__;

    vring_publish(&vq, (uint16_t)head);
    if (*vq.avail_idx != 1 || vq.avail_ring[0] != (uint16_t)head)
        return __LINE__;

    /* A device reports the chain by its head descriptor, never its tail. */
    device_complete_one(&vq, 0, 513);
    uint32_t len = 0;
    int32_t got = vring_get_used(&vq, &len);
    if (got != head || len != 513)
        return __LINE__;

    vring_free_chain(&vq, (uint16_t)got);
    if (vq.num_free != QNUM)
        return __LINE__;
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_size_and_layout),
        WASMOS_TEST_CASE(test_reject_bad_params),
        WASMOS_TEST_CASE(test_publish_kick_consume),
        WASMOS_TEST_CASE(test_free_list_exhaustion),
        WASMOS_TEST_CASE(test_consumer_validation),
        WASMOS_TEST_CASE(test_alloc_chain_links_and_flags),
        WASMOS_TEST_CASE(test_free_chain_returns_every_descriptor),
        WASMOS_TEST_CASE(test_alloc_chain_rejects_bad_counts),
        WASMOS_TEST_CASE(test_chain_round_trip),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
