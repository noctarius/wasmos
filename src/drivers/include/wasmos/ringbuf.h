#ifndef WASMOS_RINGBUF_H
#define WASMOS_RINGBUF_H

/* General-purpose single-producer/single-consumer byte ring buffer.
 *
 * Pure ring logic over a caller-provided memory region and a notify callback,
 * with NO IPC, buffer-object, or transport knowledge — the same shape as the
 * vring core (wasmos/vring.h). The region is expected to live in memory shared
 * by exactly two parties (one producer, one consumer): typically an xfer-buffer
 * object one side owns and borrows to the other, overlaid at a stable address
 * on both ends. This core does not create, map, or borrow that region; it only
 * reads and writes the header + data bytes inside it.
 *
 * Layout (matches docs/architecture/22, but not networking-specific):
 *   +0            64-byte header (wasmos_ringbuf_hdr_t)
 *   +64           data region: `capacity` bytes, capacity a power of two
 *
 * Indices are free-running unsigned 32-bit counters, never stored modulo:
 *   empty = (write == read);  full = (write - read == capacity)
 * Index into the data region with `pos & (capacity - 1)`. Unsigned wrap at 2^32
 * is harmless with a power-of-two capacity, and this removes the read==write
 * full/empty ambiguity a modulo scheme has.
 *
 * Ordering is directional acquire/release, not a global barrier. The producer
 * writes payload then release-stores `write`; the consumer acquire-loads `write`
 * before reading payload, then release-stores `read`. Symmetric on the other
 * index. On x86 aligned u32 accesses are atomic and TSO supplies most ordering,
 * but the discipline (do the ops in that order, with acquire/release) is what
 * makes it correct, not the atomicity alone.
 *
 * Framing: the ring is byte-oriented. A raw byte stream (e.g. TCP) uses
 * wasmos_ringbuf_write / _read directly. A datagram stream (e.g. UDP) prefixes
 * each record with a 4-byte length using wasmos_ringbuf_write_record /
 * _read_record; a record is written all-or-nothing so a visible length prefix
 * always has its full payload behind it.
 *
 * Mutual distrust note: this core trusts the two indices to be consistent with
 * each other (a compromised peer can corrupt its own side of a shared ring).
 * All copies are bounded by the live free/used span computed from the indices,
 * so a bad index can only cause short/empty transfers within the region, never
 * an out-of-region access. Callers that share a ring with an untrusted peer
 * must still treat received bytes as untrusted input.
 *
 * Header-only (static inline), matching the rest of libsys. Depends only on
 * <stdint.h> and clang/gcc atomic + memcpy builtins. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WASMOS_RINGBUF_MAGIC 0x474E5257u /* 'WRNG' (little-endian) */
#define WASMOS_RINGBUF_VERSION 1u
#define WASMOS_RINGBUF_HDR_BYTES 64u

/* Header flags. Either party may set these; they are advisory state, not part
 * of the index protocol. */
#define WASMOS_RINGBUF_FLAG_PEER_CLOSED (1u << 0)      /* other end went away */
#define WASMOS_RINGBUF_FLAG_RESET (1u << 1)            /* stream reset/aborted */
#define WASMOS_RINGBUF_FLAG_OVERFLOW_DROPPED (1u << 2) /* producer dropped data */

/* Shared header, fixed 64 bytes. `write` and `read` are kept far apart within
 * the header so the two owners touch different words; the reserved padding also
 * leaves room to grow the ABI (stats, close/reset detail) without re-cutting
 * the header size. Producer owns `write`; consumer owns `read`. */
typedef struct __attribute__((packed, aligned(64))) wasmos_ringbuf_hdr {
    uint32_t magic;     /* +0  WASMOS_RINGBUF_MAGIC */
    uint16_t version;   /* +4  WASMOS_RINGBUF_VERSION */
    uint16_t hdr_bytes; /* +6  = WASMOS_RINGBUF_HDR_BYTES (64) */
    uint32_t capacity;  /* +8  data-region bytes; power of two */
    uint32_t flags;     /* +12 WASMOS_RINGBUF_FLAG_* */
    uint32_t write;     /* +16 producer-owned, free-running */
    uint32_t _pad_w[7]; /* +20 pad producer/consumer words apart */
    uint32_t read;      /* +48 consumer-owned, free-running */
    uint32_t _pad_r[3]; /* +52 pad to 64 bytes total */
} wasmos_ringbuf_hdr_t;

_Static_assert(sizeof(wasmos_ringbuf_hdr_t) == WASMOS_RINGBUF_HDR_BYTES,
               "wasmos_ringbuf_hdr_t must be exactly 64 bytes");

/* Live handle onto a region. Pointers reference into the caller's mapping; the
 * handle itself is private per party (not shared) and is rebuilt with attach()
 * whenever the region is (re)mapped. */
typedef struct {
    wasmos_ringbuf_hdr_t* hdr;  /* region base */
    uint8_t* data;              /* region base + hdr_bytes */
    uint32_t capacity;          /* cached hdr->capacity (power of two) */
    void (*notify)(void* user); /* doorbell (backend-supplied), may be NULL */
    void* notify_user;
} wasmos_ringbuf_t;

/* --- sizing / validation helpers --- */

static inline int32_t wasmos_ringbuf_is_pow2(uint32_t x) {
    return x != 0u && (x & (x - 1u)) == 0u;
}

/* Total region bytes a ring with the given data capacity needs. Use this to
 * size the backing buffer (e.g. wasmos_xfer_buffer_acquire). */
static inline uint32_t wasmos_ringbuf_bytes_for(uint32_t capacity) {
    return WASMOS_RINGBUF_HDR_BYTES + capacity;
}

/* --- setup --- */

/* Initialize a fresh ring in [base, base+region_bytes): write the header, zero
 * the indices, and build the handle. Call this once, on the party that creates
 * the region (typically the producer/owner) before the peer attaches. capacity
 * must be a power of two and must fit: region_bytes >= hdr + capacity. Returns
 * 0 on success, -1 on bad parameters. */
static inline int32_t wasmos_ringbuf_init(wasmos_ringbuf_t* rb, void* base, uint32_t region_bytes,
                                          uint32_t capacity) {
    if (rb == 0 || base == 0)
        return -1;
    if (!wasmos_ringbuf_is_pow2(capacity))
        return -1;
    if (region_bytes < wasmos_ringbuf_bytes_for(capacity))
        return -1;

    wasmos_ringbuf_hdr_t* hdr = (wasmos_ringbuf_hdr_t*)base;
    hdr->capacity = capacity;
    hdr->flags = 0u;
    hdr->write = 0u;
    hdr->read = 0u;
    hdr->version = (uint16_t)WASMOS_RINGBUF_VERSION;
    hdr->hdr_bytes = (uint16_t)WASMOS_RINGBUF_HDR_BYTES;
    /* Publish magic last (release): a peer that sees the magic sees a fully
     * formed header behind it. */
    __atomic_store_n(&hdr->magic, WASMOS_RINGBUF_MAGIC, __ATOMIC_RELEASE);

    rb->hdr = hdr;
    rb->data = (uint8_t*)base + WASMOS_RINGBUF_HDR_BYTES;
    rb->capacity = capacity;
    rb->notify = 0;
    rb->notify_user = 0;
    return 0;
}

/* Build a handle onto an already-initialized ring (the peer's view, or a
 * re-map after the region moved in the local address space). Validates the
 * header (magic/version/hdr_bytes and a power-of-two capacity that fits) but
 * does NOT touch the indices. Returns 0 on success, -1 if the header is not a
 * valid ring or does not fit region_bytes. */
static inline int32_t wasmos_ringbuf_attach(wasmos_ringbuf_t* rb, void* base,
                                            uint32_t region_bytes) {
    if (rb == 0 || base == 0)
        return -1;
    wasmos_ringbuf_hdr_t* hdr = (wasmos_ringbuf_hdr_t*)base;
    if (__atomic_load_n(&hdr->magic, __ATOMIC_ACQUIRE) != WASMOS_RINGBUF_MAGIC)
        return -1;
    if (hdr->version != (uint16_t)WASMOS_RINGBUF_VERSION)
        return -1;
    if (hdr->hdr_bytes != (uint16_t)WASMOS_RINGBUF_HDR_BYTES)
        return -1;
    uint32_t capacity = hdr->capacity;
    if (!wasmos_ringbuf_is_pow2(capacity))
        return -1;
    if (region_bytes < wasmos_ringbuf_bytes_for(capacity))
        return -1;

    rb->hdr = hdr;
    rb->data = (uint8_t*)base + WASMOS_RINGBUF_HDR_BYTES;
    rb->capacity = capacity;
    rb->notify = 0;
    rb->notify_user = 0;
    return 0;
}

static inline void wasmos_ringbuf_set_notify(wasmos_ringbuf_t* rb, void (*notify)(void* user),
                                             void* user) {
    rb->notify = notify;
    rb->notify_user = user;
}

/* --- occupancy queries (safe from either side) --- */

/* Bytes currently queued (written but not yet consumed). Acquire-loads both
 * indices so it is correct whether the caller is producer or consumer. */
static inline uint32_t wasmos_ringbuf_used(const wasmos_ringbuf_t* rb) {
    uint32_t w = __atomic_load_n(&rb->hdr->write, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&rb->hdr->read, __ATOMIC_ACQUIRE);
    return w - r; /* unsigned wrap is intentional and correct */
}

static inline uint32_t wasmos_ringbuf_free(const wasmos_ringbuf_t* rb) {
    return rb->capacity - wasmos_ringbuf_used(rb);
}

static inline int32_t wasmos_ringbuf_is_empty(const wasmos_ringbuf_t* rb) {
    return wasmos_ringbuf_used(rb) == 0u;
}

static inline int32_t wasmos_ringbuf_is_full(const wasmos_ringbuf_t* rb) {
    return wasmos_ringbuf_used(rb) == rb->capacity;
}

/* --- flags --- */

static inline uint32_t wasmos_ringbuf_flags(const wasmos_ringbuf_t* rb) {
    return __atomic_load_n(&rb->hdr->flags, __ATOMIC_ACQUIRE);
}

static inline void wasmos_ringbuf_set_flags(wasmos_ringbuf_t* rb, uint32_t mask) {
    __atomic_or_fetch(&rb->hdr->flags, mask, __ATOMIC_RELEASE);
}

/* --- internal copy helpers (wraparound-aware) --- */

static inline void wasmos_ringbuf__store(wasmos_ringbuf_t* rb, uint32_t wpos, const uint8_t* src,
                                         uint32_t n) {
    uint32_t cap = rb->capacity;
    uint32_t off = wpos & (cap - 1u);
    uint32_t first = cap - off;
    if (first > n)
        first = n;
    __builtin_memcpy(rb->data + off, src, first);
    if (n > first)
        __builtin_memcpy(rb->data, src + first, n - first);
}

static inline void wasmos_ringbuf__load(const wasmos_ringbuf_t* rb, uint32_t rpos, uint8_t* dst,
                                        uint32_t n) {
    uint32_t cap = rb->capacity;
    uint32_t off = rpos & (cap - 1u);
    uint32_t first = cap - off;
    if (first > n)
        first = n;
    __builtin_memcpy(dst, rb->data + off, first);
    if (n > first)
        __builtin_memcpy(dst + first, rb->data, n - first);
}

/* --- producer: byte stream --- */

/* Write up to `len` bytes; copies min(len, free) and returns that count (a
 * short write means the ring was near full — that IS the flow control). Does
 * not ring the doorbell; use wasmos_ringbuf_write_signal for that. */
static inline uint32_t wasmos_ringbuf_write(wasmos_ringbuf_t* rb, const void* src, uint32_t len) {
    uint32_t w = __atomic_load_n(&rb->hdr->write, __ATOMIC_RELAXED); /* sole writer */
    uint32_t r = __atomic_load_n(&rb->hdr->read, __ATOMIC_ACQUIRE);
    uint32_t freeb = rb->capacity - (w - r);
    uint32_t n = (len < freeb) ? len : freeb;
    if (n != 0u) {
        wasmos_ringbuf__store(rb, w, (const uint8_t*)src, n);
        __atomic_store_n(&rb->hdr->write, w + n, __ATOMIC_RELEASE);
    }
    return n;
}

/* --- consumer: byte stream --- */

/* Copy up to `len` bytes out and advance the read index; returns the count
 * copied (0 when empty). */
static inline uint32_t wasmos_ringbuf_read(wasmos_ringbuf_t* rb, void* dst, uint32_t len) {
    uint32_t r = __atomic_load_n(&rb->hdr->read, __ATOMIC_RELAXED); /* sole reader */
    uint32_t w = __atomic_load_n(&rb->hdr->write, __ATOMIC_ACQUIRE);
    uint32_t used = w - r;
    uint32_t n = (len < used) ? len : used;
    if (n != 0u) {
        wasmos_ringbuf__load(rb, r, (uint8_t*)dst, n);
        __atomic_store_n(&rb->hdr->read, r + n, __ATOMIC_RELEASE);
    }
    return n;
}

/* Copy up to `len` bytes out WITHOUT advancing the read index; returns the
 * count copied. Useful to inspect a length prefix before committing. */
static inline uint32_t wasmos_ringbuf_peek(const wasmos_ringbuf_t* rb, void* dst, uint32_t len) {
    uint32_t r = __atomic_load_n(&rb->hdr->read, __ATOMIC_RELAXED);
    uint32_t w = __atomic_load_n(&rb->hdr->write, __ATOMIC_ACQUIRE);
    uint32_t used = w - r;
    uint32_t n = (len < used) ? len : used;
    if (n != 0u)
        wasmos_ringbuf__load(rb, r, (uint8_t*)dst, n);
    return n;
}

/* Discard up to `len` bytes from the front; returns the count skipped. */
static inline uint32_t wasmos_ringbuf_skip(wasmos_ringbuf_t* rb, uint32_t len) {
    uint32_t r = __atomic_load_n(&rb->hdr->read, __ATOMIC_RELAXED);
    uint32_t w = __atomic_load_n(&rb->hdr->write, __ATOMIC_ACQUIRE);
    uint32_t used = w - r;
    uint32_t n = (len < used) ? len : used;
    if (n != 0u)
        __atomic_store_n(&rb->hdr->read, r + n, __ATOMIC_RELEASE);
    return n;
}

/* --- datagram framing (length-prefixed records) --- */

/* Write one record: a 4-byte little-endian length prefix followed by the
 * payload, published all-or-nothing (the write index only advances once the
 * whole record is in the ring, so a consumer that sees the prefix always has
 * the full payload behind it). Returns `len` on success, or -1 if the record
 * (4 + len bytes) does not currently fit — the caller retries or drops. A
 * record larger than the ring capacity can never fit and always returns -1. */
static inline int32_t wasmos_ringbuf_write_record(wasmos_ringbuf_t* rb, const void* src,
                                                  uint32_t len) {
    uint32_t need = len + 4u;
    if (need < len)
        return -1; /* length overflow */
    uint32_t w = __atomic_load_n(&rb->hdr->write, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&rb->hdr->read, __ATOMIC_ACQUIRE);
    uint32_t freeb = rb->capacity - (w - r);
    if (need > freeb)
        return -1;

    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len & 0xFFu);
    hdr[1] = (uint8_t)((len >> 8) & 0xFFu);
    hdr[2] = (uint8_t)((len >> 16) & 0xFFu);
    hdr[3] = (uint8_t)((len >> 24) & 0xFFu);
    wasmos_ringbuf__store(rb, w, hdr, 4u);
    if (len != 0u)
        wasmos_ringbuf__store(rb, w + 4u, (const uint8_t*)src, len);
    __atomic_store_n(&rb->hdr->write, w + need, __ATOMIC_RELEASE);
    return (int32_t)len;
}

/* Peek the length of the record at the front without consuming it. Returns 1
 * and sets *out_len when a full length prefix is present, 0 when fewer than 4
 * bytes are queued. (A published prefix implies its payload is present.) */
static inline int32_t wasmos_ringbuf_peek_record_len(const wasmos_ringbuf_t* rb,
                                                     uint32_t* out_len) {
    uint8_t hdr[4];
    if (wasmos_ringbuf_peek(rb, hdr, 4u) != 4u)
        return 0;
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
                   ((uint32_t)hdr[3] << 24);
    if (out_len)
        *out_len = len;
    return 1;
}

/* Read one record into dst (capacity `max`). On success consumes the whole
 * record (prefix + payload), sets *out_len to the record length, and returns
 * the number of payload bytes copied. Returns:
 *   -1 : no complete record at the front, or a corrupt length prefix (nothing
 *        consumed; *out_len untouched),
 *   -2 : record does not fit in `max` (nothing consumed; *out_len is set to the
 *        record length so the caller can grow its buffer and retry). */
static inline int32_t wasmos_ringbuf_read_record(wasmos_ringbuf_t* rb, void* dst, uint32_t max,
                                                 uint32_t* out_len) {
    uint32_t len = 0u;
    if (!wasmos_ringbuf_peek_record_len(rb, &len))
        return -1;
    /* Reject a corrupt/oversized length prefix WITHOUT over-reading (mutual
     * distrust: the peer owns its side of a shared ring). `need` overflowing
     * (len near UINT32_MAX) or exceeding capacity means no legitimate producer
     * could have published this record — write_record caps need at free <=
     * capacity. Do not report the bogus length to the caller. */
    uint32_t need = len + 4u;
    if (need < 4u || need > rb->capacity)
        return -1;
    /* Whole record must be present (a correct producer publishes it atomically,
     * so this only trips on a truncated/forged stream). */
    if (wasmos_ringbuf_used(rb) < need)
        return -1;
    if (out_len)
        *out_len = len;
    if (len > max)
        return -2;

    (void)wasmos_ringbuf_skip(rb, 4u);
    if (len != 0u)
        (void)wasmos_ringbuf_read(rb, dst, len);
    return (int32_t)len;
}

/* --- doorbell edge helpers --- */
/*
 * Edge-triggered signalling with the lost-wakeup discipline: the PRODUCER
 * publishes its bytes THEN, if the ring was empty before, rings the doorbell
 * (empty->non-empty edge). The CONSUMER, symmetrically, must re-check occupancy
 * AFTER arming its wait so a signal that raced the arm is not lost — that
 * re-check lives in the IPC/wait layer, not in this pure core.
 */

/* write() a byte run and ring the doorbell if this write took the ring from
 * empty to non-empty. Returns bytes written. */
static inline uint32_t wasmos_ringbuf_write_signal(wasmos_ringbuf_t* rb, const void* src,
                                                   uint32_t len) {
    uint32_t before = wasmos_ringbuf_used(rb);
    uint32_t n = wasmos_ringbuf_write(rb, src, len);
    if (before == 0u && n != 0u && rb->notify)
        rb->notify(rb->notify_user);
    return n;
}

/* write_record() and ring the doorbell on the empty->non-empty edge. Returns
 * the _write_record result (len, or -1 when the record does not fit). */
static inline int32_t wasmos_ringbuf_write_record_signal(wasmos_ringbuf_t* rb, const void* src,
                                                         uint32_t len) {
    uint32_t before = wasmos_ringbuf_used(rb);
    int32_t rc = wasmos_ringbuf_write_record(rb, src, len);
    if (before == 0u && rc >= 0 && rb->notify)
        rb->notify(rb->notify_user);
    return rc;
}

#ifdef __cplusplus
}
#endif

#endif /* WASMOS_RINGBUF_H */