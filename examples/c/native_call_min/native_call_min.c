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
extern int32_t wasmos_debug_mark(int32_t tag)
    WASMOS_WASM_IMPORT("wasmos", "debug_mark");

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
    (void)wasmos_console_write((int32_t)(uintptr_t)msg, (int32_t)sizeof(msg) - 1);

    return 0;
}
