#ifndef WASMOS_LIST_H
#define WASMOS_LIST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LIST_IMPL_LINKED = 0,
    LIST_IMPL_ARRAY_CHUNK = 1
} list_impl_t;

typedef struct list_ops list_ops_t;

typedef struct {
    uint32_t elem_size;
    list_impl_t impl;
    uint32_t config_value;
    const list_ops_t *ops;
    void *impl_state;
} list_t;

typedef struct {
    list_t *list;
    void *state0;
    void *state1;
    uint32_t index;
} list_iter_t;

int list_init(list_t *list, uint32_t elem_size, list_impl_t impl, uint32_t array_chunk_capacity);
void list_destroy(list_t *list);
void *list_alloc(list_t *list);
int list_remove(list_t *list, void *elem);
void *list_first(list_t *list, list_iter_t *iter);
void *list_next(list_iter_t *iter);

#endif
