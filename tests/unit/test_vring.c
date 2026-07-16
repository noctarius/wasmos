/* Host unit test for the transport-neutral vring core (wasmos/vring.h).
 * Exercises layout, descriptor alloc/free, publish/kick, used-ring consumption,
 * and consumer-side validation. No device or QEMU — the "device" side is
 * simulated in-process by reading the avail ring and writing the used ring. */

#include "wasmos/vring.h"

#include <stdint.h>
#include <string.h>

#define QNUM 8u
#define QALIGN 4096u

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
    /* desc/avail/used must all sit inside the region. */
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
    /* Free one and confirm we can allocate again. */
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

int main(void) {
    int rc;
    if ((rc = test_size_and_layout()) != 0)
        return rc;
    if ((rc = test_reject_bad_params()) != 0)
        return rc;
    if ((rc = test_publish_kick_consume()) != 0)
        return rc;
    if ((rc = test_free_list_exhaustion()) != 0)
        return rc;
    if ((rc = test_consumer_validation()) != 0)
        return rc;
    return 0;
}
