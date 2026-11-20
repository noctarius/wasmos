/* Host unit test for the orderly shutdown's notification order
 * (src/kernel/shutdown_order.c): a participant is told before anything it
 * needs, directly or through processes that are not participants; ties and
 * cycles fall back to reverse spawn order (highest pid first).
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all stops
 * at the first failure (test_shuffle.h). */

#include "shutdown_order.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

static shutdown_node_t g_nodes[SHUTDOWN_ORDER_MAX];
static uint8_t g_needs[SHUTDOWN_ORDER_MAX * SHUTDOWN_ORDER_MAX];
static uint8_t g_reach[SHUTDOWN_ORDER_MAX * SHUTDOWN_ORDER_MAX];
static uint32_t g_out[SHUTDOWN_ORDER_MAX];
static uint32_t g_n;

static void fresh(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_needs, 0, sizeof(g_needs));
    memset(g_out, 0xFF, sizeof(g_out));
    g_n = 0;
}

static uint32_t node(uint32_t pid, uint8_t participant) {
    g_nodes[g_n].pid = pid;
    g_nodes[g_n].participant = participant;
    return g_n++;
}

/* Packs the needs relation for the nodes added so far: needs(i, j) was recorded
 * against a SHUTDOWN_ORDER_MAX stride and is re-laid at stride g_n. */
static uint32_t order(void) {
    static uint8_t packed[SHUTDOWN_ORDER_MAX * SHUTDOWN_ORDER_MAX];
    for (uint32_t i = 0; i < g_n; ++i) {
        for (uint32_t j = 0; j < g_n; ++j) {
            packed[i * g_n + j] = g_needs[i * SHUTDOWN_ORDER_MAX + j];
        }
    }
    return shutdown_order(g_nodes, g_n, packed, g_reach, g_out);
}

static void needs(uint32_t i, uint32_t j) {
    g_needs[i * SHUTDOWN_ORDER_MAX + j] = 1u;
}

/* Without needs the order is the one it replaces: highest pid first. */
static int test_without_needs_the_newest_goes_first(void) {
    fresh();
    uint32_t a = node(10, 1);
    uint32_t b = node(30, 1);
    uint32_t c = node(20, 1);
    (void)node(40, 0);
    if (order() != 3u) {
        return __LINE__;
    }
    if (g_out[0] != b || g_out[1] != c || g_out[2] != a) {
        return __LINE__;
    }
    return 0;
}

/* A filesystem that needs the volume manager goes first even when it was
 * spawned before it -- the case descending pid gets wrong. */
static int test_a_need_overrides_spawn_order(void) {
    fresh();
    uint32_t fs = node(10, 1);
    uint32_t vol = node(20, 1);
    needs(fs, vol);
    if (order() != 2u || g_out[0] != fs || g_out[1] != vol) {
        return __LINE__;
    }
    return 0;
}

/* The chain runs through a process that is not a participant: fs needs the
 * block driver, which needs the bus driver, which is a participant. */
static int test_a_need_through_a_non_participant_counts(void) {
    fresh();
    uint32_t fs = node(10, 1);
    uint32_t block = node(50, 0);
    uint32_t bus = node(30, 1);
    needs(fs, block);
    needs(block, bus);
    if (order() != 2u || g_out[0] != fs || g_out[1] != bus) {
        return __LINE__;
    }
    return 0;
}

/* Independent chains interleave by pid; each chain keeps its own order. */
static int test_independent_chains_interleave_by_pid(void) {
    fresh();
    uint32_t a1 = node(5, 1);
    uint32_t a2 = node(6, 1);
    uint32_t b1 = node(7, 1);
    uint32_t b2 = node(8, 1);
    needs(a1, a2);
    needs(b1, b2);
    /* Free first: a1 (5) and b1 (7) -> b1; then a1, b2 -> b2; then a1; a2. */
    if (order() != 4u || g_out[0] != b1 || g_out[1] != b2 || g_out[2] != a1 || g_out[3] != a2) {
        return __LINE__;
    }
    return 0;
}

/* A cycle cannot be honoured; it is broken by pid and nobody is dropped. */
static int test_a_cycle_is_broken_by_pid(void) {
    fresh();
    uint32_t x = node(11, 1);
    uint32_t y = node(12, 1);
    needs(x, y);
    needs(y, x);
    if (order() != 2u || g_out[0] != y || g_out[1] != x) {
        return __LINE__;
    }
    return 0;
}

static int test_bad_input_orders_nothing(void) {
    fresh();
    if (shutdown_order(g_nodes, 0, g_needs, g_reach, g_out) != 0u) {
        return __LINE__;
    }
    if (shutdown_order(g_nodes, SHUTDOWN_ORDER_MAX + 1u, g_needs, g_reach, g_out) != 0u) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_without_needs_the_newest_goes_first),
        WASMOS_TEST_CASE(test_a_need_overrides_spawn_order),
        WASMOS_TEST_CASE(test_a_need_through_a_non_participant_counts),
        WASMOS_TEST_CASE(test_independent_chains_interleave_by_pid),
        WASMOS_TEST_CASE(test_a_cycle_is_broken_by_pid),
        WASMOS_TEST_CASE(test_bad_input_orders_nothing),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_shutdown_order: ok\n");
    return 0;
}
