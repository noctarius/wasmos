#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

WASMOS_WASM_EXPORT int32_t
main(int32_t arg0,
     int32_t arg1,
     int32_t arg2,
     int32_t arg3)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;

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
