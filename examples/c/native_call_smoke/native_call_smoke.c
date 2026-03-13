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

    static const char msg[] = "native-call-smoke: start\n";
    volatile int32_t write_rc = putsn(msg, sizeof(msg) - 1);
    (void)write_rc;

    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < 100000u; ++i) {
        sink ^= (i ^ (uint32_t)write_rc);
    }

    static const char done[] = "native-call-smoke: done\n";
    write_rc = putsn(done, sizeof(done) - 1);
    sink ^= (uint32_t)write_rc;

    return (int32_t)sink;
}
