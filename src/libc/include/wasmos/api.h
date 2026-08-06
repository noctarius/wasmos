/* api.h - WASM import declarations for all WASMOS host system calls.
 * Each extern function maps to a kernel-provided import in the "wasmos" module.
 * These are the sole entry points from WASM user space into the kernel.
 *
 * The per-call import declarations are GENERATED from abi/hostcalls.yaml (the
 * single source of truth) and pulled in via the #include below; edit the IDL +
 * run scripts/gen_abi_hostcalls.py, never hand-edit the declaration list. This
 * header supplies the C-only context the generated decls need: the struct
 * typedefs some calls take by reference, the shared #defines, and the two
 * native-only user-mutex imports (which are not WASM host calls). */
#ifndef WASMOS_LIBC_WASMOS_API_H
#define WASMOS_LIBC_WASMOS_API_H

#include "wasmos/imports.h"
/* Packed error codes returned by host calls (single-sourced in
 * abi/errors.yaml); host-call returns are a subsystem boundary, so they
 * carry a named code rather than a bare -1. */
#include "../../../../abi/generated/c/wasmos_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASMOS_BUFFER_KIND_XFER 1
#define WASMOS_BUFFER_GRANT_READ 0x1
#define WASMOS_BUFFER_GRANT_WRITE 0x2

/* Physical memory snapshot: total/free bytes as seen by the kernel PFA. */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
} wasmos_physmem_stats_t;

/* One allocated message-signalled interrupt vector, filled by wasmos_msi_alloc.
 * address/data are the opaque interrupt-controller pair a device must write to
 * raise `vector`; a driver forwards them verbatim to the bus driver that owns
 * config space (pci-bus) and never interprets them. */
typedef struct {
    uint32_t address_lo;
    uint32_t address_hi;
    uint32_t data;
    uint32_t vector;
} wasmos_msi_desc_t;

/* Per-process statistics returned by wasmos_proc_info_stats.
 * cpu_ticks: scheduler ticks attributed to this process.
 * rss_est_bytes: estimated resident set size (committed heap + kstack). */
typedef struct {
    uint32_t state;
    uint32_t block_reason;
    char runtime_tag[8];
    uint32_t thread_count;
    uint32_t live_thread_count;
    uint32_t current_tid;
    uint32_t context_id;
    uint64_t cpu_ticks;
    uint64_t vm_total_bytes;
    uint64_t thread_kstack_total_bytes;
    uint64_t heap_committed_bytes;
    uint64_t rss_est_bytes;
    uint32_t last_cpu;
} wasmos_proc_stats_t;

typedef struct {
    uint32_t ready_count;
    uint32_t running_pid;
    uint32_t steal_count;
    uint32_t dispatch_count;
    uint32_t last_pid;
} wasmos_sched_cpu_stats_t;

typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_gop_pixel_format;
} wasmos_framebuffer_info_t;

/* GENERATED "wasmos"-module host-call import declarations (source of truth:
 * abi/hostcalls.yaml via scripts/gen_abi_hostcalls.py). Included by relative
 * path so the generated header (which lives outside src/, out of the
 * format/lint scope) does not require threading -Iabi/generated/c through every
 * app/driver/service/test compile that pulls in libc. Must follow the struct
 * typedefs above (some decls take them by reference) and the WASMOS_WASM_IMPORT
 * macro from wasmos/imports.h. */
#include "../../../../abi/generated/c/wasmos_imports.h"

/* Kernel-backed recursive mutex using the wasmos_mutex_t struct in WASM memory.
 * mutex_ptr is the int32 WASM linear-memory address of the wasmos_mutex_t.
 * try_lock returns 0=acquired, 1=contended (caller must retry), <0=error.
 * unlock returns 0=released, <0=error (e.g. not the owner).
 *
 * NB: these are NOT WASM host calls — they resolve to driver_api vtable entries
 * (native_driver.c) on the native build via the WASMOS_WASM_IMPORT no-op shim,
 * so they are hand-declared here rather than in the IDL.
 * TODO(user-mutex-futex): migrate the contended path onto futex_wait/futex_wake
 * so user mutexes stop yield-spinning (see wasmos/mutex.h). */
extern int32_t wasmos_mutex_try_lock_host(int32_t mutex_ptr)
    WASMOS_WASM_IMPORT("wasmos", "mutex_try_lock");
extern int32_t wasmos_mutex_unlock_host(int32_t mutex_ptr)
    WASMOS_WASM_IMPORT("wasmos", "mutex_unlock");

#ifdef __cplusplus
}
#endif

#endif
