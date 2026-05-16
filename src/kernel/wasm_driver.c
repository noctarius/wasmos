#include "wasm_driver.h"
#include "klog.h"
#include "process.h"
#include "serial.h"
#include "wasm3_link.h"
#include "wasm3_shim.h"

static void
log_wasm3_error(const char *prefix, const char *error, IM3Runtime runtime)
{
    klog_write(prefix);
    if (error && error[0]) {
        klog_write(error);
    } else {
        klog_write("unknown");
    }
    klog_write("\n");
    if (runtime) {
        M3ErrorInfo info;
        m3_GetErrorInfo(runtime, &info);
        if (info.message && info.message[0]) {
            klog_write("[wasm-driver] detail: ");
            klog_write(info.message);
            klog_write("\n");
        }
    }
}

static void
wasm_driver_reset(wasm_driver_t *driver)
{
    if (!driver) {
        return;
    }
    driver->module = 0;
    driver->runtime = 0;
    driver->env = 0;
    driver->owner_pid = 0;
    driver->owner_context_id = 0;
    driver->endpoint = IPC_ENDPOINT_NONE;
    driver->active = 0;
}

static uint32_t
wasm_driver_bind_owner_pid(const wasm_driver_t *driver)
{
    uint32_t current_pid = process_current_pid();
    if (!driver || driver->owner_pid == 0 || driver->owner_pid == current_pid) {
        return 0xFFFFFFFFu;
    }
    return wasm3_heap_bind_pid(driver->owner_pid);
}

static void
wasm_driver_restore_owner_pid(uint32_t previous_pid)
{
    if (previous_pid != 0xFFFFFFFFu) {
        wasm3_heap_restore_pid(previous_pid);
    }
}

static uint32_t
wasm_driver_enter_runtime(const wasm_driver_t *driver)
{
    preempt_disable();
    return wasm_driver_bind_owner_pid(driver);
}

static void
wasm_driver_leave_runtime(uint32_t previous_pid)
{
    wasm_driver_restore_owner_pid(previous_pid);
    preempt_enable();
}

int
wasm_driver_start(wasm_driver_t *driver,
                  const wasm_driver_manifest_t *manifest,
                  uint32_t owner_context_id)
{
    if (!driver || !manifest || !manifest->module_bytes ||
        manifest->module_size == 0 ||
        owner_context_id == 0) {
        return -1;
    }

    wasm_driver_reset(driver);
    driver->manifest = *manifest;
    driver->owner_context_id = owner_context_id;
    spinlock_init(&driver->lock);
    process_t *owner = process_find_by_context(owner_context_id);
    driver->owner_pid = owner ? owner->pid : process_current_pid();
    wasm3_heap_configure(driver->owner_pid,
                         driver->manifest.heap_size,
                         2ULL * 1024ULL * 1024ULL * 1024ULL);

    uint32_t previous_pid = wasm_driver_enter_runtime(driver);

    driver->env = m3_NewEnvironment();
    if (!driver->env) {
        klog_write("[wasm-driver] env alloc failed\n");
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }

    uint32_t stack_size = driver->manifest.stack_size ? driver->manifest.stack_size : (64u * 1024u);
    driver->runtime = m3_NewRuntime(driver->env, stack_size, NULL);
    if (!driver->runtime) {
        klog_write("[wasm-driver] runtime alloc failed\n");
        m3_FreeEnvironment(driver->env);
        driver->env = 0;
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }

    M3Result res = m3_ParseModule(driver->env, &driver->module,
                                  driver->manifest.module_bytes,
                                  driver->manifest.module_size);
    if (res) {
        log_wasm3_error("[wasm-driver] parse failed: ", res, driver->runtime);
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        driver->runtime = 0;
        driver->env = 0;
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }

    res = m3_LoadModule(driver->runtime, driver->module);
    if (res) {
        log_wasm3_error("[wasm-driver] load failed: ", res, driver->runtime);
        m3_FreeModule(driver->module);
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        driver->runtime = 0;
        driver->env = 0;
        driver->module = 0;
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }

    if (wasm3_link_wasmos(driver->module) != 0 || wasm3_link_env(driver->module) != 0) {
        klog_write("[wasm-driver] link failed\n");
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        driver->runtime = 0;
        driver->env = 0;
        driver->module = 0;
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }

    if (ipc_endpoint_create(owner_context_id, &driver->endpoint) != 0) {
        klog_write("[wasm-driver] endpoint allocation failed\n");
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        wasm_driver_reset(driver);
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }

    driver->active = 1;
    klog_write("[wasm-driver] started\n");
    wasm_driver_leave_runtime(previous_pid);
    return 0;
}

void
wasm_driver_stop(wasm_driver_t *driver)
{
    if (!driver) {
        return;
    }
    uint32_t previous_pid = wasm_driver_enter_runtime(driver);
    if (driver->runtime) {
        m3_FreeRuntime(driver->runtime);
    }
    if (driver->env) {
        m3_FreeEnvironment(driver->env);
    }
    wasm_driver_leave_runtime(previous_pid);
    wasm_driver_reset(driver);
}

int
wasm_driver_endpoint(const wasm_driver_t *driver, uint32_t *out_endpoint)
{
    if (!driver || !out_endpoint || !driver->active ||
        driver->endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    *out_endpoint = driver->endpoint;
    return 0;
}

int
wasm_driver_call_entry(wasm_driver_t *driver)
{
    uint32_t argv[4] = { 0, 0, 0, 0 };
    uint32_t argc = 0;

    if (!driver || !driver->active || !driver->runtime ||
        !driver->manifest.entry_export) {
        return -1;
    }

    argc = driver->manifest.entry_argc;
    if (argc > 4) {
        klog_write("[wasm-driver] entry args too large\n");
        return -1;
    }
    for (uint32_t i = 0; i < argc; ++i) {
        argv[i] = driver->manifest.entry_argv ? driver->manifest.entry_argv[i] : 0;
    }

    IM3Function func = NULL;
    uint32_t previous_pid = wasm_driver_enter_runtime(driver);
    M3Result res = m3_FindFunction(&func, driver->runtime, driver->manifest.entry_export);
    if (res) {
        log_wasm3_error("[wasm-driver] entry lookup failed: ", res, driver->runtime);
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }
    const void *args[4] = { &argv[0], &argv[1], &argv[2], &argv[3] };
    res = m3_Call(func, argc, args);
    if (res) {
        log_wasm3_error("[wasm-driver] entry failed: ", res, driver->runtime);
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }
    wasm_driver_leave_runtime(previous_pid);
    return 0;
}

int
wasm_driver_call(wasm_driver_t *driver,
                 const char *export_name,
                 uint32_t argc,
                 uint32_t *argv)
{
    if (!driver || !export_name || !driver->active || !driver->runtime) {
        return -1;
    }

    spinlock_lock(&driver->lock);
    uint32_t previous_pid = wasm_driver_enter_runtime(driver);
    IM3Function func = NULL;
    M3Result res = m3_FindFunction(&func, driver->runtime, export_name);
    int ok = 0;
    if (res) {
        log_wasm3_error("[wasm-driver] lookup failed: ", res, driver->runtime);
    } else {
        const void *args[4] = { &argv[0], &argv[1], &argv[2], &argv[3] };
        res = m3_Call(func, argc, args);
        ok = res ? -1 : 0;
        if (res) {
            log_wasm3_error("[wasm-driver] call failed: ", res, driver->runtime);
        }
    }
    wasm_driver_leave_runtime(previous_pid);
    spinlock_unlock(&driver->lock);
    return ok == 0 ? 0 : -1;
}

int
wasm_driver_call_unlocked(wasm_driver_t *driver,
                          const char *export_name,
                          uint32_t argc,
                          uint32_t *argv)
{
    if (!driver || !export_name || !driver->active || !driver->runtime) {
        return -1;
    }
    uint32_t previous_pid = wasm_driver_enter_runtime(driver);
    IM3Function func = NULL;
    M3Result res = m3_FindFunction(&func, driver->runtime, export_name);
    if (res) {
        log_wasm3_error("[wasm-driver] lookup failed: ", res, driver->runtime);
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }
    const void *args[4] = { &argv[0], &argv[1], &argv[2], &argv[3] };
    res = m3_Call(func, argc, args);
    if (res) {
        log_wasm3_error("[wasm-driver] call failed: ", res, driver->runtime);
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }
    wasm_driver_leave_runtime(previous_pid);
    return 0;
}
