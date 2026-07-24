#ifndef WASMOS_SERVICE_RUNTIME_WASM_H
#define WASMOS_SERVICE_RUNTIME_WASM_H

#include "wasmos/libsys.h"

typedef struct {
    wasmos_wasm_runtime_t runtime;
    wasmos_wasm_coroutine_t root;
    wasmos_sys_event_loop_t event_loop;
    int32_t reply_endpoint;
    wasmos_wasm_task_resume_fn resume;
    void (*prepare)(void* user, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3);
    void* user;
} wasmos_sys_wasm_async_config_t;

/* Drivers/services define wasmos_async_service; applications define
 * wasmos_async_app. Their root state owns the corresponding entry arguments. */
int32_t wasmos_sys_wasm_async_run(wasmos_sys_wasm_async_config_t* config, int32_t arg0, int32_t arg1,
                                   int32_t arg2, int32_t arg3);
int32_t async_initialize(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3);
int32_t async_wasmos_main(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3);
/* Valid while an async app/service wrapper is running.  Applications submit
 * futures through this loop; they never poll or create their own reply port. */
wasmos_sys_event_loop_t* wasmos_sys_wasm_async_event_loop(void);
int32_t wasmos_sys_wasm_async_reply_endpoint(void);
wasmos_wasm_runtime_t* wasmos_sys_wasm_async_runtime(void);

#endif
