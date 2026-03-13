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

__attribute__((noinline, used)) static int32_t
call_console(const char *s)
{
    int32_t len = 0;
    while (s[len]) {
        len++;
    }
    return wasmos_console_write((int32_t)(uintptr_t)s, len);
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

    static const char msg[] = "native-call-smoke: start\n";
    volatile int32_t write_rc = wasmos_console_write((int32_t)(uintptr_t)msg, (int32_t)sizeof(msg) - 1);
    (void)write_rc;

    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < 100000u; ++i) {
        sink ^= (i ^ (uint32_t)write_rc);
    }

    static const char done[] = "native-call-smoke: done\n";
    write_rc = wasmos_console_write((int32_t)(uintptr_t)done, (int32_t)sizeof(done) - 1);
    sink ^= (uint32_t)write_rc;

    return (int32_t)sink;
}
