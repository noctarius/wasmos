#include "shutdown_order.h"

uint32_t shutdown_order(const shutdown_node_t* nodes, uint32_t n, const uint8_t* needs,
                        uint8_t* reach, uint32_t* out) {
    uint8_t done[SHUTDOWN_ORDER_MAX];
    uint32_t remaining = 0;
    uint32_t emitted = 0;

    if (!nodes || !needs || !reach || !out || n == 0u || n > SHUTDOWN_ORDER_MAX) {
        return 0;
    }
    /* Transitive closure of `needs` (Warshall): reach[i][j] when i needs j
     * through any chain, participants or not. */
    for (uint32_t i = 0; i < n * n; ++i) {
        reach[i] = needs[i] ? 1u : 0u;
    }
    for (uint32_t k = 0; k < n; ++k) {
        for (uint32_t i = 0; i < n; ++i) {
            if (!reach[i * n + k]) {
                continue;
            }
            for (uint32_t j = 0; j < n; ++j) {
                if (reach[k * n + j]) {
                    reach[i * n + j] = 1u;
                }
            }
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        done[i] = 0u;
        if (nodes[i].participant) {
            remaining++;
        }
    }
    while (emitted < remaining) {
        int32_t best = -1;
        int32_t fallback = -1;
        for (uint32_t c = 0; c < n; ++c) {
            int needed = 0;
            if (!nodes[c].participant || done[c]) {
                continue;
            }
            if (fallback < 0 || nodes[c].pid > nodes[fallback].pid) {
                fallback = (int32_t)c;
            }
            /* Free to go once no participant still to be told needs it. */
            for (uint32_t r = 0; r < n && !needed; ++r) {
                needed = r != c && nodes[r].participant && !done[r] && reach[r * n + c];
            }
            if (!needed && (best < 0 || nodes[c].pid > nodes[best].pid)) {
                best = (int32_t)c;
            }
        }
        if (best < 0) {
            best = fallback; /* every one left is in a cycle */
        }
        done[best] = 1u;
        out[emitted++] = (uint32_t)best;
    }
    return emitted;
}
