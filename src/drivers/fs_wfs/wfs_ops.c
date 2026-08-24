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
