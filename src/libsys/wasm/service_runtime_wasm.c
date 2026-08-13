#include "wasmos/service_runtime_wasm.h"

static wasmos_sys_wasm_async_config_t* g_active_config;

wasmos_sys_event_loop_t* wasmos_sys_wasm_async_event_loop(void) {
    return g_active_config ? &g_active_config->event_loop : NULL;
}

int32_t wasmos_sys_wasm_async_reply_endpoint(void) {
    return g_active_config ? g_active_config->reply_endpoint : -1;
}

wasmos_wasm_runtime_t* wasmos_sys_wasm_async_runtime(void) {
    return g_active_config ? &g_active_config->runtime : NULL;
}

int32_t wasmos_sys_wasm_async_run(wasmos_sys_wasm_async_config_t* config, int32_t arg0,
                                  int32_t arg1, int32_t arg2, int32_t arg3) {
    int32_t result = -1;
    if (!config || !config->resume)
        return -1;
    config->reply_endpoint = wasmos_ipc_create_endpoint();
    if (config->reply_endpoint < 0)
        return -1;
    wasmos_sys_event_loop_init(&config->event_loop, config->reply_endpoint, 1);
    if (config->prepare)
        config->prepare(config->user, arg0, arg1, arg2, arg3);
    wasmos_wasm_runtime_init(&config->runtime);
    if (!wasmos_async_start(&config->runtime, &config->root, config->resume, config->user))
        return -1;
    g_active_config = config;
    while (config->root.state != WASMOS_WASM_COROUTINE_DEAD) {
        if (wasmos_wasm_coroutine_run_budget(&config->runtime, 1u) < 0)
            return -1;
        if (config->root.state == WASMOS_WASM_COROUTINE_WAITING) {
            /* The root task is parked on a future: poll parks on the wrapper's
             * select-set until a reply lands, so this loop does not spin. */
            (void)wasmos_sys_event_loop_poll(&config->event_loop, 1);
        }
    }
    g_active_config = NULL;
    return wasmos_wasm_coroutine_join(&config->root, &result) == 0 ? result : -1;
}
