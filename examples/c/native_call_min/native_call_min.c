#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    (void)wasmos_debug_mark(0x2201);

    static const char msg[] = "native-call-min: reached\n";
    (void)putsn(msg, sizeof(msg) - 1);

    return 0;
}
