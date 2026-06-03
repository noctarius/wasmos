#include "list.h"
#include "list_internal.h"
#include "stdlib.h"
#include "string.h"

/* Early-boot fallback arena for list metadata/chunks before heap allocators
 * are fully available. */
#define LIST_EARLY_ARENA_BYTES (256u * 1024u)
static uint8_t g_list_early_arena[LIST_EARLY_ARENA_BYTES];
static uint32_t g_list_early_arena_off;

static void *
list_early_alloc(size_t size)
{
    uintptr_t base = (uintptr_t)&g_list_early_arena[0];
    if (size == 0) {
        return 0;
    }
    for (;;) {
        uint32_t observed = g_list_early_arena_off;
        uintptr_t cursor = base + (uintptr_t)observed;
        uintptr_t aligned = (cursor + 7u) & ~(uintptr_t)0x7u;
        uintptr_t end = aligned + (uintptr_t)size;
        uint32_t next = 0;
        if (end < aligned || end > (base + (uintptr_t)LIST_EARLY_ARENA_BYTES)) {
            return 0;
        }
        next = (uint32_t)(end - base);
        if (__sync_bool_compare_and_swap(&g_list_early_arena_off, observed, next)) {
            return (void *)aligned;
        }
    }
}

void *
list_alloc_mem(size_t size)
{
    void *ptr = 0;
    if (size == 0) {
        return 0;
    }
    ptr = malloc(size);
    if (ptr) {
        return ptr;
    }
    return list_early_alloc(size);
}

void
list_free_mem(void *ptr)
{
    uintptr_t start = (uintptr_t)&g_list_early_arena[0];
    uintptr_t end = start + (uintptr_t)LIST_EARLY_ARENA_BYTES;
    uintptr_t p = (uintptr_t)ptr;
    if (!ptr) {
        return;
    }
    if (p >= start && p < end) {
        /* TODO(list-allocator): add reclaim for early-arena allocations if
         * repeated early remove/destroy paths become hot. */
        return;
    }
    free(ptr);
}

int
list_init(list_t *list, uint32_t elem_size, list_impl_t impl, uint32_t config_value)
{
    if (!list || elem_size == 0) {
        return -1;
    }
    memset(list, 0, sizeof(*list));
    list->elem_size = elem_size;
    list->impl = impl;
    list->config_value = config_value;

    if (impl == LIST_IMPL_LINKED) {
        return list_linked_impl_init(list);
    }
    if (impl == LIST_IMPL_ARRAY_CHUNK) {
        return list_array_chunk_impl_init(list);
    }
    memset(list, 0, sizeof(*list));
    return -1;
}

void
list_destroy(list_t *list)
{
    if (!list || !list->ops || !list->ops->destroy) {
        return;
    }
    list->ops->destroy(list);
    list->ops = 0;
    list->impl_state = 0;
}

void *
list_alloc(list_t *list)
{
    if (!list || !list->ops || !list->ops->alloc) {
        return 0;
    }
    return list->ops->alloc(list);
}

int
list_remove(list_t *list, void *elem)
{
    if (!list || !list->ops || !list->ops->remove) {
        return -1;
    }
    return list->ops->remove(list, elem);
}

void *
list_first(list_t *list, list_iter_t *iter)
{
    if (!list || !iter || !list->ops || !list->ops->first) {
        return 0;
    }
    memset(iter, 0, sizeof(*iter));
    iter->list = list;
    return list->ops->first(list, iter);
}

void *
list_next(list_iter_t *iter)
{
    if (!iter || !iter->list || !iter->list->ops || !iter->list->ops->next) {
        return 0;
    }
    return iter->list->ops->next(iter);
}
