#ifndef WASMOS_LIST_H
#define WASMOS_LIST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LIST_IMPL_LINKED = 0,
    LIST_IMPL_ARRAY_CHUNK = 1
} list_impl_t;

typedef struct list_linked_node {
    struct list_linked_node *next;
    uint8_t payload[];
} list_linked_node_t;

typedef struct list_array_chunk {
    struct list_array_chunk *next;
    uint32_t capacity;
    uint8_t slots[];
} list_array_chunk_t;

typedef struct {
    uint32_t elem_size;
    list_impl_t impl;
    uint32_t array_chunk_capacity;
    list_linked_node_t *linked_head;
    list_array_chunk_t *array_head;
} list_t;

typedef struct {
    list_t *list;
    list_linked_node_t *linked_node;
    list_array_chunk_t *array_chunk;
    uint32_t array_index;
} list_iter_t;

int list_init(list_t *list, uint32_t elem_size, list_impl_t impl, uint32_t array_chunk_capacity);
void list_destroy(list_t *list);
void *list_alloc(list_t *list);
int list_remove(list_t *list, void *elem);
void *list_first(list_t *list, list_iter_t *iter);
void *list_next(list_iter_t *iter);

#endif
