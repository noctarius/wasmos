/* linmem_hold - test fixture: grow linear memory, then hold it.
 *
 * Usage: linmem_hold
 *
 * Grows linear memory by 1 MiB, which under WARP moves it into the process's
 * dedicated linear-memory VA slot, and then blocks on its own endpoint so the
 * slot stays taken. tests/test_linmem_slots.py spawns these until the slots
 * run out, to show that a module whose linear memory cannot get a slot is
 * refused rather than run from a block that relocates when it grows.
 *
 * Prints "linmem-hold: holding" once the grow succeeded, or "linmem-hold: grow
 * failed" and exits 1. */
#include "stdio.h"
#include "wasmos/ipc.h"

int main(void) {
    if (__builtin_wasm_memory_grow(0, 16) < 0) {
        puts("linmem-hold: grow failed");
        return 1;
    }
    int32_t endpoint = wasmos_ipc_create_endpoint();
    puts("linmem-hold: holding");
    /* Nothing ever sends here: the receive parks the process for good. */
    while (endpoint >= 0) {
        (void)wasmos_ipc_select_one(endpoint);
    }
    return 0;
}
