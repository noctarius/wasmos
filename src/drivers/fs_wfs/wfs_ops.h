/* wfs_ops.h - the plumbing every WFS operation task shares.
 *
 * The system coroutine runtime hands a task only its `user` pointer, but every
 * operation needs the same block client and the same runtime to start children
 * on. Both are bound once, here, rather than threaded through every context.
 */
#ifndef FS_WFS_WFS_OPS_H
#define FS_WFS_WFS_OPS_H

#include "wasmos/coroutine_wasm.h"
#include "wasmos_status.h"
#include "wfs_block.h"

/* Bind the runtime and block client the tasks operate on. Called once, before
 * any task is started. */
void wfs_ops_bind(wasmos_wasm_runtime_t* runtime, wfs_block_t* block);

/* The bound block client, or NULL before wfs_ops_bind. */
wfs_block_t* wfs_ops_block(void);

/* The bound runtime, or NULL before wfs_ops_bind. */
wasmos_wasm_runtime_t* wfs_ops_runtime(void);

/* Await `future` from inside a task, resuming at `next_pc`.
 *
 * A wrapper over wasmos_future_await, not a scheduler: the parking, the waiter
 * list and the resumption are the runtime's. It exists only so a step does not
 * repeat the store-pc / test-PENDING / return-YIELDED sequence at every await,
 * and so the resume point is written down next to the await it belongs to.
 *
 * A NULL future means there was nothing to wait for — a staged-block cache hit —
 * so execution falls straight through.
 */
#define WFS_AWAIT(ctx, future, next_pc)                                                            \
    do {                                                                                           \
        wasmos_future_t* _f = (future);                                                            \
        (ctx)->pc = (next_pc);                                                                     \
        if (_f) {                                                                                  \
            uintptr_t _v = 0;                                                                      \
            if (wasmos_future_await(_f, &_v) == WASMOS_WASM_AWAIT_PENDING) {                       \
                return WASMOS_WASM_TASK_YIELDED;                                                   \
            }                                                                                      \
        }                                                                                          \
    } while (0)

/* Fail the task with a packed code. Returned as a negative status, which the
 * runtime uses to reject the task's completion future, so a caller that joins it
 * receives the code with no separate status channel. */
#define WFS_FAIL(ctx, code)                                                                        \
    do {                                                                                           \
        (ctx)->err = (code);                                                                       \
        return (int32_t)(code);                                                                    \
    } while (0)

#endif /* FS_WFS_WFS_OPS_H */
