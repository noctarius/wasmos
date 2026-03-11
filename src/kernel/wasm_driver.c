#include "wasm_driver.h"
#include "serial.h"
#include "wamr_context.h"

static uint8_t g_runtime_ready;

static void
log_wamr_error(const char *prefix, const char *error)
{
    serial_write(prefix);
    if (error && error[0]) {
        serial_write(error);
    }
    else {
        serial_write("unknown");
    }
    serial_write("\n");
}

int
wasm_driver_runtime_ensure(void)
{
    if (g_runtime_ready) {
        return 0;
    }
    if (wamr_context_init() != 0) {
        serial_write("[wasm-driver] runtime init failed\n");
        return -1;
    }
    g_runtime_ready = 1;
    serial_write("[wasm-driver] runtime ready\n");
    return 0;
}

static void
wasm_driver_reset(wasm_driver_t *driver)
{
    if (!driver) {
        return;
    }
    driver->module = 0;
    driver->instance = 0;
    driver->owner_context_id = 0;
    driver->endpoint = IPC_ENDPOINT_NONE;
    driver->active = 0;
}

int
wasm_driver_start(wasm_driver_t *driver,
                  const wasm_driver_manifest_t *manifest,
                  uint32_t owner_context_id)
{
    char error_buf[128];

    if (!driver || !manifest || !manifest->module_bytes ||
        manifest->module_size == 0 ||
        owner_context_id == 0) {
        return -1;
    }

    wasm_driver_reset(driver);
    driver->manifest = *manifest;
    driver->owner_context_id = owner_context_id;
    spinlock_init(&driver->lock);

    if (wasm_driver_runtime_ensure() != 0) {
        return -1;
    }

    if (!wamr_load_module(driver->manifest.module_bytes, driver->manifest.module_size,
                          &driver->module, error_buf, sizeof(error_buf))) {
        log_wamr_error("[wasm-driver] load failed: ", error_buf);
        return -1;
    }

    if (!wamr_instantiate_module(driver->module,
                                 driver->manifest.stack_size,
                                 driver->manifest.heap_size,
                                 &driver->instance,
                                 error_buf,
                                 sizeof(error_buf))) {
        log_wamr_error("[wasm-driver] instantiate failed: ", error_buf);
        wamr_unload_module(driver->module);
        driver->module = 0;
        return -1;
    }

    if (ipc_endpoint_create(owner_context_id, &driver->endpoint) != 0) {
        serial_write("[wasm-driver] endpoint allocation failed\n");
        wamr_deinstantiate_module(driver->instance);
        wamr_unload_module(driver->module);
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
    if (driver->instance) {
        wamr_deinstantiate_module(driver->instance);
    }
    if (driver->module) {
        wamr_unload_module(driver->module);
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

    if (!driver || !driver->active || !driver->instance ||
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

    int ok = wamr_call_function(driver->instance,
                                driver->manifest.entry_export,
                                argc,
                                argv,
                                0);
    if (!ok) {
        serial_write("[wasm-driver] entry failed\n");
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
    if (!driver || !export_name || !driver->active || !driver->instance) {
        return -1;
    }

    spinlock_lock(&driver->lock);
    int ok = wamr_call_function(driver->instance,
                                export_name,
                                argc,
                                argv,
                                0);
    spinlock_unlock(&driver->lock);
    return ok ? 0 : -1;
}

int
wasm_driver_call_unlocked(wasm_driver_t *driver,
                          const char *export_name,
                          uint32_t argc,
                          uint32_t *argv)
{
    if (!driver || !export_name || !driver->active || !driver->instance) {
        return -1;
    }
    int ok = wamr_call_function(driver->instance,
                                export_name,
                                argc,
                                argv,
                                0);
    if (!ok) {
        serial_write("[wasm-driver] call failed export=");
        serial_write(export_name);
        serial_write("\n");
    } else {
        serial_write("[wasm-driver] call ok export=");
        serial_write(export_name);
        serial_write("\n");
    }
    return ok ? 0 : -1;
}
