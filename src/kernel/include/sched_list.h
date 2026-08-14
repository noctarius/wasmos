#ifndef WASMOS_SCHED_LIST_H
#define WASMOS_SCHED_LIST_H

#include <stddef.h>

/*
 * Minimal intrusive doubly-linked list for the scheduler.  The head is a
 * sentinel node: an empty list has head.next == head.prev == &head.  Only
 * the operations used by the in-kernel scheduler are provided.
 */

/* Embedded in the object being queued; the containing struct is recovered with
 * list_entry.  All-zero storage is NOT a valid node — the pointers must be self-linked by
 * list_head_init before any other operation, on both a list head and a node that is going
 * to be added or deleted. */
typedef struct list_head {
    struct list_head* next;
    struct list_head* prev;
} list_head_t;

/* Self-link a node, making it a valid empty list head and a valid unlinked node.  Calling
 * it on a node that is currently in a list corrupts that list, since the neighbours keep
 * pointing at it. */
static inline void list_head_init(list_head_t* head) {
    head->next = head;
    head->prev = head;
}

static inline int list_head_empty(const list_head_t* head) {
    return head->next == head;
}

/* Append `node` before the sentinel, i.e. at the end of the list, giving FIFO order when
 * paired with list_first_entry.  `node` must not already be in a list. */
static inline void list_head_add_tail(list_head_t* head, list_head_t* node) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

/* Unlink `node` from whatever list it is in and re-self-link it, so a second delete is
 * harmless and the node is immediately re-addable.  A node that was never initialised has
 * garbage neighbour pointers and this writes through them. */
static inline void list_head_del(list_head_t* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

/* First element in the list — caller must ensure non-empty. */
#define list_first_entry(head, type, member)                                                       \
    ((type*)((char*)((head)->next) - offsetof(type, member)))

/* Get the struct containing a known embedded list_head_t node. */
#define list_entry(node, type, member) ((type*)((char*)(node) - offsetof(type, member)))

/* Iterate safely: safe against removal of the current entry mid-loop. */
#define list_for_each_safe(pos, tmp, head)                                                         \
    for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head);                               \
         (pos) = (tmp), (tmp) = (pos)->next)

#endif /* WASMOS_SCHED_LIST_H */
