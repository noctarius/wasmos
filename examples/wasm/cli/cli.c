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

typedef enum {
    CLI_PHASE_INIT = 0,
    CLI_PHASE_DONE,
    CLI_PHASE_FAILED
} cli_phase_t;

static cli_phase_t g_phase = CLI_PHASE_INIT;

static int32_t
str_len(const char *s)
{
    int32_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

static void
console_write(const char *s)
{
    int32_t len = str_len(s);
    if (len <= 0) {
        return;
    }
    wasmos_console_write((int32_t)(uintptr_t)s, len);
}

WASMOS_WASM_EXPORT int32_t
cli_step(int32_t ignored_type,
         int32_t ignored_arg0,
         int32_t ignored_arg1,
         int32_t ignored_arg2,
         int32_t ignored_arg3)
{
    (void)ignored_type;
    (void)ignored_arg0;
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (g_phase == CLI_PHASE_INIT) {
        const char *msg = "WASMOS CLI\ncommands: help, ls, cat, ps, exec\n(inputs not wired yet)\n";
        wasmos_console_write((int32_t)(uintptr_t)msg, str_len(msg));
        g_phase = CLI_PHASE_DONE;
        return WASMOS_WASM_STEP_DONE;
    }

    if (g_phase == CLI_PHASE_DONE) {
        return WASMOS_WASM_STEP_DONE;
    }

    return WASMOS_WASM_STEP_FAILED;
}
