#ifndef WASMOS_SERVICE_RUNTIME_WASM_H
#define WASMOS_SERVICE_RUNTIME_WASM_H

#include "wasmos/libsys.h"

/* Definition of one async WASM guest, supplied as a single global by the
 * driver/service/app. `resume` is the root task's step function and is
 * required; `prepare` is an optional hook run once, before the runtime starts,
 * with the raw entry arguments so the guest can stash them in `user`. The
 * runtime, root task, event loop and reply endpoint are filled in by
 * wasmos_sys_wasm_async_run() and are scratch state, not caller inputs. */
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
/* Create the reply endpoint and event loop, run `prepare`, start the root task,
 * and pump it until it is dead: each pass resumes one task and, while the root
 * is parked on a future, polls the event loop (which parks on the loop's
 * select-set, so the pump sleeps rather than spins). Returns the root task's
 * result, or -1 for a config with no resume function, a reply endpoint that
 * could not be created, a root task that could not start, a runtime error, or a
 * root task that ended with a non-zero status. Publishes `config` as the active
 * one for the accessors below for as long as it runs; it is not reentrant. */
int32_t wasmos_sys_wasm_async_run(wasmos_sys_wasm_async_config_t* config, int32_t arg0,
                                  int32_t arg1, int32_t arg2, int32_t arg3);
/* Loader entry points supplied by libsys: async_initialize is the driver and
 * service entry, async_wasmos_main the application entry. Neither takes
 * arguments -- startup values come from the spawn-info buffer -- and each calls
 * wasmos_sys_wasm_async_run() on the corresponding global config and returns
 * what that returns. A guest links exactly one of them by linking the matching
 * entry object. */
int32_t async_initialize(void);
int32_t async_wasmos_main(void);
/* Valid while an async app/service wrapper is running.  Applications submit
 * futures through this loop; they never poll or create their own reply port. */
wasmos_sys_event_loop_t* wasmos_sys_wasm_async_event_loop(void);
/* The endpoint the wrapper created for replies, or -1 outside a run. */
int32_t wasmos_sys_wasm_async_reply_endpoint(void);
/* The coroutine runtime driving the root task, or NULL outside a run. Use it to
 * chain futures onto the same runtime the root task awaits on. */
wasmos_wasm_runtime_t* wasmos_sys_wasm_async_runtime(void);

#endif
