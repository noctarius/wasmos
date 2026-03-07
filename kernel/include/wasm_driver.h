#ifndef WASMOS_WASM_DRIVER_H
#define WASMOS_WASM_DRIVER_H

#include <stdint.h>
#include "ipc.h"
#include "spinlock.h"
#include "wamr_runtime.h"

typedef struct {
    const char *name;
    const uint8_t *module_bytes;
    uint32_t module_size;
    const char *init_export;
    const char *dispatch_export;
    uint32_t stack_size;
    uint32_t heap_size;
} wasm_driver_manifest_t;

typedef struct {
    wasm_driver_manifest_t manifest;
    wamr_module_t *module;
    wamr_instance_t *instance;
    uint32_t owner_context_id;
    uint32_t endpoint;
    spinlock_t lock;
    uint8_t active;
} wasm_driver_t;

int wasm_driver_runtime_ensure(void);
int wasm_driver_start(wasm_driver_t *driver,
                      const wasm_driver_manifest_t *manifest,
                      uint32_t owner_context_id);
void wasm_driver_stop(wasm_driver_t *driver);
int wasm_driver_endpoint(const wasm_driver_t *driver, uint32_t *out_endpoint);
int wasm_driver_dispatch(wasm_driver_t *driver,
                         const ipc_message_t *request,
                         int32_t *out_value);

#endif
