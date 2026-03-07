#include "wasm_driver.h"
#include "serial.h"
#include "wamr_context.h"

static uint8_t g_runtime_ready;

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
        manifest->module_size == 0 || !manifest->dispatch_export ||
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
        serial_write("[wasm-driver] load failed\n");
        return -1;
    }

    if (!wamr_instantiate_module(driver->module,
                                 driver->manifest.stack_size,
                                 driver->manifest.heap_size,
                                 &driver->instance,
                                 error_buf,
                                 sizeof(error_buf))) {
        serial_write("[wasm-driver] instantiate failed\n");
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

    if (driver->manifest.init_export) {
        uint32_t argv[1] = { 0 };
        (void)wamr_call_function(driver->instance,
                                 driver->manifest.init_export,
                                 0,
                                 argv,
                                 0);
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
wasm_driver_dispatch(wasm_driver_t *driver,
                     const ipc_message_t *request,
                     int32_t *out_value)
{
    uint32_t argv[5];

    if (!driver || !request || !out_value || !driver->active || !driver->instance) {
        return -1;
    }

    argv[0] = request->type;
    argv[1] = request->arg0;
    argv[2] = request->arg1;
    argv[3] = request->arg2;
    argv[4] = request->arg3;

    spinlock_lock(&driver->lock);
    int ok = wamr_call_function(driver->instance,
                                driver->manifest.dispatch_export,
                                5,
                                argv,
                                0);
    spinlock_unlock(&driver->lock);
    if (!ok) {
        serial_write("[wasm-driver] dispatch failed\n");
        return -1;
    }

    *out_value = (int32_t)argv[0];
    return 0;
}
