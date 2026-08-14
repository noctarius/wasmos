/* init_smoke - proof that a spawned app runs to completion and is scheduled.
 *
 * Prints a start marker, burns a fixed number of loop iterations, prints a done
 * marker, then burns more before exiting. The busy loops exist to occupy the
 * CPU long enough for the scheduler to preempt this process, which is what the
 * boot test observes: the markers must appear in order and the rest of the
 * system must keep making progress across them. `sink` is volatile so the loops
 * survive optimisation.
 *
 * No preconditions: no service lookup, no IPC, either runtime. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    putsn("init-smoke: init start\n", 23);

    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < 200000u; ++i) {
        sink ^= i;
    }

    putsn("init-smoke: init done\n", 22);

    for (uint32_t i = 0; i < 200000u; ++i) {
        sink ^= (i << 1u);
    }
    return 0;
}
