#include "wasmos/service_runtime_wasm.h"

extern int32_t wasmos_go_async_app_resume(uintptr_t* out_value);

static int32_t go_async_resume(void* user, uintptr_t* out_value) {
    (void)user;
    return wasmos_go_async_app_resume(out_value);
}

static wasmos_sys_wasm_async_config_t g_go_async_app = {
    .resume = go_async_resume,
};

/* Go applications call this once from Main.  The C wrapper owns runtime,
 * endpoint creation, event dispatch, and root-task scheduling. */
int32_t wasmos_go_async_app_run(void) {
    return wasmos_sys_wasm_async_run(&g_go_async_app, 0, 0, 0, 0);
}
