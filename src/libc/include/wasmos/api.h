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

/* Buffer-object kind selector for the generic wasmos_buffer_* host calls (the
 * xfer-buffer kind is the only one the kernel implements; any other value is
 * rejected with WASMOS_ERR_XFER_BUFFER_INVALID_KIND). The wasmos_xfer_buffer_*
 * calls are the kind-less shorthand for the same objects and take no kind. */
#define WASMOS_BUFFER_KIND_XFER 1
/* Access rights passed as the `flags` bitmask of buffer_borrow/reborrow: READ
 * lets the grantee call xfer_buffer_read, WRITE lets it call xfer_buffer_write.
 * The mask must be non-zero and contain no other bits, and a reborrow may only
 * narrow (never widen) the rights of the borrow it derives from. */
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

/* Per-process statistics returned by wasmos_proc_info_stats. The layout mirrors
 * the kernel's process_stats_t; it is a snapshot taken while the process table
 * is walked and is stale the moment it lands in guest memory.
 *
 * state: kernel process_state_t (1=READY, 2=RUNNING, 3=BLOCKED, 4=ZOMBIE,
 *     7=NEW); enumeration skips free and mid-reap slots, so those never appear.
 * block_reason: kernel process_block_reason_t (0=none, 1=IPC, 2=wait).
 * runtime_tag: subsystem/runtime label ("WARP", "KERNEL", …), NUL-padded but
 *     NOT NUL-terminated when the tag uses all 8 bytes.
 * current_tid: the TID this process is running on the reporting CPU right now,
 *     or 0 when it is not the CPU's current process.
 * cpu_ticks: scheduler ticks summed over the process's threads.
 * vm_total_bytes: mapped context memory (linear memory + code + stacks).
 * thread_kstack_total_bytes: kernel stacks of all live threads.
 * heap_committed_bytes: runtime heap committed for this pid (wasm3 plus native
 *     driver heaps).
 * rss_est_bytes: resident-set estimate; currently equal to vm_total_bytes,
 *     since per-page presence is not tracked yet.
 * last_cpu: CPU id the main thread last ran on. */
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

/* Per-CPU scheduler counters filled by wasmos_sched_cpu_stats(cpu_id, out).
 * ready_count is the number of ready threads queued on that CPU summed over all
 * priority bands; running_pid is 0 when the CPU runs no process; steal_count and
 * dispatch_count are running totals (threads stolen from another CPU's queue,
 * and threads dispatched here); last_pid is the pid dispatched most recently. */
typedef struct {
    uint32_t ready_count;
    uint32_t running_pid;
    uint32_t steal_count;
    uint32_t dispatch_count;
    uint32_t last_pid;
} wasmos_sched_cpu_stats_t;

/* GOP framebuffer geometry filled by wasmos_framebuffer_info. framebuffer_base
 * is the PHYSICAL base (usable with wasmos_framebuffer_map, not dereferenceable
 * from a guest); framebuffer_size is in bytes; framebuffer_stride is PIXELS per
 * scanline and is >= framebuffer_width; framebuffer_gop_pixel_format is the raw
 * EFI_GRAPHICS_PIXEL_FORMAT value reported by firmware. */
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
 * Declared here rather than in abi/hostcalls.yaml, and exempted from the
 * --verify-source client guard by C_CLIENT_ALLOWLIST in
 * scripts/gen_abi_hostcalls.py. Only wasmos/mutex.h calls them, and only under
 * __wasm__; the x86_64 native path uses the int-0x80 syscalls
 * (wasmos_sys_mutex_try_lock/unlock) and native drivers use the
 * mutex_try_lock/mutex_unlock entries of the wasmos_driver_api_t vtable.
 * FIXME(user-mutex-import): no kernel link table exports "wasmos"."mutex_try_lock"
 * or "mutex_unlock", so a WASM guest that actually calls these fails to
 * instantiate on an unresolved import.
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
