#include "list.h"
#include "stdlib.h"
#include "string.h"

static uint32_t
list_array_stride(const list_t *list)
{
    return 1u + list->elem_size;
}

static uint8_t *
list_array_slot_addr(list_array_chunk_t *chunk, uint32_t stride, uint32_t index)
{
    return chunk->slots + (uint64_t)stride * (uint64_t)index;
}

int
list_init(list_t *list, uint32_t elem_size, list_impl_t impl, uint32_t array_chunk_capacity)
{
    if (!list || elem_size == 0) {
        return -1;
    }
    if (impl != LIST_IMPL_LINKED && impl != LIST_IMPL_ARRAY_CHUNK) {
        return -1;
    }
    if (impl == LIST_IMPL_ARRAY_CHUNK && array_chunk_capacity == 0) {
        return -1;
    }
    memset(list, 0, sizeof(*list));
    list->elem_size = elem_size;
    list->impl = impl;
    list->array_chunk_capacity = array_chunk_capacity;
    return 0;
}

void
list_destroy(list_t *list)
{
    if (!list) {
        return;
    }
    if (list->impl == LIST_IMPL_LINKED) {
        list_linked_node_t *node = list->linked_head;
        while (node) {
            list_linked_node_t *next = node->next;
            free(node);
            node = next;
        }
    } else {
        list_array_chunk_t *chunk = list->array_head;
        while (chunk) {
            list_array_chunk_t *next = chunk->next;
            free(chunk);
            chunk = next;
        }
    }
    list->linked_head = 0;
    list->array_head = 0;
}

void *
list_alloc(list_t *list)
{
    if (!list || list->elem_size == 0) {
        return 0;
    }
    if (list->impl == LIST_IMPL_LINKED) {
        uint64_t size = sizeof(list_linked_node_t) + list->elem_size;
        list_linked_node_t *node = (list_linked_node_t *)malloc((size_t)size);
        if (!node) {
            return 0;
        }
        memset(node, 0, (size_t)size);
        node->next = list->linked_head;
        list->linked_head = node;
        return node->payload;
    }

    uint32_t stride = list_array_stride(list);
    list_array_chunk_t *chunk = list->array_head;
    while (chunk) {
        for (uint32_t i = 0; i < chunk->capacity; ++i) {
            uint8_t *slot = list_array_slot_addr(chunk, stride, i);
            if (slot[0] == 0) {
                slot[0] = 1;
                memset(slot + 1, 0, list->elem_size);
                return slot + 1;
            }
        }
        chunk = chunk->next;
    }

    uint64_t alloc_size = sizeof(list_array_chunk_t) +
                          (uint64_t)list->array_chunk_capacity * (uint64_t)stride;
    chunk = (list_array_chunk_t *)malloc((size_t)alloc_size);
    if (!chunk) {
        return 0;
    }
    memset(chunk, 0, (size_t)alloc_size);
    chunk->capacity = list->array_chunk_capacity;
    chunk->next = list->array_head;
    list->array_head = chunk;
    chunk->slots[0] = 1;
    return chunk->slots + 1;
}

int
list_remove(list_t *list, void *elem)
{
    if (!list || !elem) {
        return -1;
    }
    if (list->impl == LIST_IMPL_LINKED) {
        list_linked_node_t *prev = 0;
        list_linked_node_t *node = list->linked_head;
        while (node) {
            if ((void *)node->payload == elem) {
                if (prev) {
                    prev->next = node->next;
                } else {
                    list->linked_head = node->next;
                }
                free(node);
                return 0;
            }
            prev = node;
            node = node->next;
        }
        return -1;
    }

    uint32_t stride = list_array_stride(list);
    list_array_chunk_t *chunk = list->array_head;
    while (chunk) {
        for (uint32_t i = 0; i < chunk->capacity; ++i) {
            uint8_t *slot = list_array_slot_addr(chunk, stride, i);
            if ((void *)(slot + 1) != elem) {
                continue;
            }
            if (slot[0] == 0) {
                return -1;
            }
            slot[0] = 0;
            memset(slot + 1, 0, list->elem_size);
            return 0;
        }
        chunk = chunk->next;
    }
    return -1;
}

void *
list_first(list_t *list, list_iter_t *iter)
{
    if (!list || !iter) {
        return 0;
    }
    memset(iter, 0, sizeof(*iter));
    iter->list = list;
    if (list->impl == LIST_IMPL_LINKED) {
        iter->linked_node = list->linked_head;
        if (!iter->linked_node) {
            return 0;
        }
        return iter->linked_node->payload;
    }

    iter->array_chunk = list->array_head;
    iter->array_index = 0;
    return list_next(iter);
}

void *
list_next(list_iter_t *iter)
{
    if (!iter || !iter->list) {
        return 0;
    }
    if (iter->list->impl == LIST_IMPL_LINKED) {
        if (!iter->linked_node) {
            return 0;
        }
        iter->linked_node = iter->linked_node->next;
        if (!iter->linked_node) {
            return 0;
        }
        return iter->linked_node->payload;
    }

    uint32_t stride = list_array_stride(iter->list);
    while (iter->array_chunk) {
        while (iter->array_index < iter->array_chunk->capacity) {
            uint8_t *slot = list_array_slot_addr(iter->array_chunk, stride, iter->array_index++);
            if (slot[0] != 0) {
                return slot + 1;
            }
        }
        iter->array_chunk = iter->array_chunk->next;
        iter->array_index = 0;
    }
    return 0;
}
