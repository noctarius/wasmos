/* wfs_bitmap.c - allocation bitmap access and the run search (§12). */
#include "wfs_bitmap.h"

int wfs_bitmap_test(const uint8_t* map, uint32_t i) {
    if (!map) {
        return 0;
    }
    return (map[i >> 3] & (uint8_t)(1u << (i & 7u))) != 0u;
}

void wfs_bitmap_set(uint8_t* map, uint32_t i) {
    if (!map) {
        return;
    }
    map[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

void wfs_bitmap_clear(uint8_t* map, uint32_t i) {
    if (!map) {
        return;
    }
    map[i >> 3] &= (uint8_t)~(1u << (i & 7u));
}

uint32_t wfs_bitmap_count_free(const uint8_t* map, uint32_t bits) {
    uint32_t free_bits = 0u;
    uint32_t i;

    if (!map) {
        return 0u;
    }
    /* Bit at a time rather than a popcount over whole bytes: `bits` need not be
     * a byte multiple, so the tail would need the special case anyway, and this
     * runs once per allocation rather than per byte of I/O. */
    for (i = 0u; i < bits; ++i) {
        if (!wfs_bitmap_test(map, i)) {
            free_bits++;
        }
    }
    return free_bits;
}

uint32_t wfs_bitmap_find_run(const uint8_t* map, uint32_t bits, uint32_t want,
                             uint32_t* out_start) {
    uint32_t best_start = 0u;
    uint32_t best_len = 0u;
    uint32_t run_start = 0u;
    uint32_t run_len = 0u;
    uint32_t i;

    if (!map || !out_start || bits == 0u || want == 0u) {
        return 0u;
    }

    for (i = 0u; i < bits; ++i) {
        if (wfs_bitmap_test(map, i)) {
            run_len = 0u;
            continue;
        }
        if (run_len == 0u) {
            run_start = i;
        }
        run_len++;
        /* First fit: the earliest run long enough ends the search, which keeps
         * an allocation near the metadata that precedes it instead of hunting
         * for a tighter one further out. */
        if (run_len >= want) {
            *out_start = run_start;
            return want;
        }
        if (run_len > best_len) {
            best_len = run_len;
            best_start = run_start;
        }
    }

    if (best_len == 0u) {
        return 0u;
    }
    /* Nothing was long enough, so the longest run is offered for the caller to
     * take and come back for the remainder -- the fragmented fallback in §12.
     * A run is never reported past `bits`: the final group of a volume is
     * partial, and blocks the device does not have must not be handed out. */
    *out_start = best_start;
    return best_len;
}
