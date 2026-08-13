/* wasm_driver.h - WASM module loader and driver/service instance runner.
 *
 * Supports two runtime backends selected at compile time via WASMOS_WASM_RUNTIME:
 *   0 = wasm3 interpreter (default)
 *   1 = WARP single-pass JIT compiler
 *
 * The public API is identical for both backends.  Internal fields of
 * wasm_driver_t differ but are opaque to callers outside wasm_driver.c /
 * warp_driver.cpp. */
#ifndef WASMOS_WASM_DRIVER_H
#define WASMOS_WASM_DRIVER_H

#include <stdint.h>
#include "ipc.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"

/* Parameters needed to instantiate and run a WASM module. */
typedef struct {
    const char* name;
    const uint8_t* module_bytes;
    uint32_t module_size;
    const uint8_t* compiled_bytes; /* pre-compiled WARP native binary; NULL if JIT */
    uint32_t compiled_size;
    const char* entry_export; /* exported function name called as the driver entry point */
    uint32_t stack_size;
    uint32_t heap_size;
    uint32_t entry_argc;
    const uint32_t* entry_argv;
} wasm_driver_manifest_t;

#if WASMOS_WASM_RUNTIME == 1 /* WARP JIT backend */

/* Live WASM driver instance backed by vb::WasmModule (C++ object).
 * wasm_module is a heap-allocated vb::WasmModule * cast to void *. */
typedef struct {
    wasm_driver_manifest_t manifest;
    void* wasm_module; /* vb::WasmModule * */
    uint32_t owner_pid;
    uint32_t owner_context_id;
    uint32_t endpoint;     /* IPC endpoint for service requests */
    ksync_spinlock_t lock; /* guards WARP module re-entrancy */
    uint8_t active;
    uint8_t started;
#ifdef WASMOS_WASM_RUNTIME_WARP
    uint64_t r3_user_root;   /* per-module user CR3 for ring-3 execution */
    uint64_t r3_stack_phys;  /* per-module ring-3 stack physical base */
    uint64_t r3_linmem_base; /* per-module user VA for WARP linMem register */
#endif
} wasm_driver_t;

#else /* wasm3 interpreter backend (default) */

#include "wasm3.h"

/* Live WASM driver instance backed by the wasm3 interpreter. */
typedef struct {
    wasm_driver_manifest_t manifest;
    IM3Environment env;
    IM3Runtime runtime;
    IM3Module module;
    uint32_t owner_pid;
    uint32_t owner_context_id;
    uint32_t endpoint;  /* IPC endpoint for service requests */
    ksync_mutex_t lock; /* serializes wasm3 runtime entry for this driver */
    uint8_t active;
} wasm_driver_t;

#endif /* WASMOS_WASM_RUNTIME */

/* Initialize the WASM driver subsystem; called once during kernel startup. */
void wasm_driver_init(void);

/* Instantiate the configured runtime backend and load the WASM module described
 * by manifest, registering all host-call imports before returning. The manifest
 * is copied into *driver, but the pointers it holds (name, module_bytes,
 * entry_export, entry_argv) are borrowed and must outlive the driver. Returns 0
 * on success, -1 on failure. */
int wasm_driver_start(wasm_driver_t* driver, const wasm_driver_manifest_t* manifest,
                      uint32_t owner_context_id);

void wasm_driver_stop(wasm_driver_t* driver);

/* Return the IPC endpoint number for driver in *out_endpoint.
 * Returns 0 on success, -1 if the driver is not started. */
int wasm_driver_endpoint(const wasm_driver_t* driver, uint32_t* out_endpoint);

/* Call the entry export named in driver->manifest, serializing per-driver
 * execution. Returns 0 when the call completed, -1 if the driver is inactive,
 * the export is missing, manifest.entry_argc exceeds 4, or the module trapped. */
int wasm_driver_call_entry(wasm_driver_t* driver);

/* Call an arbitrary export by name with argc i32 arguments from argv,
 * serializing per-driver execution. Returns 0 when the call completed, -1 if
 * the driver is inactive, the export is missing, or the module trapped.
 * FIXME: the wasm3 backend marshals through a fixed 4-slot argument array and
 * does not bounds-check argc (wasm_driver_call_entry does), so an argc above 4
 * reads past that array. Keep argc <= 4 and make argv 4 elements wide. */
int wasm_driver_call(wasm_driver_t* driver, const char* export_name, uint32_t argc, uint32_t* argv);

/* Same as wasm_driver_call but bypasses the per-driver execution lock.
 * Callers must provide equivalent higher-level serialization if needed. */
int wasm_driver_call_unlocked(wasm_driver_t* driver, const char* export_name, uint32_t argc,
                              uint32_t* argv);

#endif
