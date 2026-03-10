#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, import_name)
#define WASMOS_WASM_EXPORT
#endif

extern int32_t wasmos_console_write(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_write");

static void
write_line(const char *s, int32_t len)
{
    if (len > 0) {
        wasmos_console_write((int32_t)(uintptr_t)s, len);
    }
}

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
        write_line(line1, (int32_t)(sizeof(line1) - 1));
        write_line(line2, (int32_t)(sizeof(line2) - 1));
        write_line(line3, (int32_t)(sizeof(line3) - 1));
    }

    return 0;
}
