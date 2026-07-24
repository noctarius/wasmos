#include "slab.h"
#include "sync/spinlock.h"
#include "physmem.h"
#include "paging.h"
#include <stdint.h>

/*
 * Minimal fixed-size slab allocator for small kernel objects. This is optional
 * infrastructure for now; existing static-table paths remain the default.
 *
 * Each size class starts on a fixed static buffer and, once that is exhausted,
 * grows on demand: a fresh 4 KiB frame is allocated from the physical page
 * allocator (constrained to the kernel higher-half direct-map window so it is
 * reachable at phys | KERNEL_HIGHER_HALF_BASE) and carved into chunks that are
 * pushed onto the class free list. Growth is therefore bounded only by physical
 * memory, never by a compile-time count.
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
