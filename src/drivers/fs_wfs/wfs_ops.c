/* wfs_ops.c - the bound runtime and block client. */
#include "wfs_ops.h"

static wasmos_wasm_runtime_t* g_runtime;
static wfs_block_t* g_block;

void wfs_ops_bind(wasmos_wasm_runtime_t* runtime, wfs_block_t* block) {
    g_runtime = runtime;
    g_block = block;
}

wfs_block_t* wfs_ops_block(void) {
    return g_block;
}

wasmos_wasm_runtime_t* wfs_ops_runtime(void) {
    return g_runtime;
}

void wfs_ops_task_reset(wasmos_wasm_coroutine_t* task) {
    uint8_t* p = (uint8_t*)task;
    uint32_t i;

    /* Byte-wise rather than memset: this driver carries no libc dependency,
     * which is what lets the host suites link it without the driver ABI. */
    for (i = 0; i < (uint32_t)sizeof(*task); ++i) {
        p[i] = 0u;
    }
}

int32_t wfs_ops_run(wasmos_wasm_task_resume_fn fn, void* ctx) {
    wasmos_wasm_runtime_t* runtime = wfs_ops_runtime();
    wfs_block_t* block = wfs_ops_block();
    wasmos_wasm_coroutine_t task;

    if (!runtime || !block || !block->loop) {
        return WASMOS_ERR_FS_NOT_READY;
    }
    wfs_ops_task_reset(&task);
    if (!wasmos_async_start(runtime, &task, fn, ctx)) {
        return WASMOS_ERR_FS_BUSY;
    }
    for (;;) {
        int32_t status = 0;

        (void)wasmos_wasm_coroutine_run_budget(runtime, 32u);
        if (task.state == WASMOS_WASM_COROUTINE_DEAD) {
            return wasmos_wasm_coroutine_join(&task, &status);
        }
        (void)wasmos_sys_event_loop_poll(block->loop, 8);
    }
}
