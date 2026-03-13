#include "wasm_driver.h"
#include "serial.h"
#include "wasm3_link.h"

static void
log_wasm3_error(const char *prefix, const char *error)
{
    serial_write(prefix);
    if (error && error[0]) {
        serial_write(error);
    } else {
        serial_write("unknown");
    }
    serial_write("\n");
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
    driver->owner_context_id = 0;
    driver->endpoint = IPC_ENDPOINT_NONE;
    driver->active = 0;
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

    driver->env = m3_NewEnvironment();
    if (!driver->env) {
        serial_write("[wasm-driver] env alloc failed\n");
        return -1;
    }

    uint32_t stack_size = driver->manifest.stack_size ? driver->manifest.stack_size : (64u * 1024u);
    driver->runtime = m3_NewRuntime(driver->env, stack_size, NULL);
    if (!driver->runtime) {
        serial_write("[wasm-driver] runtime alloc failed\n");
        m3_FreeEnvironment(driver->env);
        driver->env = 0;
        return -1;
    }

    M3Result res = m3_ParseModule(driver->env, &driver->module,
                                  driver->manifest.module_bytes,
                                  driver->manifest.module_size);
    if (res) {
        log_wasm3_error("[wasm-driver] parse failed: ", res);
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        driver->runtime = 0;
        driver->env = 0;
        return -1;
    }

    res = m3_LoadModule(driver->runtime, driver->module);
    if (res) {
        log_wasm3_error("[wasm-driver] load failed: ", res);
        m3_FreeModule(driver->module);
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        driver->runtime = 0;
        driver->env = 0;
        driver->module = 0;
        return -1;
    }

    if (wasm3_link_wasmos(driver->module) != 0 || wasm3_link_env(driver->module) != 0) {
        serial_write("[wasm-driver] link failed\n");
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        driver->runtime = 0;
        driver->env = 0;
        driver->module = 0;
        return -1;
    }

    if (ipc_endpoint_create(owner_context_id, &driver->endpoint) != 0) {
        serial_write("[wasm-driver] endpoint allocation failed\n");
        m3_FreeRuntime(driver->runtime);
        m3_FreeEnvironment(driver->env);
        wasm_driver_reset(driver);
        return -1;
    }

    driver->active = 1;
    serial_write("[wasm-driver] started\n");
    return 0;
}

void
wasm_driver_stop(wasm_driver_t *driver)
{
    if (!driver) {
        return;
    }
    if (driver->runtime) {
        m3_FreeRuntime(driver->runtime);
    }
    if (driver->env) {
        m3_FreeEnvironment(driver->env);
    }
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
        serial_write("[wasm-driver] entry args too large\n");
        return -1;
    }
    for (uint32_t i = 0; i < argc; ++i) {
        argv[i] = driver->manifest.entry_argv ? driver->manifest.entry_argv[i] : 0;
    }

    IM3Function func = NULL;
    M3Result res = m3_FindFunction(&func, driver->runtime, driver->manifest.entry_export);
    if (res) {
        log_wasm3_error("[wasm-driver] entry lookup failed: ", res);
        return -1;
    }
    const void *args[4] = { &argv[0], &argv[1], &argv[2], &argv[3] };
    res = m3_Call(func, argc, args);
    if (res) {
        log_wasm3_error("[wasm-driver] entry failed: ", res);
        return -1;
    }
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
    IM3Function func = NULL;
    M3Result res = m3_FindFunction(&func, driver->runtime, export_name);
    int ok = 0;
    if (res) {
        log_wasm3_error("[wasm-driver] lookup failed: ", res);
    } else {
        const void *args[4] = { &argv[0], &argv[1], &argv[2], &argv[3] };
        res = m3_Call(func, argc, args);
        ok = res ? -1 : 0;
        if (res) {
            log_wasm3_error("[wasm-driver] call failed: ", res);
        }
    }
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
    IM3Function func = NULL;
    M3Result res = m3_FindFunction(&func, driver->runtime, export_name);
    if (res) {
        log_wasm3_error("[wasm-driver] lookup failed: ", res);
        return -1;
    }
    const void *args[4] = { &argv[0], &argv[1], &argv[2], &argv[3] };
    res = m3_Call(func, argc, args);
    if (res) {
        log_wasm3_error("[wasm-driver] call failed: ", res);
        return -1;
    }
    return 0;
}
