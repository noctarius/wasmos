/* list_array_chunk.c - Chunk-array list backend.
 * Items are packed into fixed-size chunks (each holding up to
 * LIST_ARRAY_CHUNK_SLOTS elements) to reduce allocator pressure and improve
 * cache locality over the linked-list backend for large collections. */
#include "list_internal.h"
#include "string.h"

typedef struct list_array_chunk {
    struct list_array_chunk *next;
    uint32_t capacity;
    uint8_t slots[];
} list_array_chunk_t;

typedef struct {
    uint32_t chunk_capacity;
    list_array_chunk_t *head;
} list_array_chunk_state_t;

static void *list_array_chunk_next(list_iter_t *iter);

#define LIST_ARRAY_SLOT_ALIGN 8u

static uint32_t
list_array_slot_header_size(void)
{
    return LIST_ARRAY_SLOT_ALIGN;
}

static uint32_t
list_array_stride(const list_t *list)
{
    uint32_t raw = list_array_slot_header_size() + list->elem_size;
    return (raw + (LIST_ARRAY_SLOT_ALIGN - 1u)) & ~(LIST_ARRAY_SLOT_ALIGN - 1u);
}

static uint8_t *
list_array_slot_addr(list_array_chunk_t *chunk, uint32_t stride, uint32_t index)
{
    return chunk->slots + (uint64_t)stride * (uint64_t)index;
}

static void
list_array_chunk_destroy(list_t *list)
{
    list_array_chunk_state_t *state = (list_array_chunk_state_t *)list->impl_state;
    list_array_chunk_t *chunk = state ? state->head : 0;
    while (chunk) {
        list_array_chunk_t *next = chunk->next;
        list_free_mem(chunk);
        chunk = next;
    }
    list_free_mem(state);
}

static void *
list_array_chunk_alloc(list_t *list)
{
    list_array_chunk_state_t *state = (list_array_chunk_state_t *)list->impl_state;
    uint32_t stride = list_array_stride(list);
    list_array_chunk_t *chunk = state ? state->head : 0;
    if (!state || state->chunk_capacity == 0) {
        return 0;
    }
    while (chunk) {
        for (uint32_t i = 0; i < chunk->capacity; ++i) {
            uint8_t *slot = list_array_slot_addr(chunk, stride, i);
            if (slot[0] == 0) {
                slot[0] = 1;
                memset(slot + list_array_slot_header_size(), 0, list->elem_size);
                return slot + list_array_slot_header_size();
            }
        }
        chunk = chunk->next;
    }

    uint64_t alloc_size = sizeof(list_array_chunk_t) +
                          (uint64_t)state->chunk_capacity * (uint64_t)stride;
    chunk = (list_array_chunk_t *)list_alloc_mem((size_t)alloc_size);
    if (!chunk) {
        return 0;
    }
    memset(chunk, 0, (size_t)alloc_size);
    chunk->capacity = state->chunk_capacity;
    chunk->next = state->head;
    state->head = chunk;
    chunk->slots[0] = 1;
    return chunk->slots + list_array_slot_header_size();
}

static int
list_array_chunk_remove(list_t *list, void *elem)
{
    list_array_chunk_state_t *state = (list_array_chunk_state_t *)list->impl_state;
    uint32_t stride = list_array_stride(list);
    list_array_chunk_t *chunk = state ? state->head : 0;
    while (chunk) {
        for (uint32_t i = 0; i < chunk->capacity; ++i) {
            uint8_t *slot = list_array_slot_addr(chunk, stride, i);
            if ((void *)(slot + list_array_slot_header_size()) != elem) {
                continue;
            }
            if (slot[0] == 0) {
                return -1;
            }
            slot[0] = 0;
            memset(slot + list_array_slot_header_size(), 0, list->elem_size);
            return 0;
        }
        chunk = chunk->next;
    }
    return -1;
}

static void *
list_array_chunk_first(list_t *list, list_iter_t *iter)
{
    list_array_chunk_state_t *state = (list_array_chunk_state_t *)list->impl_state;
    iter->state0 = state ? state->head : 0;
    iter->index = 0;
    return list_array_chunk_next(iter);
}

static void *
list_array_chunk_next(list_iter_t *iter)
{
    list_t *list = iter->list;
    list_array_chunk_t *chunk = (list_array_chunk_t *)iter->state0;
    uint32_t stride = list_array_stride(list);

    while (chunk) {
        while (iter->index < chunk->capacity) {
            uint8_t *slot = list_array_slot_addr(chunk, stride, iter->index++);
            if (slot[0] != 0) {
                iter->state0 = chunk;
                return slot + list_array_slot_header_size();
            }
        }
        chunk = chunk->next;
        iter->state0 = chunk;
        iter->index = 0;
    }
    return 0;
}

static const list_ops_t g_list_array_chunk_ops = {
    .destroy = list_array_chunk_destroy,
    .alloc = list_array_chunk_alloc,
    .remove = list_array_chunk_remove,
    .first = list_array_chunk_first,
    .next = list_array_chunk_next
};

int
list_array_chunk_impl_init(list_t *list)
{
    list_array_chunk_state_t *state = 0;
    if (!list || list->elem_size == 0 || list->config_value == 0) {
        return -1;
    }
    state = (list_array_chunk_state_t *)list_alloc_mem(sizeof(list_array_chunk_state_t));
    if (!state) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    state->chunk_capacity = list->config_value;
    list->impl_state = state;
    list->ops = &g_list_array_chunk_ops;
    return 0;
}
