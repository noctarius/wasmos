#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

int
main(int argc, char **argv)
{
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
