/* test_libui_shim_alloc.c - the libui Zig shim's arena allocator.
 *
 * libui_shim.c is compiled for wasm32 in the real build and linked only into
 * the Zig calculator, so nothing exercised its allocator directly. It is plain
 * freestanding C over a static buffer, which makes it host-testable as-is.
 *
 * The cases here are memory-safety properties, not API surface. Two of them
 * fail against the previous implementation:
 *   - realloc copied `size` bytes out of a block whose length it did not know,
 *     so growing a block read past its end -- and past g_arena entirely for a
 *     block at the arena's tail.
 *   - calloc multiplied n * size unchecked, so a wrapped product returned a
 *     block smaller than the caller asked for instead of NULL.
 *
 * Reading past the arena does not necessarily fault, so "did it crash" is not
 * the oracle. The grow case instead asserts the property directly: bytes beyond
 * what the old block held must read back as malloc's zero fill, which they
 * cannot if they were copied out of whatever followed the block.
 *
 * Every case starts by resetting the arena through the WASMOS_LIBUI_SHIM_TEST_SEAMS
 * seam, so no case depends on what the previous one allocated and the suite can
 * be shuffled like the others here.
 */
#include <stddef.h>
#include <stdint.h>

#include "test_shuffle.h"

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t n, size_t size);
void* realloc(void* old, size_t size);
void libui_shim_arena_reset(void);

/* Link-time stand-ins for the host calls the shim's libui_zig_* wrappers reach.
 * The real build resolves these as WASM imports; none of the allocator cases
 * calls a wrapper, so these exist to satisfy the link, not to model behaviour.
 * Each answers the failure value for its call, so a case that reached one by
 * accident would fail rather than proceed on a fabricated success. */
int32_t wasmos_ipc_create_endpoint(void) {
    return -1;
}
int32_t wasmos_ipc_last_field(int32_t field) {
    (void)field;
    return -1;
}
int32_t wasmos_ipc_select_one(int32_t endpoint) {
    (void)endpoint;
    return -1;
}
int32_t wasmos_ipc_send(int32_t dest, int32_t src, int32_t type, int32_t request_id, int32_t a0,
                        int32_t a1, int32_t a2, int32_t a3) {
    (void)dest;
    (void)src;
    (void)type;
    (void)request_id;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return -1;
}
int32_t wasmos_sched_yield(void) {
    return -1;
}
int32_t wasmos_shmem_create(int32_t size) {
    (void)size;
    return -1;
}
int32_t wasmos_shmem_flush(int32_t id, int32_t ptr, int32_t len, int32_t offset) {
    (void)id;
    (void)ptr;
    (void)len;
    (void)offset;
    return -1;
}
int32_t wasmos_shmem_map_auto(int32_t id) {
    (void)id;
    return -1;
}
int32_t wasmos_shmem_refresh(int32_t id, int32_t ptr, int32_t len, int32_t offset) {
    (void)id;
    (void)ptr;
    (void)len;
    (void)offset;
    return -1;
}
int32_t wasmos_shmem_unmap(int32_t id) {
    (void)id;
    return -1;
}

/* Growing a block must preserve its contents and leave the rest zeroed.
 *
 * The old block is filled with a non-zero pattern and a second block is
 * allocated right behind it, also non-zero. Under the old implementation the
 * grow copied over the neighbour, so the tail of the new block came back
 * holding 0xBB. */
static int test_realloc_grow_does_not_read_past_the_old_block(void) {
    libui_shim_arena_reset();
    const size_t old_bytes = 16;
    const size_t new_bytes = 128;

    uint8_t* first = (uint8_t*)malloc(old_bytes);
    if (!first) {
        return __LINE__;
    }
    for (size_t i = 0; i < old_bytes; ++i) {
        first[i] = 0xAA;
    }
    /* The neighbour whose bytes an unbounded copy would pull in. */
    uint8_t* neighbour = (uint8_t*)malloc(256);
    if (!neighbour) {
        return __LINE__;
    }
    for (size_t i = 0; i < 256; ++i) {
        neighbour[i] = 0xBB;
    }

    uint8_t* grown = (uint8_t*)realloc(first, new_bytes);
    if (!grown) {
        return __LINE__;
    }
    for (size_t i = 0; i < old_bytes; ++i) {
        if (grown[i] != 0xAA) {
            return __LINE__;
        }
    }
    for (size_t i = old_bytes; i < new_bytes; ++i) {
        if (grown[i] != 0x00) {
            return __LINE__;
        }
    }
    return 0;
}

/* Shrinking copies only the requested prefix. */
static int test_realloc_shrink_keeps_the_prefix(void) {
    libui_shim_arena_reset();
    uint8_t* block = (uint8_t*)malloc(64);
    if (!block) {
        return __LINE__;
    }
    for (size_t i = 0; i < 64; ++i) {
        block[i] = (uint8_t)(i + 1u);
    }
    uint8_t* small = (uint8_t*)realloc(block, 8);
    if (!small) {
        return __LINE__;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (small[i] != (uint8_t)(i + 1u)) {
            return __LINE__;
        }
    }
    return 0;
}

/* A NULL old pointer is a plain malloc, and the result is zeroed. */
static int test_realloc_null_is_malloc(void) {
    libui_shim_arena_reset();
    uint8_t* block = (uint8_t*)realloc(NULL, 32);
    if (!block) {
        return __LINE__;
    }
    for (size_t i = 0; i < 32; ++i) {
        if (block[i] != 0) {
            return __LINE__;
        }
    }
    return 0;
}

/* A pointer this arena never handed out has no recorded length, so realloc
 * refuses it rather than reading a header that was never written. */
static int test_realloc_rejects_a_foreign_pointer(void) {
    libui_shim_arena_reset();
    static uint8_t foreign[32];
    if (realloc(foreign, 64) != NULL) {
        return __LINE__;
    }
    return 0;
}

/* An overflowing product must be refused, not wrapped into a short block. */
static int test_calloc_overflow_is_refused(void) {
    libui_shim_arena_reset();
    const size_t huge = ((size_t)-1 / 2u) + 2u;
    if (calloc(huge, 4) != NULL) {
        return __LINE__;
    }
    if (calloc(4, huge) != NULL) {
        return __LINE__;
    }
    /* (size_t)-1 * 2 wraps to a small even number on any two's-complement
       target, which is the shape that produced an undersized block. */
    if (calloc((size_t)-1, 2) != NULL) {
        return __LINE__;
    }
    return 0;
}

/* An ordinary calloc still works and is zeroed. */
static int test_calloc_ordinary(void) {
    libui_shim_arena_reset();
    uint8_t* block = (uint8_t*)calloc(8, 4);
    if (!block) {
        return __LINE__;
    }
    for (size_t i = 0; i < 32; ++i) {
        if (block[i] != 0) {
            return __LINE__;
        }
    }
    return 0;
}

/* Exhaustion reports NULL rather than handing out memory past the arena, and
 * stays exhausted afterwards -- the arithmetic that decides this underflowed in
 * an earlier draft of the fix, which turns the bound into a comparison against
 * ~4 GiB and lets the next request through. Starts from a reset arena, so the
 * page count needed to drain it does not depend on the other cases. */
static int test_arena_exhaustion_returns_null(void) {
    libui_shim_arena_reset();
    for (int i = 0; i < 64; ++i) {
        if (malloc(4096) == NULL) {
            /* Once exhausted it must stay exhausted, including for a request
               that would fit if the bounds check had underflowed. */
            if (malloc(8) != NULL && malloc(4096) != NULL) {
                return __LINE__;
            }
            return 0;
        }
    }
    return __LINE__; /* 64 * 4 KiB should have exceeded a 12 KiB arena */
}

int main(void) {
    /* Randomized order: every case resets the arena first, so a case that leaks
     * state must not be able to make its neighbour pass. Replay with
     * WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_realloc_grow_does_not_read_past_the_old_block),
        WASMOS_TEST_CASE(test_realloc_shrink_keeps_the_prefix),
        WASMOS_TEST_CASE(test_realloc_null_is_malloc),
        WASMOS_TEST_CASE(test_realloc_rejects_a_foreign_pointer),
        WASMOS_TEST_CASE(test_calloc_overflow_is_refused),
        WASMOS_TEST_CASE(test_calloc_ordinary),
        WASMOS_TEST_CASE(test_arena_exhaustion_returns_null),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
