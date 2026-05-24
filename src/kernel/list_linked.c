#include "list_internal.h"
#include "string.h"

typedef struct list_linked_node {
    struct list_linked_node *next;
    uint8_t payload[];
} list_linked_node_t;

typedef struct {
    list_linked_node_t *head;
} list_linked_state_t;

static void
list_linked_destroy(list_t *list)
{
    list_linked_state_t *state = (list_linked_state_t *)list->impl_state;
    list_linked_node_t *node = state ? state->head : 0;
    while (node) {
        list_linked_node_t *next = node->next;
        list_free_mem(node);
        node = next;
    }
    list_free_mem(state);
}

static void *
list_linked_alloc(list_t *list)
{
    list_linked_state_t *state = (list_linked_state_t *)list->impl_state;
    uint64_t size = sizeof(list_linked_node_t) + list->elem_size;
    list_linked_node_t *node = (list_linked_node_t *)list_alloc_mem((size_t)size);
    if (!state || !node) {
        return 0;
    }
    memset(node, 0, (size_t)size);
    node->next = state->head;
    state->head = node;
    return node->payload;
}

static int
list_linked_remove(list_t *list, void *elem)
{
    list_linked_state_t *state = (list_linked_state_t *)list->impl_state;
    list_linked_node_t *prev = 0;
    list_linked_node_t *node = state ? state->head : 0;
    while (node) {
        if ((void *)node->payload == elem) {
            if (prev) {
                prev->next = node->next;
            } else {
                state->head = node->next;
            }
            list_free_mem(node);
            return 0;
        }
        prev = node;
        node = node->next;
    }
    return -1;
}

static void *
list_linked_first(list_t *list, list_iter_t *iter)
{
    list_linked_state_t *state = (list_linked_state_t *)list->impl_state;
    if (!state || !state->head) {
        return 0;
    }
    iter->state0 = state->head;
    return ((list_linked_node_t *)iter->state0)->payload;
}

static void *
list_linked_next(list_iter_t *iter)
{
    list_linked_node_t *node = (list_linked_node_t *)iter->state0;
    if (!node) {
        return 0;
    }
    node = node->next;
    iter->state0 = node;
    return node ? node->payload : 0;
}

static const list_ops_t g_list_linked_ops = {
    .destroy = list_linked_destroy,
    .alloc = list_linked_alloc,
    .remove = list_linked_remove,
    .first = list_linked_first,
    .next = list_linked_next
};

int
list_linked_impl_init(list_t *list)
{
    list_linked_state_t *state = 0;
    if (!list || list->elem_size == 0) {
        return -1;
    }
    state = (list_linked_state_t *)list_alloc_mem(sizeof(list_linked_state_t));
    if (!state) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    list->impl_state = state;
    list->ops = &g_list_linked_ops;
    return 0;
}
