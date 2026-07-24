#include "wasmos/service_runtime_wasm.h"

extern wasmos_sys_wasm_async_config_t wasmos_async_app;

int32_t async_wasmos_main(int32_t a, int32_t b, int32_t c, int32_t d) {
    return wasmos_sys_wasm_async_run(&wasmos_async_app, a, b, c, d);
}
