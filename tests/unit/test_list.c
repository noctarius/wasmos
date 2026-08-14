/* test_list.c — the generic list (list.h) over both storage backends.
 *
 * list.c, list_linked.c, list_array_chunk.c and kmem.c are compiled in for real;
 * the only substitution is the slab allocator underneath kmem, which
 * tests/unit/stubs_slab.c forwards to the host heap. A kmem allocation therefore
 * fails only when the host is out of memory, so the out-of-memory arms of
 * list_init/list_alloc are not reachable from here.
 *
 * The cases pin what list.h promises and what the two backends implement
 * differently: argument validation (including the chunk capacity that only
 * LIST_IMPL_ARRAY_CHUNK requires), interior pointers that stay usable while
 * other elements come and go, refusal of a pointer the list never handed out,
 * and slot reuse across chunk growth. Iteration order is unspecified in both
 * backends, so every walk is checked by element count and value sum rather than
 * by sequence.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all
 * shuffles the cases and stops at the first failure (test_shuffle.h).
 */
#include "list.h"
#include <stdint.h>

#include "test_shuffle.h"

/* Payload element. `in_use` is the test's own field, not the backends' occupancy
 * bookkeeping (which the list keeps privately); both fields are read straight
 * after list_alloc to confirm the slot is handed back zeroed. */
typedef struct {
    uint32_t in_use;
    uint32_t value;
} test_entry_t;

static int test_init_validation(void) {
    list_t list;
    if (list_init(0, sizeof(test_entry_t), LIST_IMPL_LINKED, 0) == 0)
        return __LINE__;
    if (list_init(&list, 0, LIST_IMPL_LINKED, 0) == 0)
        return __LINE__;
    if (list_init(&list, sizeof(test_entry_t), (list_impl_t)99, 0) == 0)
        return __LINE__;
    /* The chunk capacity is what LIST_IMPL_ARRAY_CHUNK sizes its chunks by, so
     * zero is refused for that backend while LIST_IMPL_LINKED ignores it. */
    if (list_init(&list, sizeof(test_entry_t), LIST_IMPL_ARRAY_CHUNK, 0) == 0)
        return __LINE__;
    return 0;
}

static int test_linked_alloc_iterate(void) {
    list_t list;
    if (list_init(&list, sizeof(test_entry_t), LIST_IMPL_LINKED, 0) != 0)
        return __LINE__;
    test_entry_t* a = (test_entry_t*)list_alloc(&list);
    test_entry_t* b = (test_entry_t*)list_alloc(&list);
    test_entry_t* c = (test_entry_t*)list_alloc(&list);
    if (!a || !b || !c)
        return __LINE__;
    if (a->in_use != 0 || a->value != 0)
        return __LINE__;
    if (b->in_use != 0 || b->value != 0)
        return __LINE__;
    if (c->in_use != 0 || c->value != 0)
        return __LINE__;

    a->in_use = 1;
    a->value = 11;
    b->in_use = 1;
    b->value = 22;
    c->in_use = 1;
    c->value = 33;

    list_iter_t it;
    test_entry_t* p = (test_entry_t*)list_first(&list, &it);
    uint32_t seen = 0;
    uint32_t sum = 0;
    while (p) {
        seen++;
        sum += p->value;
        p = (test_entry_t*)list_next(&it);
    }
    list_destroy(&list);
    if (seen != 3)
        return __LINE__;
    if (sum != (11 + 22 + 33))
        return __LINE__;
    return 0;
}

static int test_array_chunk_alloc_iterate(void) {
    list_t list;
    if (list_init(&list, sizeof(test_entry_t), LIST_IMPL_ARRAY_CHUNK, 2) != 0)
        return __LINE__;
    for (uint32_t i = 0; i < 7; ++i) {
        test_entry_t* slot = (test_entry_t*)list_alloc(&list);
        if (!slot)
            return __LINE__;
        if (slot->in_use != 0 || slot->value != 0)
            return __LINE__;
        slot->in_use = 1;
        slot->value = i + 1;
    }

    list_iter_t it;
    test_entry_t* p = (test_entry_t*)list_first(&list, &it);
    uint32_t seen = 0;
    uint32_t sum = 0;
    while (p) {
        seen++;
        sum += p->value;
        p = (test_entry_t*)list_next(&it);
    }
    list_destroy(&list);
    if (seen != 7)
        return __LINE__;
    if (sum != 28)
        return __LINE__;
    return 0;
}

static int test_linked_remove(void) {
    list_t list;
    if (list_init(&list, sizeof(test_entry_t), LIST_IMPL_LINKED, 0) != 0)
        return __LINE__;
    test_entry_t* a = (test_entry_t*)list_alloc(&list);
    test_entry_t* b = (test_entry_t*)list_alloc(&list);
    test_entry_t* c = (test_entry_t*)list_alloc(&list);
    if (!a || !b || !c)
        return __LINE__;
    a->value = 1;
    b->value = 2;
    c->value = 3;

    if (list_remove(&list, b) != 0)
        return __LINE__;
    if (list_remove(&list, b) == 0)
        return __LINE__;

    list_iter_t it;
    test_entry_t* p = (test_entry_t*)list_first(&list, &it);
    uint32_t seen = 0;
    uint32_t sum = 0;
    while (p) {
        seen++;
        sum += p->value;
        p = (test_entry_t*)list_next(&it);
    }
    if (seen != 2)
        return __LINE__;
    if (sum != 4)
        return __LINE__;
    if (list_remove(&list, (void*)(uintptr_t)0x1234u) == 0)
        return __LINE__;
    list_destroy(&list);
    return 0;
}

static int test_array_chunk_grow_remove_reuse(void) {
    list_t list;
    if (list_init(&list, sizeof(test_entry_t), LIST_IMPL_ARRAY_CHUNK, 2) != 0)
        return __LINE__;

    test_entry_t* slots[5];
    for (uint32_t i = 0; i < 5; ++i) {
        slots[i] = (test_entry_t*)list_alloc(&list);
        if (!slots[i])
            return __LINE__;
        slots[i]->value = i + 1;
    }

    if (list_remove(&list, slots[1]) != 0)
        return __LINE__;
    if (list_remove(&list, slots[3]) != 0)
        return __LINE__;
    if (list_remove(&list, slots[3]) == 0)
        return __LINE__;
    if (list_remove(&list, (void*)(uintptr_t)0x5678u) == 0)
        return __LINE__;

    test_entry_t* n1 = (test_entry_t*)list_alloc(&list);
    test_entry_t* n2 = (test_entry_t*)list_alloc(&list);
    if (!n1 || !n2)
        return __LINE__;
    n1->value = 40;
    n2->value = 50;

    list_iter_t it;
    test_entry_t* p = (test_entry_t*)list_first(&list, &it);
    uint32_t seen = 0;
    uint32_t sum = 0;
    while (p) {
        seen++;
        sum += p->value;
        p = (test_entry_t*)list_next(&it);
    }
    if (seen != 5)
        return __LINE__;
    if (sum != (1 + 3 + 5 + 40 + 50))
        return __LINE__;
    list_destroy(&list);
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_init_validation),
        WASMOS_TEST_CASE(test_linked_alloc_iterate),
        WASMOS_TEST_CASE(test_array_chunk_alloc_iterate),
        WASMOS_TEST_CASE(test_linked_remove),
        WASMOS_TEST_CASE(test_array_chunk_grow_remove_reuse),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
