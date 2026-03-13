#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

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
