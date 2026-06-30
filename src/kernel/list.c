/* list.c - Generic intrusive list facade.
 * Dispatches to list_linked.c (singly-linked, unbounded) or
 * list_array_chunk.c (chunk-array, cache-friendly for fixed-size items)
 * depending on the list_t type field.  Both variants share the same
 * list_iter_t cursor so callers are implementation-agnostic. */
#include "list.h"
#include "list_internal.h"
#include "string.h"

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
