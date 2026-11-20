/* shutdown_order.h - the order the orderly shutdown notifies its participants in.
 *
 * Design: docs/architecture/15-drivers-and-services.md, "Orderly Shutdown Design
 * Direction". A participant quiesces while the services beneath it still run, so
 * it is told before anything it needs -- directly, or through processes that are
 * not participants themselves. "Needs" are the manifest's [deps] NEEDS records,
 * resolved to the processes that hold the named service or class.
 *
 * Pure: no kernel state, so the rule is testable on the host
 * (tests/unit/test_shutdown_order.c). */
#ifndef WASMOS_SHUTDOWN_ORDER_H
#define WASMOS_SHUTDOWN_ORDER_H

#include <stdint.h>

/* Most processes one ordering considers: participants plus every process a
 * need leads through. */
#define SHUTDOWN_ORDER_MAX 64u

typedef struct {
    uint32_t pid;
    uint8_t participant; /* told WASMOS_IPC_SHUTDOWN_REQ */
} shutdown_node_t;

/* Orders the participants among nodes[0..n): a participant comes before every
 * participant it reaches through `needs`, where needs[i * n + j] != 0 means
 * node i needs something node j provides. Among participants free to go next
 * the highest pid goes first -- reverse spawn order, the rule this refines --
 * and a cycle of needs is broken the same way. `reach` is n * n bytes of
 * scratch. Writes participant node indices to out[] and returns how many;
 * 0 for n of 0 or above SHUTDOWN_ORDER_MAX. */
uint32_t shutdown_order(const shutdown_node_t* nodes, uint32_t n, const uint8_t* needs,
                        uint8_t* reach, uint32_t* out);

#endif
