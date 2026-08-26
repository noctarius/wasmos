#ifndef WASMOS_VRING_H
#define WASMOS_VRING_H

/* Transport-neutral virtqueue (split-ring) core.
 *
 * Pure descriptor-table + avail/used-ring logic over a caller-provided memory
 * region and a notify callback, with NO device, PCI, or IPC knowledge — the
 * point of zero-copy is to keep the kernel and transport out of the data path
 * (see docs/architecture/09-process-and-ipc.md). Two backends drive it:
 *   - PCI/MMIO backend: region is a pinned physical region (wasmos_region_alloc),
 *     addresses in descriptors are device physical addresses, and the notify
 *     callback writes the device's QUEUE_NOTIFY doorbell.
 *   - shmem/IPC backend: region is a shmem mapping shared with a peer service,
 *     and the notify callback rings a NOTIFICATION endpoint.
 *
 * Layout is the legacy split-virtqueue layout (virtio 0.9.5 / legacy PCI):
 *   [ descriptor table | available ring | pad to `align` | used ring ]
 * EVENT_IDX suppression is not used; the trailing event fields are reserved in
 * the size calculation for layout compatibility but left untouched.
 *
 * Mutual distrust: the consumer bounds-checks every used-ring element (id < num)
 * before touching a descriptor, so a buggy or malicious producer can at worst
 * fault within the shared region — it cannot reach the consumer's private
 * memory. For device vrings the same bound is backed by the region allocation's
 * capability window and the low-2 GiB clamp.
 *
 * Header-only (static inline), matching the rest of libsys. Descriptor
 * addresses are DEVICE addresses (region_phys + offset); the caller translates
 * between its linear-memory view of the region and those device addresses.
 *
 * Descriptor lifecycle, driver side. A descriptor index is owned by exactly one
 * of three places and moves between them in this order:
 *
 *   free list -> vring_alloc_desc() -> owned by the driver, being filled
 *             -> vring_publish()    -> owned by the DEVICE, must not be touched
 *             -> vring_get_used()   -> owned by the driver again, buffer readable
 *             -> vring_free_desc()  -> free list
 *
 * A request spanning several buffers takes the same path through
 * vring_alloc_chain() and vring_free_chain(), which allocate and release the
 * whole VRING_DESC_F_NEXT chain as one unit.
 *
 * Nothing enforces that order: publishing an index twice, or freeing one the
 * device still owns, hands the same buffer to two users. The buffer a
 * descriptor points at is likewise off-limits between publish and get_used -
 * for a device-writable (VRING_DESC_F_WRITE) descriptor the device is writing
 * into it, and its contents are only meaningful after get_used() reports the
 * completion length.
 *
 * Ordering. vring_publish() fences between the descriptor/ring-entry stores and
 * the avail_idx bump, so a device that observes the new index also observes
 * what it points at. vring_kick() fences before the doorbell, so avail_idx is
 * visible before the device is told to look. On the completion side the fence
 * lives in vring_has_used(), which is why the used-ring element must only be
 * read through vring_get_used(). Nothing here is a cache-maintenance
 * operation: it is store/load ordering only, which is what a cache-coherent
 * x86 DMA device needs.
 *
 * Concurrency. Single producer, single consumer: one context must own the
 * queue. avail_idx, free_head/num_free and last_used_idx are all plain
 * read-modify-write state with no locking. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Descriptor flags. */
#define VRING_DESC_F_NEXT 1u     /* buffer continues in `next` */
#define VRING_DESC_F_WRITE 2u    /* device writes into this buffer (device-writable) */
#define VRING_DESC_F_INDIRECT 4u /* buffer is an indirect descriptor table (unused) */

/* One descriptor-table entry, laid out as the device reads it. While the
 * descriptor sits on the free list `next` is reused as the free-list link. */
typedef struct __attribute__((packed)) {
    uint64_t addr; /* device/physical address of the buffer */
    uint32_t len;  /* buffer size in bytes */
    uint16_t flags;
    uint16_t next; /* chain index when VRING_DESC_F_NEXT is set */
} vring_desc_t;

/* One used-ring element: the device's report that it has finished with a
 * previously published chain. */
typedef struct __attribute__((packed)) {
    uint32_t id;  /* index of the head descriptor of the completed chain */
    uint32_t len; /* number of bytes written by the device */
} vring_used_elem_t;

/* A live virtqueue. Pointers reference into the caller's mapped region, so the
 * whole struct is invalidated if that region is unmapped or moved; re-run
 * vring_layout() rather than patching it. All of it is filled in by
 * vring_layout(); callers read it but do not assign to it. */
typedef struct {
    uint16_t num;          /* queue size (power of two) */
    uint64_t region_phys;  /* device address of the region base */
    uint8_t* region_base;  /* caller's linear-memory view of the region */
    uint64_t region_bytes; /* region size, for bounds checks */

    vring_desc_t* desc; /* descriptor table (num entries) */
    uint16_t* avail_flags;
    uint16_t* avail_idx;
    uint16_t* avail_ring; /* num entries */
    uint16_t* used_flags;
    uint16_t* used_idx;
    vring_used_elem_t* used_ring; /* num entries */

    uint16_t free_head;     /* head of the free-descriptor list */
    uint16_t num_free;      /* free descriptors remaining */
    uint16_t last_used_idx; /* last used->idx we consumed */

    void (*notify)(void* user); /* doorbell (backend-supplied) */
    void* notify_user;
} vring_t;

/* Full memory barrier. On x86 DMA is cache-coherent, so ordering (not cache
 * maintenance) is what matters between the driver and a concurrent device. */
static inline void vring_mb(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Round x up to a multiple of `a`, which must be a power of two. */
static inline uint64_t vring_align_up(uint64_t x, uint64_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

/* Bytes of contiguous region a queue of `num` entries needs, `align`-aligned
 * used ring (legacy uses align = 4096). Use this to size wasmos_region_alloc. */
static inline uint64_t vring_size(uint16_t num, uint64_t align) {
    uint64_t desc = (uint64_t)num * sizeof(vring_desc_t);
    uint64_t avail = 2u + 2u + (uint64_t)num * 2u + 2u; /* flags,idx,ring,used_event */
    uint64_t used = 2u + 2u + (uint64_t)num * sizeof(vring_used_elem_t) + 2u;
    return vring_align_up(desc + avail, align) + used;
}

/* Lay a queue out over [region_base, region_base+region_bytes), zero the rings,
 * and build the free-descriptor list. region_phys is the device address of the
 * same region. num must be a power of two. Returns 0 on success, -1 if the
 * region is too small or num is not a power of two. */
static inline int32_t vring_layout(vring_t* vq, uint8_t* region_base, uint64_t region_phys,
                                   uint64_t region_bytes, uint16_t num, uint64_t align) {
    if (num == 0 || (num & (num - 1)) != 0)
        return -1;
    if (region_bytes < vring_size(num, align))
        return -1;

    uint64_t desc_bytes = (uint64_t)num * sizeof(vring_desc_t);
    uint64_t avail_bytes = 2u + 2u + (uint64_t)num * 2u + 2u;
    uint64_t used_off = vring_align_up(desc_bytes + avail_bytes, align);

    vq->num = num;
    vq->region_phys = region_phys;
    vq->region_base = region_base;
    vq->region_bytes = region_bytes;

    vq->desc = (vring_desc_t*)region_base;
    uint8_t* avail = region_base + desc_bytes;
    vq->avail_flags = (uint16_t*)avail;
    vq->avail_idx = (uint16_t*)(avail + 2);
    vq->avail_ring = (uint16_t*)(avail + 4);
    uint8_t* used = region_base + used_off;
    vq->used_flags = (uint16_t*)used;
    vq->used_idx = (uint16_t*)(used + 2);
    vq->used_ring = (vring_used_elem_t*)(used + 4);

    /* Zero the whole queue region so idx/flags start clean. */
    for (uint64_t i = 0; i < region_bytes; ++i)
        region_base[i] = 0;

    /* Free list: 0 -> 1 -> ... -> num-1. */
    for (uint16_t i = 0; i < num; ++i) {
        vq->desc[i].next = (uint16_t)(i + 1);
    }
    vq->free_head = 0;
    vq->num_free = num;
    vq->last_used_idx = 0;
    vq->notify = 0;
    vq->notify_user = 0;
    return 0;
}

/* Install the doorbell vring_kick() calls; vring_layout() clears it, so this
 * must be re-done after every layout. A queue with no notify callback still
 * publishes correctly, but the device is never told. */
static inline void vring_set_notify(vring_t* vq, void (*notify)(void* user), void* user) {
    vq->notify = notify;
    vq->notify_user = user;
}

/* Allocate one descriptor for a buffer at device address buf_phys. flags is a
 * combination of VRING_DESC_F_*, with VRING_DESC_F_NEXT masked off. Returns the
 * descriptor index, or -1 if the free list is empty. For a request that spans
 * several buffers -- a virtio-blk request is a header, a data buffer and a
 * status byte -- use vring_alloc_chain() instead.
 *
 * The returned index is the driver's until it is published; the buffer it
 * names must stay valid and untouched from vring_publish() until the matching
 * vring_get_used(). Running out of descriptors is an ordinary backpressure
 * signal, not an error: retry after reclaiming completions. */
static inline int32_t vring_alloc_desc(vring_t* vq, uint64_t buf_phys, uint32_t len,
                                       uint16_t flags) {
    if (vq->num_free == 0)
        return -1;
    uint16_t head = vq->free_head;
    vq->free_head = vq->desc[head].next;
    vq->num_free--;
    vq->desc[head].addr = buf_phys;
    vq->desc[head].len = len;
    vq->desc[head].flags = (uint16_t)(flags & ~VRING_DESC_F_NEXT);
    vq->desc[head].next = 0;
    return (int32_t)head;
}

/* Return a descriptor (chain head) to the free list, making it available to the
 * next vring_alloc_desc(). Only legal once the device has reported the
 * descriptor through vring_get_used(), or before it was ever published. An
 * out-of-range index is ignored; a double free is not detected and corrupts the
 * free list. */
static inline void vring_free_desc(vring_t* vq, uint16_t head) {
    if (head >= vq->num)
        return;
    vq->desc[head].next = vq->free_head;
    vq->free_head = head;
    vq->num_free++;
}

/* One buffer of a descriptor chain. `flags` carries VRING_DESC_F_WRITE when the
 * DEVICE writes the buffer and the driver reads it after completion, and 0 when
 * the driver filled it for the device to read; VRING_DESC_F_NEXT is set by
 * vring_alloc_chain() and must not be passed in. */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
} vring_buf_t;

/* Longest chain vring_alloc_chain() builds and vring_free_chain() walks. The
 * bound is what keeps a corrupted `next` link from looping the free walk
 * forever; virtio-blk's longest chain is three. */
#define VRING_MAX_CHAIN 8u

/* Allocate one descriptor per entry of bufs[0..count) and link them into a
 * single chain, returning the head index, or -1 when count is 0 or above
 * VRING_MAX_CHAIN, or the free list cannot cover it. A short free list is
 * ordinary backpressure, not an error: retry after reclaiming completions.
 *
 * The chain is the driver's until vring_publish(); every buffer it names must
 * stay valid and untouched from then until the matching vring_get_used(). Free
 * it with vring_free_chain(), never vring_free_desc() -- the latter returns only
 * the head and leaks the rest of the chain. */
static inline int32_t vring_alloc_chain(vring_t* vq, const vring_buf_t* bufs, uint16_t count) {
    if (!bufs || count == 0u || count > VRING_MAX_CHAIN || vq->num_free < count)
        return -1;

    uint16_t head = vq->free_head;
    uint16_t current = head;
    for (uint16_t i = 0; i < count; ++i) {
        /* Read the free-list link before overwriting it: for a non-final entry
         * it doubles as the chain link, and for the final one it is where the
         * free list resumes. */
        uint16_t next = vq->desc[current].next;
        uint16_t flags = (uint16_t)(bufs[i].flags & ~VRING_DESC_F_NEXT);
        vq->desc[current].addr = bufs[i].addr;
        vq->desc[current].len = bufs[i].len;
        if (i + 1u < count) {
            flags |= VRING_DESC_F_NEXT;
            vq->desc[current].next = next;
        } else {
            vq->desc[current].next = 0;
        }
        vq->desc[current].flags = flags;
        current = next;
    }
    /* `current` is the first descriptor the chain did not consume. The final
     * chained descriptor's `next` was cleared above, so the free list can no
     * longer be walked back into the chain. */
    vq->free_head = current;
    vq->num_free = (uint16_t)(vq->num_free - count);
    return (int32_t)head;
}

/* Return a whole chain to the free list by walking its VRING_DESC_F_NEXT links.
 * Legal only once the device has reported the head through vring_get_used(), or
 * before the chain was ever published. An out-of-range head is ignored, and a
 * link that points outside the table ends the walk: the descriptors beyond it
 * stay out of the free list, which costs queue capacity but never corrupts it. */
static inline void vring_free_chain(vring_t* vq, uint16_t head) {
    if (head >= vq->num)
        return;
    uint16_t current = head;
    for (uint16_t walked = 0; walked < VRING_MAX_CHAIN; ++walked) {
        uint16_t flags = vq->desc[current].flags;
        uint16_t next = vq->desc[current].next;
        vq->desc[current].next = vq->free_head;
        vq->free_head = current;
        vq->num_free++;
        if ((flags & VRING_DESC_F_NEXT) == 0u || next >= vq->num)
            return;
        current = next;
    }
}

/* Publish a prepared descriptor to the available ring so the device can consume
 * it. Does not ring the doorbell — batch several publishes, then vring_kick().
 * Driver side only: avail_idx has a single producer, so the plain read-modify-
 * write below is safe as long as one context owns the queue. */
static inline void vring_publish(vring_t* vq, uint16_t desc_head) {
    uint16_t avail = *vq->avail_idx;
    vq->avail_ring[avail % vq->num] = desc_head;
    vring_mb(); /* descriptor + ring entry visible before idx bump */
    *vq->avail_idx = (uint16_t)(avail + 1);
}

/* Ring the device's doorbell for everything published since the last kick. */
static inline void vring_kick(vring_t* vq) {
    vring_mb(); /* avail_idx visible to device before notify */
    if (vq->notify)
        vq->notify(vq->notify_user);
}

/* Nonzero if the device has completed buffers that are not yet consumed.
 * It compares the device's used_idx against last_used_idx, so it answers "is
 * the used ring ahead of us" - not whether any particular descriptor is done.
 * It is also the acquire fence for the used ring: a used-ring element may only
 * be read after this has returned nonzero, which is why vring_get_used() calls
 * it rather than re-reading used_idx itself. */
static inline int32_t vring_has_used(vring_t* vq) {
    vring_mb(); /* observe the device's latest used_idx */
    return (int32_t)(*vq->used_idx != vq->last_used_idx);
}

/* Consume the next completed buffer. Returns its head descriptor index and, via
 * out_len, the byte count the device reported; returns -1 when nothing is
 * pending. The descriptor is NOT auto-freed — the caller reads the buffer, then
 * vring_free_desc()s the head. Consumer-side validation: a used-ring id outside
 * [0, num) is skipped (returns -1, last_used_idx still advances) rather than
 * trusted. The element must be read after the used_idx that publishes it; the
 * fence inside vring_has_used() is what keeps the compiler from hoisting either
 * load above it, and x86 does not reorder the two loads. */
static inline int32_t vring_get_used(vring_t* vq, uint32_t* out_len) {
    if (!vring_has_used(vq))
        return -1;
    uint16_t slot = (uint16_t)(vq->last_used_idx % vq->num);
    uint32_t id = vq->used_ring[slot].id;
    uint32_t len = vq->used_ring[slot].len;
    if (id >= vq->num) {
        /* Corrupt/malicious element: skip it without dereferencing a bad desc. */
        vq->last_used_idx++;
        return -1;
    }
    vq->last_used_idx++;
    if (out_len)
        *out_len = len;
    return (int32_t)id;
}

#ifdef __cplusplus
}
#endif

#endif /* WASMOS_VRING_H */
