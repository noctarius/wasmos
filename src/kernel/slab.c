#include "slab.h"
#include "sync/spinlock.h"
#include "physmem.h"
#include "paging.h"
#include <stdint.h>

/*
 * Minimal fixed-size slab allocator for small kernel objects.
 *
 * Each size class starts on a fixed static buffer and, once that is exhausted,
 * grows on demand: a fresh 4 KiB frame is allocated from the physical page
 * allocator (constrained to the kernel higher-half direct-map window so it is
 * reachable at phys | KERNEL_HIGHER_HALF_BASE) and carved into chunks that are
 * pushed onto the class free list. No compile-time count caps the growth; the
 * limit is free physical memory inside that window.
 *
 * Grown frames are never returned to the page allocator: a freed chunk goes back
 * on its class free list and the frame stays owned by the class for good.
 */

/* Must not exceed paging.c's higher-half direct-map window: frames above it are
 * not mapped at phys | KERNEL_HIGHER_HALF_BASE and could not be dereferenced. */
#define SLAB_GROW_WINDOW_BYTES (512u * 1024u * 1024u)
#define SLAB_GROW_FRAME_BYTES 4096u

#define SLAB_CLASS_COUNT 3u

typedef struct slab_node {
    struct slab_node* next;
} slab_node_t;

typedef struct {
    uint16_t magic;
    uint8_t class_index;
    uint8_t reserved;
} slab_header_t;

typedef struct {
    uint16_t chunk_size;
    uint16_t chunk_count;
    uint8_t* buffer;
    slab_node_t* free_list;
} slab_class_t;

#define SLAB_MAGIC 0x51ABu

static uint8_t g_slab_buf_32[32u * 128u];
static uint8_t g_slab_buf_64[64u * 128u];
static uint8_t g_slab_buf_128[128u * 96u];

static slab_class_t g_classes[SLAB_CLASS_COUNT] = {
    {32u, 128u, g_slab_buf_32, 0},
    {64u, 128u, g_slab_buf_64, 0},
    {128u, 96u, g_slab_buf_128, 0},
};
static ksync_spinlock_t g_slab_lock;

/* Initialises the lock and threads every chunk of each class's static buffer
 * onto that class's free list.  Must run before kalloc_small can return
 * anything; kalloc_small before it finds empty free lists and, until the page
 * allocator is up, cannot grow them either.
 *
 * Not idempotent: a second call rebuilds the free lists from the static buffers
 * alone, which both re-offers chunks that are currently allocated and forgets
 * every frame the classes have grown into. */
void slab_init(void) {
    ksync_spinlock_init(&g_slab_lock);
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; ++c) {
        slab_class_t* klass = &g_classes[c];
        klass->free_list = 0;
        for (uint32_t i = 0; i < klass->chunk_count; ++i) {
            uint8_t* chunk = klass->buffer + ((uint32_t)klass->chunk_size * i);
            slab_node_t* node = ptr_cast(slab_node_t, chunk);
            node->next = klass->free_list;
            klass->free_list = node;
        }
    }
}

static int find_class(size_t total_size) {
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; ++c) {
        if (total_size <= g_classes[c].chunk_size) {
            return (int)c;
        }
    }
    return -1;
}

/* Grow a size class by one frame's worth of chunks. Caller holds g_slab_lock.
 * Returns 0 if at least one chunk was added, -1 if no backing was available
 * (e.g. the page allocator is not yet initialised during very early boot, in
 * which case the caller falls back to reporting exhaustion). */
static int slab_class_grow(slab_class_t* klass) {
    uint64_t phys = pfa_alloc_pages_below(1, SLAB_GROW_WINDOW_BYTES);
    if (!phys) {
        return -1;
    }
    uint8_t* frame = (uint8_t*)(uintptr_t)(phys | KERNEL_HIGHER_HALF_BASE);
    uint32_t count = SLAB_GROW_FRAME_BYTES / klass->chunk_size;
    for (uint32_t i = 0; i < count; ++i) {
        slab_node_t* node = ptr_cast(slab_node_t, frame + (uint32_t)klass->chunk_size * i);
        node->next = klass->free_list;
        klass->free_list = node;
    }
    return count > 0 ? 0 : -1;
}

/* Allocates from the smallest class whose chunk holds size plus the 4-byte
 * slab_header_t that precedes the returned pointer.  The largest class is 128
 * bytes, so a request above 124 bytes has no class and returns 0 immediately,
 * without touching the lock.
 *
 * 0 is also returned when the class free list is empty and slab_class_grow
 * cannot obtain a frame — during very early boot, or once the higher-half window
 * is out of memory.  Takes g_slab_lock for the list manipulation.
 *
 * The block is uninitialised.  It must be released with kfree_small, which reads
 * the header immediately below the pointer. */
void* kalloc_small(size_t size) {
    size_t total = size + sizeof(slab_header_t);
    int c = find_class(total);
    if (c < 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_slab_lock);
    slab_class_t* klass = &g_classes[c];
    slab_node_t* node = klass->free_list;
    if (!node && slab_class_grow(klass) == 0) {
        node = klass->free_list;
    }
    if (!node) {
        ksync_spinlock_unlock(&g_slab_lock);
        return 0;
    }
    klass->free_list = node->next;
    slab_header_t* hdr = ptr_cast(slab_header_t, node);
    hdr->magic = SLAB_MAGIC;
    hdr->class_index = (uint8_t)c;
    hdr->reserved = 0;
    ksync_spinlock_unlock(&g_slab_lock);
    return ptr_cast(void, (hdr + 1));
}

/* Returns a chunk to its class free list, identified by the header just below
 * ptr.  NULL is ignored, and a pointer whose header carries the wrong magic or
 * an out-of-range class index is ignored too — so a foreign pointer is refused
 * rather than corrupting a free list.
 *
 * Freeing clears the magic, which makes an immediate double free a silent no-op
 * as well.  That only holds until the chunk is handed out again; there is no
 * detection after that.  The frame itself is never returned to the page
 * allocator. */
void kfree_small(void* ptr) {
    if (!ptr) {
        return;
    }
    slab_header_t* hdr = ((slab_header_t*)ptr) - 1;
    if (hdr->magic != SLAB_MAGIC || hdr->class_index >= SLAB_CLASS_COUNT) {
        return;
    }
    ksync_spinlock_lock(&g_slab_lock);
    slab_class_t* klass = &g_classes[hdr->class_index];
    hdr->magic = 0;
    slab_node_t* node = ptr_cast(slab_node_t, hdr);
    node->next = klass->free_list;
    klass->free_list = node;
    ksync_spinlock_unlock(&g_slab_lock);
}
