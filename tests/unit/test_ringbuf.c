/* Host unit test for the general SPSC ring buffer core (wasmos/ringbuf.h).
 * Exercises header init/attach validation, byte-stream write/read with
 * wraparound and flow control, occupancy queries across the 2^32 index wrap,
 * length-prefixed datagram framing, doorbell empty->non-empty edge signalling,
 * and the flags word. No IPC/QEMU — producer and consumer are the same process
 * operating on one shared region, which is exactly the contract the core
 * assumes (two parties, one shared buffer). */

#include "wasmos/ringbuf.h"

#include <stdint.h>
#include <string.h>

#define CAP 64u  /* small power-of-two capacity to force wraparound quickly */

/* Backing store for one ring: header + data, generously aligned. */
static uint8_t g_region[WASMOS_RINGBUF_HDR_BYTES + CAP] __attribute__((aligned(64)));

static int g_notify_calls;
static void count_notify(void *user) {
    (void)user;
    g_notify_calls++;
}

static int
test_bytes_for_and_layout(void) {
    if (sizeof(wasmos_ringbuf_hdr_t) != 64u) return __LINE__;
    if (wasmos_ringbuf_bytes_for(CAP) != WASMOS_RINGBUF_HDR_BYTES + CAP) return __LINE__;
    if (wasmos_ringbuf_is_pow2(0u)) return __LINE__;
    if (wasmos_ringbuf_is_pow2(3u)) return __LINE__;
    if (!wasmos_ringbuf_is_pow2(1u) || !wasmos_ringbuf_is_pow2(64u)) return __LINE__;

    wasmos_ringbuf_t rb;
    if (wasmos_ringbuf_init(&rb, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (rb.capacity != CAP) return __LINE__;
    if (rb.data != g_region + WASMOS_RINGBUF_HDR_BYTES) return __LINE__;
    if (rb.hdr->magic != WASMOS_RINGBUF_MAGIC) return __LINE__;
    if (rb.hdr->version != WASMOS_RINGBUF_VERSION) return __LINE__;
    if (rb.hdr->hdr_bytes != WASMOS_RINGBUF_HDR_BYTES) return __LINE__;
    if (rb.hdr->capacity != CAP) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&rb)) return __LINE__;
    if (wasmos_ringbuf_is_full(&rb)) return __LINE__;
    if (wasmos_ringbuf_used(&rb) != 0u) return __LINE__;
    if (wasmos_ringbuf_free(&rb) != CAP) return __LINE__;

    /* write/read words must sit far enough apart to not share a word. */
    if ((uint8_t *)&rb.hdr->read - (uint8_t *)&rb.hdr->write < 16) return __LINE__;
    return 0;
}

static int
test_reject_bad_params(void) {
    wasmos_ringbuf_t rb;
    /* non-power-of-two capacity. */
    if (wasmos_ringbuf_init(&rb, g_region, sizeof(g_region), 3u) == 0) return __LINE__;
    /* region too small for capacity. */
    if (wasmos_ringbuf_init(&rb, g_region, WASMOS_RINGBUF_HDR_BYTES + 4u, CAP) == 0)
        return __LINE__;
    /* attach must reject a region whose header is not a valid ring. */
    uint8_t junk[WASMOS_RINGBUF_HDR_BYTES + CAP];
    memset(junk, 0, sizeof(junk));
    if (wasmos_ringbuf_attach(&rb, junk, sizeof(junk)) == 0) return __LINE__;
    return 0;
}

static int
test_attach_matches_init(void) {
    wasmos_ringbuf_t prod;
    if (wasmos_ringbuf_init(&prod, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    /* A second party attaches to the same region and sees the same geometry. */
    wasmos_ringbuf_t cons;
    if (wasmos_ringbuf_attach(&cons, g_region, sizeof(g_region)) != 0) return __LINE__;
    if (cons.capacity != CAP) return __LINE__;
    if (cons.data != prod.data) return __LINE__;
    if (cons.hdr != prod.hdr) return __LINE__;
    return 0;
}

static int
test_write_read_and_flow_control(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    uint8_t src[100];
    for (int i = 0; i < 100; ++i) src[i] = (uint8_t)(i + 1);

    /* Ring holds CAP bytes; writing 100 must be truncated to CAP (flow control),
     * never overrun the consumer. */
    uint32_t n = wasmos_ringbuf_write(&p, src, 100u);
    if (n != CAP) return __LINE__;
    if (!wasmos_ringbuf_is_full(&c)) return __LINE__;
    if (wasmos_ringbuf_free(&c) != 0u) return __LINE__;
    /* A write into a full ring writes nothing. */
    if (wasmos_ringbuf_write(&p, src, 1u) != 0u) return __LINE__;

    /* Consumer drains 20 bytes and they match. */
    uint8_t dst[100];
    memset(dst, 0, sizeof(dst));
    uint32_t got = wasmos_ringbuf_read(&c, dst, 20u);
    if (got != 20u) return __LINE__;
    if (memcmp(dst, src, 20u) != 0) return __LINE__;
    if (wasmos_ringbuf_used(&c) != CAP - 20u) return __LINE__;

    /* Now 20 bytes are free again; the producer can add exactly 20. */
    if (wasmos_ringbuf_free(&p) != 20u) return __LINE__;
    n = wasmos_ringbuf_write(&p, src, 30u);
    if (n != 20u) return __LINE__;
    if (!wasmos_ringbuf_is_full(&p)) return __LINE__;
    return 0;
}

static int
test_wraparound_payload(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    /* Advance both indices to just before a data-region wrap so the next write
     * straddles the physical end of the buffer. */
    uint8_t pad[CAP];
    for (uint32_t i = 0; i < CAP; ++i) pad[i] = (uint8_t)i;
    /* Fill, drain most, leaving read/write near the wrap boundary. */
    if (wasmos_ringbuf_write(&p, pad, CAP) != CAP) return __LINE__;
    uint8_t sink[CAP];
    if (wasmos_ringbuf_read(&c, sink, CAP - 8u) != CAP - 8u) return __LINE__;

    /* write index is at CAP, read at CAP-8: writing 40 bytes wraps across 0. */
    uint8_t msg[40];
    for (int i = 0; i < 40; ++i) msg[i] = (uint8_t)(0x80 + i);
    if (wasmos_ringbuf_write(&p, msg, 40u) != 40u) return __LINE__;

    /* Drain everything and confirm the straddling payload is intact and in
     * order: the 8 leftover pad bytes, then the 40 msg bytes. */
    uint8_t out[64];
    memset(out, 0, sizeof(out));
    uint32_t total = wasmos_ringbuf_read(&c, out, sizeof(out));
    if (total != 48u) return __LINE__;
    for (uint32_t i = 0; i < 8u; ++i) {
        if (out[i] != (uint8_t)((CAP - 8u) + i)) return __LINE__;
    }
    for (int i = 0; i < 40; ++i) {
        if (out[8 + i] != (uint8_t)(0x80 + i)) return __LINE__;
    }
    return 0;
}

static int
test_index_counter_wrap(void) {
    /* The free-running indices must behave correctly across the 2^32 wrap. Seed
     * write/read near UINT32_MAX and confirm occupancy math and a wrapping write
     * still work. */
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    p.hdr->write = 0xFFFFFFF0u;
    p.hdr->read  = 0xFFFFFFF0u;  /* empty at a near-max index */
    if (!wasmos_ringbuf_is_empty(&p)) return __LINE__;
    if (wasmos_ringbuf_free(&p) != CAP) return __LINE__;

    uint8_t src[32];
    for (int i = 0; i < 32; ++i) src[i] = (uint8_t)(i + 1);
    /* This write pushes the write counter across 0xFFFFFFFF. */
    if (wasmos_ringbuf_write(&p, src, 32u) != 32u) return __LINE__;
    if (wasmos_ringbuf_used(&c) != 32u) return __LINE__; /* wrap-safe subtraction */

    uint8_t dst[32];
    memset(dst, 0, sizeof(dst));
    if (wasmos_ringbuf_read(&c, dst, 32u) != 32u) return __LINE__;
    if (memcmp(dst, src, 32u) != 0) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&c)) return __LINE__;
    return 0;
}

static int
test_datagram_framing(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    uint8_t a[10], b[5];
    for (int i = 0; i < 10; ++i) a[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 5; ++i) b[i] = (uint8_t)(0xB0 + i);

    /* Two records back to back: consume 4+10 then 4+5 = 23 bytes total. */
    if (wasmos_ringbuf_write_record(&p, a, 10u) != 10) return __LINE__;
    if (wasmos_ringbuf_write_record(&p, b, 5u) != 5) return __LINE__;
    if (wasmos_ringbuf_used(&c) != 23u) return __LINE__;

    /* Peek the first record's length without consuming. */
    uint32_t plen = 0;
    if (!wasmos_ringbuf_peek_record_len(&c, &plen) || plen != 10u) return __LINE__;
    if (wasmos_ringbuf_used(&c) != 23u) return __LINE__; /* peek did not consume */

    uint8_t out[16];
    uint32_t rlen = 0;
    memset(out, 0, sizeof(out));
    if (wasmos_ringbuf_read_record(&c, out, sizeof(out), &rlen) != 10) return __LINE__;
    if (rlen != 10u || memcmp(out, a, 10u) != 0) return __LINE__;

    /* Record boundaries are preserved: second read yields exactly b, not a
     * merged byte stream. */
    memset(out, 0, sizeof(out));
    if (wasmos_ringbuf_read_record(&c, out, sizeof(out), &rlen) != 5) return __LINE__;
    if (rlen != 5u || memcmp(out, b, 5u) != 0) return __LINE__;

    /* Empty now: read_record reports "no complete record" (-1). */
    if (wasmos_ringbuf_read_record(&c, out, sizeof(out), &rlen) != -1) return __LINE__;
    return 0;
}

static int
test_datagram_undersized_dst_and_toobig(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    uint8_t rec[20];
    for (int i = 0; i < 20; ++i) rec[i] = (uint8_t)(i + 1);
    if (wasmos_ringbuf_write_record(&p, rec, 20u) != 20) return __LINE__;

    /* Destination too small: -2, nothing consumed, length reported for retry. */
    uint8_t small[8];
    uint32_t rlen = 0;
    if (wasmos_ringbuf_read_record(&c, small, sizeof(small), &rlen) != -2) return __LINE__;
    if (rlen != 20u) return __LINE__;
    if (wasmos_ringbuf_used(&c) != 24u) return __LINE__; /* still queued */

    /* Retry with a big-enough buffer succeeds. */
    uint8_t big[20];
    memset(big, 0, sizeof(big));
    if (wasmos_ringbuf_read_record(&c, big, sizeof(big), &rlen) != 20) return __LINE__;
    if (memcmp(big, rec, 20u) != 0) return __LINE__;

    /* A record that cannot ever fit the capacity is rejected at write time. */
    uint8_t huge[CAP];
    if (wasmos_ringbuf_write_record(&p, huge, CAP) != -1) return __LINE__;
    return 0;
}

static int
test_doorbell_edge(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;
    g_notify_calls = 0;
    wasmos_ringbuf_set_notify(&p, count_notify, 0);

    uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    /* First write into an empty ring fires the doorbell (empty->non-empty). */
    if (wasmos_ringbuf_write_signal(&p, src, 4u) != 4u) return __LINE__;
    if (g_notify_calls != 1) return __LINE__;
    /* Second write while non-empty must NOT fire (no edge). */
    if (wasmos_ringbuf_write_signal(&p, src, 4u) != 4u) return __LINE__;
    if (g_notify_calls != 1) return __LINE__;

    /* Drain fully, then the next write fires the edge again. */
    uint8_t dst[8];
    if (wasmos_ringbuf_read(&c, dst, 8u) != 8u) return __LINE__;
    if (wasmos_ringbuf_write_signal(&p, src, 2u) != 2u) return __LINE__;
    if (g_notify_calls != 2) return __LINE__;

    /* A signalled write that copies nothing (ring full) must not fire. */
    (void)wasmos_ringbuf_write(&p, src, CAP); /* fill */
    int before = g_notify_calls;
    if (wasmos_ringbuf_write_signal(&p, src, 1u) != 0u) return __LINE__;
    if (g_notify_calls != before) return __LINE__;

    /* Record-signal fires on the empty->non-empty edge too. */
    wasmos_ringbuf_t p2, c2;
    if (wasmos_ringbuf_init(&p2, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c2, g_region, sizeof(g_region)) != 0) return __LINE__;
    g_notify_calls = 0;
    wasmos_ringbuf_set_notify(&p2, count_notify, 0);
    if (wasmos_ringbuf_write_record_signal(&p2, src, 4u) != 4) return __LINE__;
    if (g_notify_calls != 1) return __LINE__;
    if (wasmos_ringbuf_write_record_signal(&p2, src, 4u) != 4) return __LINE__;
    if (g_notify_calls != 1) return __LINE__; /* no edge */
    return 0;
}

static int
test_flags(void) {
    wasmos_ringbuf_t rb;
    if (wasmos_ringbuf_init(&rb, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_flags(&rb) != 0u) return __LINE__;
    wasmos_ringbuf_set_flags(&rb, WASMOS_RINGBUF_FLAG_PEER_CLOSED);
    wasmos_ringbuf_set_flags(&rb, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);
    uint32_t f = wasmos_ringbuf_flags(&rb);
    if ((f & WASMOS_RINGBUF_FLAG_PEER_CLOSED) == 0u) return __LINE__;
    if ((f & WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED) == 0u) return __LINE__;
    if ((f & WASMOS_RINGBUF_FLAG_RESET) != 0u) return __LINE__;
    return 0;
}

int
main(void) {
    int rc;
    if ((rc = test_bytes_for_and_layout()) != 0) return rc;
    if ((rc = test_reject_bad_params()) != 0) return rc;
    if ((rc = test_attach_matches_init()) != 0) return rc;
    if ((rc = test_write_read_and_flow_control()) != 0) return rc;
    if ((rc = test_wraparound_payload()) != 0) return rc;
    if ((rc = test_index_counter_wrap()) != 0) return rc;
    if ((rc = test_datagram_framing()) != 0) return rc;
    if ((rc = test_datagram_undersized_dst_and_toobig()) != 0) return rc;
    if ((rc = test_doorbell_edge()) != 0) return rc;
    if ((rc = test_flags()) != 0) return rc;
    return 0;
}