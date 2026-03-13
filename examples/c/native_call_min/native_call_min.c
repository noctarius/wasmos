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

    (void)wasmos_debug_mark(0x2201);

    static const char msg[] = "native-call-min: reached\n";
    (void)putsn(msg, sizeof(msg) - 1);

    return 0;
}
