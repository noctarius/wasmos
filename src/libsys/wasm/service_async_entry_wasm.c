#include "wasmos/service_runtime_wasm.h"

extern wasmos_sys_wasm_async_config_t wasmos_async_service;

/* Loader entry point for an async WASM service. Takes no arguments: startup
 * values come from the spawn-info buffer, and the four zeros below are what the
 * runner still forwards to config->prepare. */
int32_t async_initialize(void) {
    return wasmos_sys_wasm_async_run(&wasmos_async_service, 0, 0, 0, 0);
}
