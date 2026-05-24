#ifndef WASMOS_LIST_INTERNAL_H
#define WASMOS_LIST_INTERNAL_H

#include "list.h"

typedef struct list_ops {
    void (*destroy)(list_t *list);
    void *(*alloc)(list_t *list);
    int (*remove)(list_t *list, void *elem);
    void *(*first)(list_t *list, list_iter_t *iter);
    void *(*next)(list_iter_t *iter);
} list_ops_t;

int list_linked_impl_init(list_t *list);
int list_array_chunk_impl_init(list_t *list);
void *list_alloc_mem(size_t size);
void list_free_mem(void *ptr);

#endif
