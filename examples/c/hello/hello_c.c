/* hello_c - the "hello world" tutorial app for the C guest binding.
 *
 * Demonstrates the two things every C WASMOS-APP needs: console output through
 * libc (putsn), and the in-process coroutine runtime from
 * wasmos/coroutine_wasm.h.
 *
 * The coroutine part is the interesting half. A task is a plain resume
 * function: it is called repeatedly, returns wasmos_wasm_coroutine_yield() to
 * be called again later, and WASMOS_WASM_TASK_COMPLETE with a value written to
 * *out_value when it is done. wasmos_async_start registers it,
 * wasmos_wasm_coroutine_run drives every runnable task until none can make
 * progress, and wasmos_future_poll reads the completion status without
 * blocking. This task deliberately yields once; since it is immediately
 * runnable again, the one unbounded run call resumes it twice and the poll then
 * sees the completed value.
 *
 * No preconditions: no service lookup, no IPC, runs on either runtime. */
#include "stdio.h"
#include "wasmos/coroutine_wasm.h"

typedef struct {
    int phase;
} hello_task_t;

static int32_t hello_resume(void* user, uintptr_t* out_value) {
    hello_task_t* task = user;
    if (task->phase++ == 0)
        return wasmos_wasm_coroutine_yield();
    *out_value = 42u;
    return WASMOS_WASM_TASK_COMPLETE;
}

int main(int argc, char** argv) {
    static int printed = 0;
    (void)argc;
    (void)argv;
    if (!printed) {
        wasmos_wasm_runtime_t runtime = {0};
        wasmos_wasm_coroutine_t coroutine = {0};
        hello_task_t task = {0};
        int32_t status = -1;
        uintptr_t value = 0;
        int coroutine_ok;
        printed = 1;
        static const char line1[] = "Hello from C on WASMOS!\n";
        static const char line2[] = "This is a tiny WASMOS-APP written in C.\n";
        static const char line3[] = "Entry: main\n";
        wasmos_wasm_runtime_init(&runtime);
        coroutine_ok = wasmos_async_start(&runtime, &coroutine, hello_resume, &task) != NULL &&
                       wasmos_wasm_coroutine_run(&runtime) >= 0 &&
                       wasmos_future_poll(&coroutine.completion, &status, &value) && status == 0 &&
                       value == 42u;
        putsn(line1, sizeof(line1) - 1);
        putsn(line2, sizeof(line2) - 1);
        putsn(line3, sizeof(line3) - 1);
        putsn(coroutine_ok ? "coroutine task: ready\n" : "coroutine task: failed\n",
              coroutine_ok ? sizeof("coroutine task: ready\n") - 1
                           : sizeof("coroutine task: failed\n") - 1);
    }
    return 0;
}
