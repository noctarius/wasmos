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
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>

/* Counted assertion for the high-volume tests (sweep / exhaustive / fuzz):
 * bumps a global tally so the suite can report how many checks it ran, and
 * bails to the caller's __LINE__ on failure like the rest of the file. */
static long g_checks = 0;
#define CK(cond) do { g_checks++; if (!(cond)) return __LINE__; } while (0)

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

static int
test_zero_length_ops(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;
    g_notify_calls = 0;
    wasmos_ringbuf_set_notify(&p, count_notify, 0);

    uint8_t b[4] = {1, 2, 3, 4};
    /* Zero-length write is a no-op and never fires the doorbell. */
    if (wasmos_ringbuf_write(&p, b, 0u) != 0u) return __LINE__;
    if (wasmos_ringbuf_write_signal(&p, b, 0u) != 0u) return __LINE__;
    if (g_notify_calls != 0) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&p)) return __LINE__;

    /* Reads/peeks/skips on an empty ring return 0, never underflow. */
    uint8_t o[4];
    if (wasmos_ringbuf_read(&c, o, 4u) != 0u) return __LINE__;
    if (wasmos_ringbuf_peek(&c, o, 4u) != 0u) return __LINE__;
    if (wasmos_ringbuf_skip(&c, 4u) != 0u) return __LINE__;
    if (wasmos_ringbuf_read(&c, o, 0u) != 0u) return __LINE__;
    return 0;
}

static int
test_skip_and_short_read(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    uint8_t src[30];
    for (int i = 0; i < 30; ++i) src[i] = (uint8_t)(i + 1);
    if (wasmos_ringbuf_write(&p, src, 30u) != 30u) return __LINE__;

    /* skip fewer than queued advances exactly that many. */
    if (wasmos_ringbuf_skip(&c, 10u) != 10u) return __LINE__;
    if (wasmos_ringbuf_used(&c) != 20u) return __LINE__;
    /* skip more than remaining is clamped to what is queued. */
    if (wasmos_ringbuf_skip(&c, 100u) != 20u) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&c)) return __LINE__;
    /* short read on an empty ring returns 0. */
    uint8_t o[10];
    if (wasmos_ringbuf_read(&c, o, 10u) != 0u) return __LINE__;
    return 0;
}

static int
test_record_size_boundary(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    uint8_t big[CAP];
    for (uint32_t i = 0; i < CAP; ++i) big[i] = (uint8_t)i;

    /* The largest record that can ever fit has payload capacity-4 (its 4-byte
     * prefix uses the rest). It must succeed and fill the ring exactly. */
    if (wasmos_ringbuf_write_record(&p, big, CAP - 4u) != (int32_t)(CAP - 4u)) return __LINE__;
    if (!wasmos_ringbuf_is_full(&p)) return __LINE__;
    uint8_t sink[CAP];
    uint32_t rl = 0;
    if (wasmos_ringbuf_read_record(&c, sink, sizeof(sink), &rl) != (int32_t)(CAP - 4u)) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&c)) return __LINE__;

    /* One byte past the max never fits: rejected at write, nothing queued. */
    if (wasmos_ringbuf_write_record(&p, big, CAP - 3u) != -1) return __LINE__;
    if (wasmos_ringbuf_write_record(&p, big, CAP) != -1) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&p)) return __LINE__;
    return 0;
}

static int
test_empty_datagram(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    /* A zero-length datagram is a real record: just its 4-byte prefix. */
    if (wasmos_ringbuf_write_record(&p, 0, 0u) != 0) return __LINE__;
    if (wasmos_ringbuf_used(&c) != 4u) return __LINE__;
    uint8_t o[4];
    uint32_t rl = 99u;
    if (wasmos_ringbuf_read_record(&c, o, sizeof(o), &rl) != 0) return __LINE__; /* 0 payload */
    if (rl != 0u) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&c)) return __LINE__;
    return 0;
}

static int
test_record_wraparound(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    /* Push indices to 60 so the next record's payload straddles the physical
     * end of a 64-byte data region (prefix at 60..63, payload wraps to 0). */
    uint8_t pad[CAP];
    for (uint32_t i = 0; i < CAP; ++i) pad[i] = (uint8_t)i;
    if (wasmos_ringbuf_write(&p, pad, 60u) != 60u) return __LINE__;
    uint8_t sink[CAP];
    if (wasmos_ringbuf_read(&c, sink, 60u) != 60u) return __LINE__;

    uint8_t rec[20];
    for (int i = 0; i < 20; ++i) rec[i] = (uint8_t)(0xC0 + i);
    if (wasmos_ringbuf_write_record(&p, rec, 20u) != 20) return __LINE__;

    uint8_t out[20];
    uint32_t rl = 0;
    memset(out, 0, sizeof(out));
    if (wasmos_ringbuf_read_record(&c, out, sizeof(out), &rl) != 20) return __LINE__;
    if (rl != 20u || memcmp(out, rec, 20u) != 0) return __LINE__;
    return 0;
}

/* Negative / mutual-distrust: a forged length prefix must not cause an
 * over-read or a bogus success — the consumer defends against a peer that
 * corrupts its own side of the shared ring. */
static int
test_corrupt_record_prefix(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    /* Forge a prefix claiming a 1000-byte record (> capacity) via the byte API. */
    uint8_t bogus[4] = {0xE8, 0x03, 0x00, 0x00}; /* 1000 LE */
    if (wasmos_ringbuf_write(&p, bogus, 4u) != 4u) return __LINE__;
    uint8_t o[CAP];
    uint32_t rl = 12345u;
    if (wasmos_ringbuf_read_record(&c, o, sizeof(o), &rl) != -1) return __LINE__;
    if (rl != 12345u) return __LINE__;                 /* out_len untouched on corrupt */
    if (wasmos_ringbuf_used(&c) != 4u) return __LINE__; /* nothing consumed */

    /* Overflow case: len = 0xFFFFFFFF so len+4 wraps below 4. Must still reject
     * without over-reading. Reset the indices and forge the max prefix. */
    p.hdr->write = 0u;
    p.hdr->read = 0u;
    uint8_t of[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    if (wasmos_ringbuf_write(&p, of, 4u) != 4u) return __LINE__;
    if (wasmos_ringbuf_read_record(&c, o, sizeof(o), &rl) != -1) return __LINE__;
    if (wasmos_ringbuf_used(&c) != 4u) return __LINE__;
    return 0;
}

static int
test_partial_prefix_peek(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    /* Fewer than 4 bytes queued: no full length prefix yet. */
    uint8_t two[2] = {5, 0};
    if (wasmos_ringbuf_write(&p, two, 2u) != 2u) return __LINE__;
    uint32_t l = 999u;
    if (wasmos_ringbuf_peek_record_len(&c, &l) != 0) return __LINE__;
    if (l != 999u) return __LINE__; /* untouched */
    uint8_t o[4];
    uint32_t rl = 0;
    if (wasmos_ringbuf_read_record(&c, o, sizeof(o), &rl) != -1) return __LINE__;
    return 0;
}

/* Full-state behavior: the core never blocks — a full ring makes writes copy
 * zero (backpressure), which is exactly the flow-control that stands in for
 * blocking. A producer that chooses to drop marks OVERFLOW_DROPPED, and the
 * consumer observes it. */
static int
test_full_state_backpressure(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_region, sizeof(g_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_region, sizeof(g_region)) != 0) return __LINE__;

    uint8_t src[CAP];
    for (uint32_t i = 0; i < CAP; ++i) src[i] = (uint8_t)(i + 1);
    if (wasmos_ringbuf_write(&p, src, CAP) != CAP) return __LINE__;
    if (!wasmos_ringbuf_is_full(&p)) return __LINE__;

    /* Repeated writes into a full ring copy nothing and never overrun. */
    for (int i = 0; i < 5; ++i) {
        if (wasmos_ringbuf_write(&p, src, CAP) != 0u) return __LINE__;
    }
    wasmos_ringbuf_set_flags(&p, WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED);

    /* Consumer frees exactly k; producer can then place exactly k, no more. */
    uint8_t dst[CAP];
    uint32_t k = 10u;
    if (wasmos_ringbuf_read(&c, dst, k) != k) return __LINE__;
    if (wasmos_ringbuf_free(&p) != k) return __LINE__;
    if (wasmos_ringbuf_write(&p, src, CAP) != k) return __LINE__;
    if (!wasmos_ringbuf_is_full(&p)) return __LINE__;

    if ((wasmos_ringbuf_flags(&c) & WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED) == 0u) return __LINE__;
    return 0;
}

/* --- concurrency: real producer/consumer threads over one shared region --- */

/* Small ring, large volume: forces continuous full/empty churn and wraparound
 * while two threads run the acquire/release protocol against each other. The
 * consumer verifies every byte arrives exactly once, in order. */
#define CC_BYTES (2u * 1000u * 1000u)
static uint8_t g_cc_region[WASMOS_RINGBUF_HDR_BYTES + CAP] __attribute__((aligned(64)));
static volatile int g_cc_fail;

static void *
cc_byte_producer(void *arg) {
    wasmos_ringbuf_t *p = (wasmos_ringbuf_t *)arg;
    uint8_t stage[37]; /* odd sizes keep producer/consumer chunk boundaries misaligned */
    uint32_t seq = 0u;
    while (seq < CC_BYTES) {
        uint32_t chunk = 0u;
        while (chunk < sizeof(stage) && seq + chunk < CC_BYTES) {
            stage[chunk] = (uint8_t)((seq + chunk) & 0xFFu);
            chunk++;
        }
        uint32_t off = 0u;
        while (off < chunk) {
            uint32_t n = wasmos_ringbuf_write(p, stage + off, chunk - off);
            if (n == 0u) { sched_yield(); continue; }
            off += n;
        }
        seq += chunk;
    }
    return 0;
}

static void *
cc_byte_consumer(void *arg) {
    wasmos_ringbuf_t *c = (wasmos_ringbuf_t *)arg;
    uint8_t out[53];
    uint32_t rseq = 0u;
    while (rseq < CC_BYTES) {
        uint32_t n = wasmos_ringbuf_read(c, out, sizeof(out));
        if (n == 0u) { sched_yield(); continue; }
        for (uint32_t i = 0; i < n; ++i) {
            if (out[i] != (uint8_t)((rseq + i) & 0xFFu)) { g_cc_fail = 1; return 0; }
        }
        rseq += n;
    }
    return 0;
}

static int
test_concurrent_byte_stream(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_cc_region, sizeof(g_cc_region), CAP) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_cc_region, sizeof(g_cc_region)) != 0) return __LINE__;
    g_cc_fail = 0;
    pthread_t tp, tc;
    if (pthread_create(&tc, 0, cc_byte_consumer, &c) != 0) return __LINE__;
    if (pthread_create(&tp, 0, cc_byte_producer, &p) != 0) return __LINE__;
    pthread_join(tp, 0);
    pthread_join(tc, 0);
    if (g_cc_fail) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&c)) return __LINE__;
    return 0;
}

/* Concurrency over the record framing: each record carries its own index and a
 * checkable payload; the consumer verifies record boundaries, ordering, and
 * content are preserved under real threading and wraparound. */
#define CC_RECS 200000u
static uint8_t g_cc_rec_region[WASMOS_RINGBUF_HDR_BYTES + 256u] __attribute__((aligned(64)));
static volatile int g_cc_rec_fail;

static void *
cc_rec_producer(void *arg) {
    wasmos_ringbuf_t *p = (wasmos_ringbuf_t *)arg;
    uint8_t buf[20];
    for (uint32_t idx = 0; idx < CC_RECS; ++idx) {
        uint32_t plen = 4u + (idx % 17u); /* 4..20 bytes */
        buf[0] = (uint8_t)(idx & 0xFFu);
        buf[1] = (uint8_t)((idx >> 8) & 0xFFu);
        buf[2] = (uint8_t)((idx >> 16) & 0xFFu);
        buf[3] = (uint8_t)((idx >> 24) & 0xFFu);
        for (uint32_t i = 4; i < plen; ++i) buf[i] = (uint8_t)((idx + i) & 0xFFu);
        while (wasmos_ringbuf_write_record(p, buf, plen) < 0) sched_yield();
    }
    return 0;
}

static void *
cc_rec_consumer(void *arg) {
    wasmos_ringbuf_t *c = (wasmos_ringbuf_t *)arg;
    uint8_t buf[64];
    for (uint32_t idx = 0; idx < CC_RECS; ++idx) {
        uint32_t rlen = 0u;
        int32_t rc;
        while ((rc = wasmos_ringbuf_read_record(c, buf, sizeof(buf), &rlen)) == -1) sched_yield();
        uint32_t expect = 4u + (idx % 17u);
        if (rc < 0 || (uint32_t)rc != expect) { g_cc_rec_fail = 1; return 0; }
        uint32_t got = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        if (got != idx) { g_cc_rec_fail = 2; return 0; }
        for (uint32_t i = 4; i < expect; ++i) {
            if (buf[i] != (uint8_t)((idx + i) & 0xFFu)) { g_cc_rec_fail = 3; return 0; }
        }
    }
    return 0;
}

static int
test_concurrent_records(void) {
    wasmos_ringbuf_t p, c;
    if (wasmos_ringbuf_init(&p, g_cc_rec_region, sizeof(g_cc_rec_region), 256u) != 0) return __LINE__;
    if (wasmos_ringbuf_attach(&c, g_cc_rec_region, sizeof(g_cc_rec_region)) != 0) return __LINE__;
    g_cc_rec_fail = 0;
    pthread_t tp, tc;
    if (pthread_create(&tc, 0, cc_rec_consumer, &c) != 0) return __LINE__;
    if (pthread_create(&tp, 0, cc_rec_producer, &p) != 0) return __LINE__;
    pthread_join(tp, 0);
    pthread_join(tc, 0);
    if (g_cc_rec_fail) return __LINE__;
    if (!wasmos_ringbuf_is_empty(&c)) return __LINE__;
    return 0;
}

/* More negative attach cases: every header field the validator checks must
 * reject a mismatch, and init must reject a zero capacity. */
static int
test_attach_negatives(void) {
    static uint8_t region[WASMOS_RINGBUF_HDR_BYTES + 64u] __attribute__((aligned(64)));
    wasmos_ringbuf_t rb, a;
    CK(wasmos_ringbuf_init(&rb, region, sizeof(region), 64u) == 0);
    CK(wasmos_ringbuf_attach(&a, region, sizeof(region)) == 0); /* baseline good */

    rb.hdr->version = (uint16_t)(WASMOS_RINGBUF_VERSION + 1u);
    CK(wasmos_ringbuf_attach(&a, region, sizeof(region)) != 0);
    rb.hdr->version = (uint16_t)WASMOS_RINGBUF_VERSION;

    rb.hdr->hdr_bytes = 32u;
    CK(wasmos_ringbuf_attach(&a, region, sizeof(region)) != 0);
    rb.hdr->hdr_bytes = (uint16_t)WASMOS_RINGBUF_HDR_BYTES;

    rb.hdr->capacity = 3u; /* not a power of two */
    CK(wasmos_ringbuf_attach(&a, region, sizeof(region)) != 0);
    rb.hdr->capacity = 64u;

    /* region smaller than the declared capacity needs. */
    CK(wasmos_ringbuf_attach(&a, region, WASMOS_RINGBUF_HDR_BYTES + 32u) != 0);

    rb.hdr->magic = 0xDEADBEEFu;
    CK(wasmos_ringbuf_attach(&a, region, sizeof(region)) != 0);
    rb.hdr->magic = WASMOS_RINGBUF_MAGIC;
    CK(wasmos_ringbuf_attach(&a, region, sizeof(region)) == 0); /* restored */

    CK(wasmos_ringbuf_init(&rb, region, sizeof(region), 0u) != 0); /* zero cap */
    return 0;
}

/* Invariants must hold across every power-of-two capacity, not just CAP=64:
 * fill fully, confirm full + write-blocked, drain fully in order, confirm
 * empty. Exercises the pos & (capacity-1) masking for cap = 1 .. 65536. */
static int
test_capacity_sweep(void) {
    for (uint32_t cap = 1u; cap <= 65536u; cap <<= 1) {
        uint32_t total = wasmos_ringbuf_bytes_for(cap);
        uint8_t *region = (uint8_t *)malloc(total);
        if (region == 0) return __LINE__;
        wasmos_ringbuf_t p, c;
        CK(wasmos_ringbuf_init(&p, region, total, cap) == 0);
        CK(wasmos_ringbuf_attach(&c, region, total) == 0);
        CK(wasmos_ringbuf_is_empty(&p));
        CK(wasmos_ringbuf_free(&p) == cap);

        uint8_t chunk[64];
        uint32_t produced = 0;
        while (produced < cap) {
            uint32_t want = cap - produced;
            if (want > sizeof(chunk)) want = sizeof(chunk);
            for (uint32_t i = 0; i < want; ++i) chunk[i] = (uint8_t)((produced + i) & 0xFFu);
            CK(wasmos_ringbuf_write(&p, chunk, want) == want);
            produced += want;
        }
        CK(wasmos_ringbuf_is_full(&p));
        CK(wasmos_ringbuf_write(&p, chunk, 1u) == 0u); /* full: writes nothing */

        uint8_t out[64];
        uint32_t consumed = 0;
        while (consumed < cap) {
            uint32_t n = wasmos_ringbuf_read(&c, out, sizeof(out));
            CK(n > 0u);
            uint8_t exp[64];
            for (uint32_t i = 0; i < n; ++i) exp[i] = (uint8_t)((consumed + i) & 0xFFu);
            CK(memcmp(out, exp, n) == 0); /* one check per read, not per byte */
            consumed += n;
        }
        CK(wasmos_ringbuf_is_empty(&c));
        free(region);
    }
    return 0;
}

/* Exhaustively exercise the two-part wraparound copy: for every physical start
 * offset in [0, cap) and every length in [1, cap], write then read back and
 * confirm byte-exact round-trip. This hits both store and load straddling the
 * ring end at every possible boundary. */
static int
test_exhaustive_wrap_offsets(void) {
    const uint32_t cap = 64u;
    static uint8_t region[WASMOS_RINGBUF_HDR_BYTES + 64u] __attribute__((aligned(64)));
    for (uint32_t start = 0; start < cap; ++start) {
        for (uint32_t len = 1u; len <= cap; ++len) {
            wasmos_ringbuf_t p, c;
            CK(wasmos_ringbuf_init(&p, region, sizeof(region), cap) == 0);
            CK(wasmos_ringbuf_attach(&c, region, sizeof(region)) == 0);
            p.hdr->write = start; /* begin at an arbitrary physical offset */
            p.hdr->read = start;

            uint8_t src[64];
            for (uint32_t i = 0; i < len; ++i) src[i] = (uint8_t)(0x40u + ((start + i) & 0x3Fu));
            CK(wasmos_ringbuf_write(&p, src, len) == len);
            uint8_t out[64];
            memset(out, 0, sizeof(out));
            CK(wasmos_ringbuf_read(&c, out, len) == len);
            CK(memcmp(out, src, len) == 0);
            CK(wasmos_ringbuf_is_empty(&c));
        }
    }
    return 0;
}

/* xorshift32, deterministic (fixed seed) so the fuzz runs are reproducible. */
#define FZ_STEP(fz) ((fz) ^= (fz) << 13, (fz) ^= (fz) >> 17, (fz) ^= (fz) << 5, (fz))

/* Model-based byte-stream fuzz: drive a long random sequence of writes and
 * reads and check the ring against a reference FIFO model (produced/consumed
 * counters + the generating byte sequence) on every operation. */
static int
test_fuzz_byte_stream(void) {
    const uint32_t cap = 64u;
    static uint8_t region[WASMOS_RINGBUF_HDR_BYTES + 64u] __attribute__((aligned(64)));
    wasmos_ringbuf_t p, c;
    CK(wasmos_ringbuf_init(&p, region, sizeof(region), cap) == 0);
    CK(wasmos_ringbuf_attach(&c, region, sizeof(region)) == 0);

    uint32_t fz = 0x00C0FFEEu;
    uint32_t produced = 0, consumed = 0;
    uint8_t buf[64];
    for (int op = 0; op < 20000; ++op) {
        CK(wasmos_ringbuf_used(&p) == produced - consumed);
        uint32_t want = FZ_STEP(fz) % 41u; /* 0..40 */
        if (FZ_STEP(fz) & 1u) {
            for (uint32_t i = 0; i < want; ++i) buf[i] = (uint8_t)((produced + i) & 0xFFu);
            uint32_t freeb = cap - (produced - consumed);
            uint32_t expect = want < freeb ? want : freeb;
            CK(wasmos_ringbuf_write(&p, buf, want) == expect);
            produced += expect;
        } else {
            uint32_t used = produced - consumed;
            uint32_t expect = want < used ? want : used;
            CK(wasmos_ringbuf_read(&c, buf, want) == expect);
            uint8_t exp[64];
            for (uint32_t i = 0; i < expect; ++i) exp[i] = (uint8_t)((consumed + i) & 0xFFu);
            CK(memcmp(buf, exp, expect) == 0); /* one check per read */
            consumed += expect;
        }
    }
    return 0;
}

/* Model-based record fuzz: random write_record / read_record against a FIFO
 * model of pending records (length + generating index). Every framing decision
 * (fit/no-fit, boundary, ordering, content) is checked against the model. */
static int
test_fuzz_records(void) {
    const uint32_t cap = 256u;
    static uint8_t region[WASMOS_RINGBUF_HDR_BYTES + 256u] __attribute__((aligned(64)));
    wasmos_ringbuf_t p, c;
    CK(wasmos_ringbuf_init(&p, region, sizeof(region), cap) == 0);
    CK(wasmos_ringbuf_attach(&c, region, sizeof(region)) == 0);

    static uint32_t qlen[2048];
    static uint32_t qidx[2048];
    uint32_t head = 0, tail = 0, count = 0;
    uint32_t next_idx = 0;

    uint32_t fz = 0x9E3779B9u;
    uint8_t wbuf[44];
    uint8_t rbuf[300];
    for (int op = 0; op < 20000; ++op) {
        if (FZ_STEP(fz) & 1u) {
            uint32_t plen = FZ_STEP(fz) % 41u; /* 0..40 */
            for (uint32_t i = 0; i < plen; ++i) wbuf[i] = (uint8_t)((next_idx * 31u + i) & 0xFFu);
            uint32_t freeb = wasmos_ringbuf_free(&p);
            int32_t rc = wasmos_ringbuf_write_record(&p, wbuf, plen);
            if (plen + 4u <= freeb) {
                CK(rc == (int32_t)plen);
                qlen[tail] = plen;
                qidx[tail] = next_idx;
                tail = (tail + 1u) % 2048u;
                next_idx++;
                count++;
            } else {
                CK(rc == -1); /* did not fit */
            }
        } else {
            uint32_t rlen = 0;
            int32_t rc = wasmos_ringbuf_read_record(&c, rbuf, sizeof(rbuf), &rlen);
            if (count == 0u) {
                CK(rc == -1);
            } else {
                uint32_t elen = qlen[head];
                uint32_t eidx = qidx[head];
                CK(rc == (int32_t)elen);
                CK(rlen == elen);
                uint8_t exp[44];
                for (uint32_t i = 0; i < elen; ++i) exp[i] = (uint8_t)((eidx * 31u + i) & 0xFFu);
                CK(memcmp(rbuf, exp, elen) == 0); /* one check per record */
                head = (head + 1u) % 2048u;
                count--;
            }
        }
    }
    return 0;
}

#define RUN(fn) do { \
        groups++; \
        if ((rc = fn()) != 0) { \
            fprintf(stderr, "test_ringbuf: FAIL %s at line %d\n", #fn, rc); \
            return rc; \
        } \
    } while (0)

int
main(void) {
    int rc;
    int groups = 0;
    RUN(test_bytes_for_and_layout);
    RUN(test_reject_bad_params);
    RUN(test_attach_matches_init);
    RUN(test_write_read_and_flow_control);
    RUN(test_wraparound_payload);
    RUN(test_index_counter_wrap);
    RUN(test_datagram_framing);
    RUN(test_datagram_undersized_dst_and_toobig);
    RUN(test_doorbell_edge);
    RUN(test_flags);
    RUN(test_zero_length_ops);
    RUN(test_skip_and_short_read);
    RUN(test_record_size_boundary);
    RUN(test_empty_datagram);
    RUN(test_record_wraparound);
    RUN(test_corrupt_record_prefix);
    RUN(test_partial_prefix_peek);
    RUN(test_full_state_backpressure);
    RUN(test_attach_negatives);
    RUN(test_capacity_sweep);
    RUN(test_exhaustive_wrap_offsets);
    RUN(test_fuzz_byte_stream);
    RUN(test_fuzz_records);
    RUN(test_concurrent_byte_stream);
    RUN(test_concurrent_records);
    printf("test_ringbuf: %d groups, %ld checks passed\n", groups, g_checks);
    return 0;
}