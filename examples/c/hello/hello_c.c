#include <stdint.h>
#include "stdio.h"
#include "wasmos/imports.h"

WASMOS_WASM_EXPORT int32_t
main(int32_t ignored_arg0,
     int32_t ignored_arg1,
     int32_t ignored_arg2,
     int32_t ignored_arg3)
{
    static int printed = 0;
    (void)ignored_arg0;
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (!printed) {
        printed = 1;
        static const char line1[] = "Hello from C on WASMOS!\n";
        static const char line2[] = "This is a tiny WASMOS-APP written in C.\n";
        static const char line3[] = "Entry: main\n";
        putsn(line1, sizeof(line1) - 1);
        putsn(line2, sizeof(line2) - 1);
        putsn(line3, sizeof(line3) - 1);
    }

    return 0;
}
