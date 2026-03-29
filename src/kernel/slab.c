#include "slab.h"
#include <stdint.h>

/*
 * Minimal fixed-size slab allocator for small kernel objects. This is optional
 * infrastructure for now; existing static-table paths remain the default.
 */

#define SLAB_CLASS_COUNT 3u

typedef struct slab_node {
    struct slab_node *next;
} slab_node_t;

typedef struct {
    uint16_t magic;
    uint8_t class_index;
    uint8_t reserved;
} slab_header_t;

typedef struct {
    uint16_t chunk_size;
    uint16_t chunk_count;
    uint8_t *buffer;
    slab_node_t *free_list;
} slab_class_t;

#define SLAB_MAGIC 0x51ABu

static uint8_t g_slab_buf_32[32u * 128u];
static uint8_t g_slab_buf_64[64u * 128u];
static uint8_t g_slab_buf_128[128u * 96u];

static slab_class_t g_classes[SLAB_CLASS_COUNT] = {
    { 32u, 128u, g_slab_buf_32, 0 },
    { 64u, 128u, g_slab_buf_64, 0 },
    { 128u, 96u, g_slab_buf_128, 0 },
};

void
slab_init(void)
{
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; ++c) {
        slab_class_t *klass = &g_classes[c];
        klass->free_list = 0;
        for (uint32_t i = 0; i < klass->chunk_count; ++i) {
            uint8_t *chunk = klass->buffer + ((uint32_t)klass->chunk_size * i);
            slab_node_t *node = (slab_node_t *)(uintptr_t)chunk;
            node->next = klass->free_list;
            klass->free_list = node;
        }
    }
}

static int
find_class(size_t total_size)
{
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; ++c) {
        if (total_size <= g_classes[c].chunk_size) {
            return (int)c;
        }
    }
    return -1;
}

void *
kalloc_small(size_t size)
{
    size_t total = size + sizeof(slab_header_t);
    int c = find_class(total);
    if (c < 0) {
        return 0;
    }
    slab_class_t *klass = &g_classes[c];
    slab_node_t *node = klass->free_list;
    if (!node) {
        return 0;
    }
    klass->free_list = node->next;

    slab_header_t *hdr = (slab_header_t *)(uintptr_t)node;
    hdr->magic = SLAB_MAGIC;
    hdr->class_index = (uint8_t)c;
    hdr->reserved = 0;
    return (void *)(uintptr_t)(hdr + 1);
}

void
kfree_small(void *ptr)
{
    if (!ptr) {
        return;
    }
    slab_header_t *hdr = ((slab_header_t *)ptr) - 1;
    if (hdr->magic != SLAB_MAGIC || hdr->class_index >= SLAB_CLASS_COUNT) {
        return;
    }
    slab_class_t *klass = &g_classes[hdr->class_index];
    hdr->magic = 0;
    slab_node_t *node = (slab_node_t *)(uintptr_t)hdr;
    node->next = klass->free_list;
    klass->free_list = node;
}
