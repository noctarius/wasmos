#include "wasm_driver.h"
#include "klog.h"
#include "process.h"
#include "wasm3_link.h"
#include "wasm3_shim.h"
#include "thread.h"
#include "string.h"

#define WASM_DRIVER_THREAD_SLOTS 64u

typedef struct {
    uint8_t in_use;
    uint32_t owner_pid;
    uint32_t owner_context_id;
    const uint8_t *module_bytes;
    uint32_t module_size;
    uint32_t stack_size;
    uint32_t heap_size;
    char export_name[64];
    uint32_t argc;
    uint32_t argv[4];
} wasm_driver_thread_slot_t;

typedef struct {
    uint8_t in_use;
    uint32_t owner_pid;
    wasm_driver_t *driver;
} wasm_driver_registry_slot_t;

static wasm_driver_thread_slot_t g_wasm_driver_thread_slots[WASM_DRIVER_THREAD_SLOTS];
static wasm_driver_registry_slot_t g_wasm_driver_registry[PROCESS_MAX_COUNT];

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
wasm_driver_registry_set(uint32_t owner_pid, wasm_driver_t *driver)
{
    if (owner_pid == 0 || !driver) {
        return;
    }
    critical_section_enter();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_driver_registry[i].in_use &&
            g_wasm_driver_registry[i].owner_pid == owner_pid) {
            g_wasm_driver_registry[i].driver = driver;
            critical_section_leave();
            return;
        }
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (!g_wasm_driver_registry[i].in_use) {
            g_wasm_driver_registry[i].in_use = 1;
            g_wasm_driver_registry[i].owner_pid = owner_pid;
            g_wasm_driver_registry[i].driver = driver;
            break;
        }
    }
    critical_section_leave();
}

static void
wasm_driver_registry_clear(uint32_t owner_pid, wasm_driver_t *driver)
{
    if (owner_pid == 0 || !driver) {
        return;
    }
    critical_section_enter();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (!g_wasm_driver_registry[i].in_use ||
            g_wasm_driver_registry[i].owner_pid != owner_pid) {
            continue;
        }
        if (g_wasm_driver_registry[i].driver == driver) {
            g_wasm_driver_registry[i].in_use = 0;
            g_wasm_driver_registry[i].owner_pid = 0;
            g_wasm_driver_registry[i].driver = 0;
            break;
        }
    }
    critical_section_leave();
}

static wasm_driver_t *
wasm_driver_registry_get(uint32_t owner_pid)
{
    wasm_driver_t *driver = 0;
    if (owner_pid == 0) {
        return 0;
    }
    critical_section_enter();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_driver_registry[i].in_use &&
            g_wasm_driver_registry[i].owner_pid == owner_pid) {
            driver = g_wasm_driver_registry[i].driver;
            break;
        }
    }
    critical_section_leave();
    return driver;
}

static wasm_driver_thread_slot_t *
wasm_driver_thread_slot_alloc(void)
{
    wasm_driver_thread_slot_t *slot = 0;
    critical_section_enter();
    for (uint32_t i = 0; i < WASM_DRIVER_THREAD_SLOTS; ++i) {
        if (!g_wasm_driver_thread_slots[i].in_use) {
            slot = &g_wasm_driver_thread_slots[i];
            slot->in_use = 1;
            break;
        }
    }
    critical_section_leave();
    return slot;
}

static void
wasm_driver_thread_slot_free(wasm_driver_thread_slot_t *slot)
{
    if (!slot) {
        return;
    }
    critical_section_enter();
    *slot = (wasm_driver_thread_slot_t){0};
    critical_section_leave();
}

static process_run_result_t
wasm_driver_vm_thread_entry(process_t *process, uint32_t tid, void *arg)
{
    wasm_driver_thread_slot_t *slot = (wasm_driver_thread_slot_t *)arg;
    IM3Environment env = 0;
    IM3Runtime runtime = 0;
    IM3Module module = 0;
    IM3Function func = 0;
    M3Result res = 0;
    int rc = -1;
    uint32_t bind_prev = 0xFFFFFFFFu;
    const void *call_args[4];
    (void)tid;
    if (!process || !slot || !slot->in_use) {
        return PROCESS_RUN_EXITED;
    }
    call_args[0] = &slot->argv[0];
    call_args[1] = &slot->argv[1];
    call_args[2] = &slot->argv[2];
    call_args[3] = &slot->argv[3];
    wasm3_heap_configure(slot->owner_pid, slot->heap_size, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    preempt_disable();
    bind_prev = wasm3_heap_bind_pid(slot->owner_pid);
    env = m3_NewEnvironment();
    if (!env) {
        goto out;
    }
    runtime = m3_NewRuntime(env, slot->stack_size ? slot->stack_size : (64u * 1024u), 0);
    if (!runtime) {
        goto out;
    }
    res = m3_ParseModule(env, &module, slot->module_bytes, slot->module_size);
    if (res) {
        goto out;
    }
    res = m3_LoadModule(runtime, module);
    if (res) {
        goto out;
    }
    if (wasm3_link_wasmos(module) != 0 || wasm3_link_env(module) != 0) {
        goto out;
    }
    res = m3_FindFunction(&func, runtime, slot->export_name);
    if (res) {
        goto out;
    }
    res = m3_Call(func, slot->argc, call_args);
    if (res) {
        goto out;
    }
    rc = 0;
out:
    if (runtime) {
        m3_FreeRuntime(runtime);
    } else if (module) {
        m3_FreeModule(module);
    }
    if (env) {
        m3_FreeEnvironment(env);
    }
    if (bind_prev != 0xFFFFFFFFu) {
        wasm3_heap_restore_pid(bind_prev);
    }
    preempt_enable();
    process_set_exit_status(process, rc);
    wasm_driver_thread_slot_free(slot);
    return PROCESS_RUN_THREAD_EXITED;
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
    wasm_driver_registry_set(driver->owner_pid, driver);
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
    wasm_driver_registry_clear(driver->owner_pid, driver);
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

int
wasm_driver_spawn_vm_thread(uint32_t owner_pid,
                            const char *export_name,
                            uint32_t argc,
                            const uint32_t *argv,
                            uint32_t *out_tid)
{
    wasm_driver_t *driver = 0;
    wasm_driver_thread_slot_t *slot = 0;
    uint32_t tid = 0;
    if (owner_pid == 0 || !export_name || !out_tid || argc > 4u) {
        return -1;
    }
    driver = wasm_driver_registry_get(owner_pid);
    if (!driver || !driver->active || !driver->manifest.module_bytes ||
        driver->manifest.module_size == 0 || driver->owner_context_id == 0) {
        return -1;
    }
    slot = wasm_driver_thread_slot_alloc();
    if (!slot) {
        return -1;
    }
    slot->owner_pid = owner_pid;
    slot->owner_context_id = driver->owner_context_id;
    slot->module_bytes = driver->manifest.module_bytes;
    slot->module_size = driver->manifest.module_size;
    slot->stack_size = driver->manifest.stack_size ? driver->manifest.stack_size : (64u * 1024u);
    slot->heap_size = driver->manifest.heap_size ? driver->manifest.heap_size : (64u * 1024u);
    slot->argc = argc;
    for (uint32_t i = 0; i < argc; ++i) {
        slot->argv[i] = argv ? argv[i] : 0;
    }
    if (str_copy_bytes(slot->export_name,
                       sizeof(slot->export_name),
                       (const uint8_t *)export_name,
                       strlen(export_name)) != 0) {
        wasm_driver_thread_slot_free(slot);
        return -1;
    }
    if (process_thread_spawn_worker_internal(owner_pid,
                                             "wasm-vm-thread",
                                             wasm_driver_vm_thread_entry,
                                             slot,
                                             &tid) != 0) {
        wasm_driver_thread_slot_free(slot);
        return -1;
    }
    *out_tid = tid;
    return 0;
}
