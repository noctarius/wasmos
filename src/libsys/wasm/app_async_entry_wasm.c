#include "wasmos/service_runtime_wasm.h"

extern wasmos_sys_wasm_async_config_t wasmos_async_app;

/* Loader entry point for an async WASM app. Takes no arguments: startup values
 * come from the spawn-info buffer, and the four zeros below are what the runner
 * still forwards to config->prepare. */
int32_t async_wasmos_main(void) {
    return wasmos_sys_wasm_async_run(&wasmos_async_app, 0, 0, 0, 0);
}
