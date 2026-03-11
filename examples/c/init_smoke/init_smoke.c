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

static int32_t (*volatile g_console_write)(int32_t, int32_t) = wasmos_console_write;

static void
write_line(const char *s)
{
    if (!s) {
        return;
    }
    int32_t len = 0;
    while (s[len]) {
        len++;
    }
    if (len > 0) {
        g_console_write((int32_t)(uintptr_t)s, len);
    }
}

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

    write_line("init-smoke: init start\n");

    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < 200000u; ++i) {
        sink ^= i;
    }

    write_line("init-smoke: init done\n");

    for (uint32_t i = 0; i < 200000u; ++i) {
        sink ^= (i << 1u);
    }
    return 0;
}
