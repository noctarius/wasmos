/* list_linked.c - Singly-linked list backend for list.h.
 *
 * One kmem_alloc per element: a node header plus elem_size bytes of inline
 * payload, and the payload address is what the list API hands out. Nodes are
 * pushed at the head, so list_alloc is O(1) and iteration yields elements in
 * reverse allocation order. list_remove scans for the matching payload
 * address, so it is O(n); list_destroy frees every node and the state block.
 *
 * The iterator holds the node it last returned. Removing that element frees the
 * node, so list_next on the same iterator afterwards reads freed memory; list.h
 * states the rule for both backends, which is to restart the walk after any
 * mutation. config_value is unused here, so the list is bounded only by kmem. */
#include "list_internal.h"
#include "kmem.h"
#include "string.h"

typedef struct list_linked_node {
    struct list_linked_node* next;
    uint8_t payload[];
} list_linked_node_t;

typedef struct {
    list_linked_node_t* head;
} list_linked_state_t;

static void list_linked_destroy(list_t* list) {
    list_linked_state_t* state = (list_linked_state_t*)list->impl_state;
    list_linked_node_t* node = state ? state->head : 0;
    while (node) {
        list_linked_node_t* next = node->next;
        kmem_free(node);
        node = next;
    }
    kmem_free(state);
}

static void* list_linked_alloc(list_t* list) {
    list_linked_state_t* state = (list_linked_state_t*)list->impl_state;
    if (!state) {
        return 0;
    }
    uint64_t size = sizeof(list_linked_node_t) + list->elem_size;
    list_linked_node_t* node = (list_linked_node_t*)kmem_alloc((size_t)size);
    if (!node) {
        return 0;
    }
    memset(node, 0, (size_t)size);
    node->next = state->head;
    state->head = node;
    return node->payload;
}

static int list_linked_remove(list_t* list, void* elem) {
    list_linked_state_t* state = (list_linked_state_t*)list->impl_state;
    list_linked_node_t* prev = 0;
    list_linked_node_t* node = state ? state->head : 0;
    while (node) {
        if ((void*)node->payload == elem) {
            if (prev) {
                prev->next = node->next;
            } else {
                state->head = node->next;
            }
            kmem_free(node);
            return 0;
        }
        prev = node;
        node = node->next;
    }
    return -1;
}

static void* list_linked_first(list_t* list, list_iter_t* iter) {
    list_linked_state_t* state = (list_linked_state_t*)list->impl_state;
    if (!state || !state->head) {
        return 0;
    }
    iter->state0 = state->head;
    return ((list_linked_node_t*)iter->state0)->payload;
}

static void* list_linked_next(list_iter_t* iter) {
    list_linked_node_t* node = (list_linked_node_t*)iter->state0;
    if (!node) {
        return 0;
    }
    node = node->next;
    iter->state0 = node;
    return node ? node->payload : 0;
}

static const list_ops_t g_list_linked_ops = {.destroy = list_linked_destroy,
                                             .alloc = list_linked_alloc,
                                             .remove = list_linked_remove,
                                             .first = list_linked_first,
                                             .next = list_linked_next};

/* Backend constructor called by list_init; not part of the public list.h API.
 *
 * Allocates the state block and installs the linked vtable, after which every
 * list_* facade call routes here.  list->elem_size must already be set;
 * config_value is ignored by this backend.
 *
 * Returns 0 on success and -1 for a NULL list, a zero elem_size, or a failed
 * state allocation — in which case list->ops stays 0 and the facade degrades to
 * NULL/-1 rather than faulting.  The state block is released by
 * list_destroy. */
int list_linked_impl_init(list_t* list) {
    list_linked_state_t* state = 0;
    if (!list || list->elem_size == 0) {
        return -1;
    }
    state = (list_linked_state_t*)kmem_alloc(sizeof(list_linked_state_t));
    if (!state) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    list->impl_state = state;
    list->ops = &g_list_linked_ops;
    return 0;
}
