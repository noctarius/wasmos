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
