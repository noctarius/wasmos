#include "wasmos/coroutine_wasm.h"

/* TinyGo cannot portably turn a Go function into a C function pointer.  This
 * stable C table entry forwards a caller-supplied 32-bit task id to one Go
 * export; the Go side owns the task registry and may serve many coroutines. */
extern int32_t wasmos_go_coroutine_resume(uint32_t task_id, uintptr_t* out_value);
extern int32_t wasmos_go_future_success(uint32_t callback_id, uintptr_t value,
                                        uintptr_t* out_value);
extern int32_t wasmos_go_future_error(uint32_t callback_id, int32_t status, uintptr_t* out_value);
extern uintptr_t wasmos_go_future_chain(uint32_t callback_id, uintptr_t value);

static int32_t go_resume(void* user, uintptr_t* out_value) {
    return wasmos_go_coroutine_resume((uint32_t)(uintptr_t)user, out_value);
}

wasmos_future_t* wasmos_go_coroutine_start(wasmos_wasm_runtime_t* runtime,
                                           wasmos_wasm_coroutine_t* coroutine, uint32_t task_id) {
    if (!runtime || !coroutine || task_id == 0)
        return 0;
    return wasmos_async_start(runtime, coroutine, go_resume, (void*)(uintptr_t)task_id);
}

static int32_t go_success(void* user, uintptr_t value, uintptr_t* out_value) {
    return wasmos_go_future_success((uint32_t)(uintptr_t)user, value, out_value);
}
static int32_t go_error(void* user, int32_t status, uintptr_t* out_value) {
    return wasmos_go_future_error((uint32_t)(uintptr_t)user, status, out_value);
}

wasmos_future_t* wasmos_go_future_then(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                       wasmos_future_continuation_t* continuation,
                                       uint32_t callback_id) {
    if (!callback_id)
        return 0;
    return wasmos_future_then(runtime, future, continuation, go_success, go_error,
                              (void*)(uintptr_t)callback_id);
}

static int32_t go_chain(void* user, uintptr_t value, uintptr_t* out_value) {
    uintptr_t next = wasmos_go_future_chain((uint32_t)(uintptr_t)user, value);
    if (!next)
        return -1;
    *out_value = next;
    return WASMOS_FUTURE_CHAIN_NEXT;
}
wasmos_future_t* wasmos_go_future_then_flat(wasmos_wasm_runtime_t* runtime, wasmos_future_t* future,
                                            wasmos_future_continuation_t* continuation,
                                            wasmos_future_continuation_t* adopt,
                                            uint32_t callback_id) {
    if (!callback_id)
        return 0;
    return wasmos_future_then_flat(runtime, future, continuation, adopt, go_chain, go_error,
                                   (void*)(uintptr_t)callback_id);
}
