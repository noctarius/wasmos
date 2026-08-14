/* wasm_driver.c - wasm3 WASM module loader and driver/service instance runner.
 * Instantiates a wasm3 environment and runtime per driver, registers all
 * hardware and IPC hostcall imports, and runs the module's entry export.  The
 * interpreter and its guest both execute in ring 0.
 *
 * driver->lock serializes wasm_driver_call_entry() and wasm_driver_call() for
 * one driver.  wasm_driver_call_unlocked() bypasses it by contract, and
 * wasm_driver_start()/_stop() do not take it at all. */
#include "wasm_driver.h"
#include "klog.h"
#include "process.h"
#include "serial.h"
#include "memory.h"
#include "paging.h"
#include "wasm3/link.h"
#include "wasm3/shim.h"
#include "thread.h"
#include "string.h"

typedef struct {
    uint8_t in_use;
    uint32_t owner_pid;
    wasm_driver_t* driver;
} wasm_driver_registry_slot_t;

static wasm_driver_registry_slot_t g_wasm_driver_registry[PROCESS_MAX_COUNT];
static ksync_spinlock_t g_wasm_driver_registry_lock;

/* Initialises the pid -> driver registry lock.  Must run once during kernel
 * bring-up, before any wasm_driver_start; the registry slots themselves are
 * static and start empty. */
void wasm_driver_init(void) {
    ksync_spinlock_init(&g_wasm_driver_registry_lock);
}

/* Per-pid runtime teardown hook (see process.h).  The wasm3 build has no
 * separate per-pid WARP state to release; wasm3's own per-pid tables are freed
 * via wasm3_heap_release().  No-op stub so process_reap() can call this
 * unconditionally.  The real implementation is in warp/link.cpp. */
void warp_release_pid(uint32_t pid) {
    (void)pid;
}

static void log_wasm3_error(const char* prefix, const char* error, IM3Runtime runtime) {
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

static void wasm_driver_reset(wasm_driver_t* driver) {
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

static uint32_t wasm_driver_enter_runtime(const wasm_driver_t* driver) {
    uint32_t pid = process_current_pid();
    if (driver && driver->owner_pid != 0) {
        pid = driver->owner_pid;
    }
    return wasm3_runtime_enter(pid);
}

static void wasm_driver_registry_set(uint32_t owner_pid, wasm_driver_t* driver) {
    if (owner_pid == 0 || !driver) {
        return;
    }
    ksync_spinlock_lock(&g_wasm_driver_registry_lock);
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_driver_registry[i].in_use && g_wasm_driver_registry[i].owner_pid == owner_pid) {
            g_wasm_driver_registry[i].driver = driver;
            ksync_spinlock_unlock(&g_wasm_driver_registry_lock);
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
    ksync_spinlock_unlock(&g_wasm_driver_registry_lock);
}

static void wasm_driver_registry_clear(uint32_t owner_pid, wasm_driver_t* driver) {
    if (owner_pid == 0 || !driver) {
        return;
    }
    ksync_spinlock_lock(&g_wasm_driver_registry_lock);
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (!g_wasm_driver_registry[i].in_use || g_wasm_driver_registry[i].owner_pid != owner_pid) {
            continue;
        }
        if (g_wasm_driver_registry[i].driver == driver) {
            g_wasm_driver_registry[i].in_use = 0;
            g_wasm_driver_registry[i].owner_pid = 0;
            g_wasm_driver_registry[i].driver = 0;
            break;
        }
    }
    ksync_spinlock_unlock(&g_wasm_driver_registry_lock);
}

static void wasm_driver_leave_runtime(uint32_t previous_pid) {
    wasm3_runtime_leave(previous_pid);
}

/* Builds a complete wasm3 instance in *driver: environment, runtime, parsed and
 * loaded module, linked host imports, and an IPC endpoint owned by
 * owner_context_id.  It does NOT run the entry export; wasm_driver_call_entry
 * does that.
 *
 * The manifest is copied by value, but its module_bytes and the string fields it
 * points at are BORROWED and must outlive the driver — the module bytes stay
 * referenced by the wasm3 runtime.
 *
 * The load switches CR3 to the owner's root and back, because linear memory
 * lives in the owner's user-VA window and m3_LoadModule writes through it.  The
 * caller can therefore be on any root.
 *
 * Returns 0 on success and -1 for a NULL driver or manifest, an empty module, a
 * zero owner context, or any allocation, parse, load, link or endpoint failure;
 * every failure path frees what it had already built, so *driver is left
 * inactive rather than half-constructed.  Does not take driver->lock: the driver
 * is not yet reachable from any caller. */
int wasm_driver_start(wasm_driver_t* driver, const wasm_driver_manifest_t* manifest,
                      uint32_t owner_context_id) {
    if (!driver || !manifest || !manifest->module_bytes || manifest->module_size == 0 ||
        owner_context_id == 0) {
        return -1;
    }

    wasm_driver_reset(driver);
    driver->manifest = *manifest;
    driver->owner_context_id = owner_context_id;
    ksync_mutex_init(&driver->lock);
    process_t* owner = process_find_by_context(owner_context_id);
    driver->owner_pid = owner ? owner->pid : process_current_pid();
    wasm3_heap_configure(driver->owner_pid, driver->manifest.heap_size,
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

    M3Result res = m3_ParseModule(driver->env, &driver->module, driver->manifest.module_bytes,
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

    /* Linear memory lives in the owner's user-VA window (PML4[1]):
     * m3_LoadModule -> InitMemory reserves it and writes the M3MemoryHeader and
     * data segments through that VA, so the owner address space must be ACTIVE
     * for the load.  Kernel-hosted drivers (e.g. chardev) run wasm_driver_start
     * from the kernel CR3, where PML4[1] is unmapped, so switch to the owner
     * root for the load and restore afterward.  No save/restore race: the
     * enclosing wasm_driver_enter_runtime disabled preemption and the load path
     * does not block. */
    uint64_t prev_root = paging_get_current_root_table();
    uint64_t owner_root = mm_context_root_table(owner_context_id);
    int switched_root = (owner_root != 0 && owner_root != prev_root);
    if (switched_root) {
        paging_switch_root(owner_root);
    }
    res = m3_LoadModule(driver->runtime, driver->module);
    if (switched_root) {
        paging_switch_root(prev_root);
    }
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
    /* Linear memory is bound to the owner's user-VA window when the block is
     * reserved during m3_LoadModule (wasm3_linmem_reserve) and re-bound per
     * committed tail on grow, so no post-load rebind is needed here.  A rebind
     * would also fault: m3_GetMemory dereferences the header through the user VA,
     * which is only mapped under the owner CR3 (this runs under the caller's). */

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
    trace_write("[wasm-driver] started\n");
    wasm_driver_leave_runtime(previous_pid);
    return 0;
}

/* Frees the wasm3 runtime and environment, unregisters the driver, and resets
 * the structure to inactive.  The driver's IPC endpoint is not destroyed here.
 *
 * Does NOT take driver->lock, so it must not race a wasm_driver_call on another
 * CPU — the caller has to have quiesced the driver first.  A NULL driver, and a
 * driver that was never started, are both no-ops. */
void wasm_driver_stop(wasm_driver_t* driver) {
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

/* Reports the driver's IPC endpoint.  Returns 0 with *out_endpoint set, or -1
 * for a NULL argument, a driver that is not active, or one without an endpoint —
 * the three are indistinguishable. */
int wasm_driver_endpoint(const wasm_driver_t* driver, uint32_t* out_endpoint) {
    if (!driver || !out_endpoint || !driver->active || driver->endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    *out_endpoint = driver->endpoint;
    return 0;
}

/* Runs the manifest's entry export with the manifest's entry_argv, under
 * driver->lock.
 *
 * BLOCKS for as long as the guest runs — a driver whose entry is an event loop
 * never returns from here — and holds the lock throughout, so no concurrent
 * wasm_driver_call can proceed meanwhile.  entry_argc above 4 is refused; a NULL
 * entry_argv is read as four zeros.
 *
 * Returns 0 when the guest returned normally, and -1 for an inactive driver, a
 * missing entry export, an argument count over 4, a failed lock, or a wasm3 trap
 * — the wasm3 error is logged, not returned. */
int wasm_driver_call_entry(wasm_driver_t* driver) {
    uint32_t argv[4] = {0, 0, 0, 0};
    uint32_t argc = 0;

    if (!driver || !driver->active || !driver->runtime || !driver->manifest.entry_export) {
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
    if (ksync_mutex_lock(&driver->lock) != KSYNC_MUTEX_OK) {
        return -1;
    }
    uint32_t previous_pid = wasm_driver_enter_runtime(driver);
    M3Result res = m3_FindFunction(&func, driver->runtime, driver->manifest.entry_export);
    if (res) {
        log_wasm3_error("[wasm-driver] entry lookup failed: ", res, driver->runtime);
        wasm_driver_leave_runtime(previous_pid);
        (void)ksync_mutex_unlock(&driver->lock);
        return -1;
    }
    const void* args[4] = {&argv[0], &argv[1], &argv[2], &argv[3]};
    res = m3_Call(func, argc, args);
    if (res) {
        log_wasm3_error("[wasm-driver] entry failed: ", res, driver->runtime);
        wasm_driver_leave_runtime(previous_pid);
        (void)ksync_mutex_unlock(&driver->lock);
        return -1;
    }
    wasm_driver_leave_runtime(previous_pid);
    (void)ksync_mutex_unlock(&driver->lock);
    return 0;
}

/* Calls a named export under driver->lock, blocking until the guest returns.
 *
 * argv must point at an array of at least FOUR uint32_t regardless of argc, and
 * argc must not exceed 4: the argument pointers handed to wasm3 are always taken
 * from argv[0..3].  Unlike wasm_driver_call_entry, this does not check argc.
 * The values are passed by address, so a guest can write back through them.
 *
 * Returns 0 on success and -1 for a NULL driver or export name, an inactive
 * driver, a failed lock, an export that does not exist, or a trap during the
 * call; the wasm3 message is logged rather than returned. */
int wasm_driver_call(wasm_driver_t* driver, const char* export_name, uint32_t argc,
                     uint32_t* argv) {
    if (!driver || !export_name || !driver->active || !driver->runtime) {
        return -1;
    }

    if (ksync_mutex_lock(&driver->lock) != KSYNC_MUTEX_OK) {
        return -1;
    }
    uint32_t previous_pid = wasm_driver_enter_runtime(driver);
    IM3Function func = NULL;
    M3Result res = m3_FindFunction(&func, driver->runtime, export_name);
    int ok = 0;
    if (res) {
        log_wasm3_error("[wasm-driver] lookup failed: ", res, driver->runtime);
    } else {
        const void* args[4] = {&argv[0], &argv[1], &argv[2], &argv[3]};
        res = m3_Call(func, argc, args);
        ok = res ? -1 : 0;
        if (res) {
            log_wasm3_error("[wasm-driver] call failed: ", res, driver->runtime);
        }
    }
    wasm_driver_leave_runtime(previous_pid);
    (void)ksync_mutex_unlock(&driver->lock);
    return ok == 0 ? 0 : -1;
}

/* wasm_driver_call without driver->lock.  It exists for re-entrant call sites:
 * a host call made from inside a running export cannot retake a lock the same
 * thread already holds.  The caller therefore GUARANTEES it is already inside
 * that driver, and using it from anywhere else races every other caller.
 *
 * Same argv/argc requirements and same return convention as wasm_driver_call. */
int wasm_driver_call_unlocked(wasm_driver_t* driver, const char* export_name, uint32_t argc,
                              uint32_t* argv) {
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
    const void* args[4] = {&argv[0], &argv[1], &argv[2], &argv[3]};
    res = m3_Call(func, argc, args);
    if (res) {
        log_wasm3_error("[wasm-driver] call failed: ", res, driver->runtime);
        wasm_driver_leave_runtime(previous_pid);
        return -1;
    }

    int32_t ret_i32 = 0;
    const void* ret_ptrs[1] = {&ret_i32};
    (void)m3_GetResults(func, 1, ret_ptrs);

    wasm_driver_leave_runtime(previous_pid);
    return 0;
}
