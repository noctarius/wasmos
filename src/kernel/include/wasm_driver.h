/* wasm_driver.h - wasm3-backed driver instance management.
 *
 * Each WASM driver or service gets a wasm_driver_t that owns a wasm3 runtime,
 * module, and an IPC endpoint.  The PM creates one per spawned WASM payload
 * and calls wasm_driver_call_entry() to run the WASM entry export.
 * The lock serializes re-entrant hostcall calls from multiple kernel threads. */
#ifndef WASMOS_WASM_DRIVER_H
#define WASMOS_WASM_DRIVER_H

#include <stdint.h>
#include "ipc.h"
#include "spinlock.h"
#include "wasm3.h"

/* Parameters needed to instantiate and run a WASM module. */
typedef struct {
    const char *name;
    const uint8_t *module_bytes;
    uint32_t module_size;
    const char *entry_export;   /* exported function name called as the driver entry point */
    uint32_t stack_size;
    uint32_t heap_size;
    uint32_t entry_argc;
    const uint32_t *entry_argv;
} wasm_driver_manifest_t;

/* Live WASM driver instance.  One per spawned WASM driver or service. */
typedef struct {
    wasm_driver_manifest_t manifest;
    IM3Environment env;
    IM3Runtime runtime;
    IM3Module module;
    uint32_t owner_pid;
    uint32_t owner_context_id;
    uint32_t endpoint;   /* IPC endpoint for service requests */
    spinlock_t lock;     /* guards wasm3 runtime re-entrancy */
    uint8_t active;
} wasm_driver_t;

/* Initialize the WASM driver subsystem; called once during kernel startup. */
void wasm_driver_init(void);

/* Instantiate the wasm3 runtime and load the WASM module from manifest.
 * Registers all hostcall imports before returning. */
int wasm_driver_start(wasm_driver_t *driver,
                      const wasm_driver_manifest_t *manifest,
                      uint32_t owner_context_id);

void wasm_driver_stop(wasm_driver_t *driver);

/* Return the IPC endpoint number for driver in *out_endpoint. */
int wasm_driver_endpoint(const wasm_driver_t *driver, uint32_t *out_endpoint);

/* Call the entry export specified in driver->manifest (acquires lock). */
int wasm_driver_call_entry(wasm_driver_t *driver);

/* Call an arbitrary export by name with argc/argv i32 arguments (acquires lock). */
int wasm_driver_call(wasm_driver_t *driver,
                     const char *export_name,
                     uint32_t argc,
                     uint32_t *argv);

/* Same as wasm_driver_call but assumes the caller already holds driver->lock. */
int wasm_driver_call_unlocked(wasm_driver_t *driver,
                              const char *export_name,
                              uint32_t argc,
                              uint32_t *argv);

/* Spawn a kernel thread to run export_name in owner_pid's wasm3 runtime.
 * Used for threaded WASM workloads via the thread_create syscall. */
int wasm_driver_spawn_vm_thread(uint32_t owner_pid,
                                const char *export_name,
                                uint32_t argc,
                                const uint32_t *argv,
                                uint32_t *out_tid);

#endif
