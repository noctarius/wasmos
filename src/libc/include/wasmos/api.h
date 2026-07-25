/* api.h - WASM import declarations for all WASMOS host system calls.
 * Each extern function maps to a kernel-provided import in the "wasmos" module.
 * These are the sole entry points from WASM user space into the kernel. */
#ifndef WASMOS_LIBC_WASMOS_API_H
#define WASMOS_LIBC_WASMOS_API_H

#include "wasmos/imports.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASMOS_BUFFER_KIND_XFER 1
#define WASMOS_BUFFER_GRANT_READ 0x1
#define WASMOS_BUFFER_GRANT_WRITE 0x2

extern int32_t wasmos_console_read(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_read");
extern int32_t wasmos_console_write(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_write");
extern int32_t wasmos_sync_user_read(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "sync_user_read");
extern int32_t wasmos_debug_mark(int32_t tag) WASMOS_WASM_IMPORT("wasmos", "debug_mark");
extern int32_t wasmos_kmap_dump(void) WASMOS_WASM_IMPORT("wasmos", "kmap_dump");
extern int32_t wasmos_kmap_dump_all(void) WASMOS_WASM_IMPORT("wasmos", "kmap_dump_all");
extern int32_t wasmos_ipc_create_endpoint(void) WASMOS_WASM_IMPORT("wasmos", "ipc_create_endpoint");
extern int32_t wasmos_ipc_endpoint_owner(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_endpoint_owner");
extern int32_t wasmos_ipc_send(int32_t destination_endpoint, int32_t source_endpoint, int32_t type,
                               int32_t request_id, int32_t arg0, int32_t arg1, int32_t arg2,
                               int32_t arg3) WASMOS_WASM_IMPORT("wasmos", "ipc_send");
/* Object/owner/borrow xfer-buffer ABI (stateless, id-based, capability-style).
 *
 * The OWNER acquires a buffer (buffer_id, held like an fd) and drives all
 * access grants: it calls `borrow` to assign a named grantee endpoint specific
 * rights, receiving a borrow_id that it hands to the grantee. A grantee may
 * `reborrow` its own borrow to a further context with rights that are a subset
 * of its own. `release` is owner-only and requires all borrows gone first;
 * `unborrow` drops one (re)borrow and cascade-revokes anything reborrowed from
 * it. read/write name the object by buffer_id (the kernel checks the caller is
 * the owner or a grantee with the required right).
 *
 * All return >= 0 on success (buffer_id / borrow_id / device address / 0) and a
 * negative xfer_buffer_status_t code on failure. */
extern int32_t wasmos_xfer_buffer_acquire(int32_t minimum_size)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_acquire");
/* Returns this process's spawn-info buffer_id (holding its wasmos_spawn_info_t
 * header + args blob), or 0 if none. The buffer is owned by this process. */
extern int32_t wasmos_spawn_info_buffer(void) WASMOS_WASM_IMPORT("wasmos", "spawn_info_buffer");
/* OWNER assigns `flags` rights over `buffer_id` to the context that owns
 * `grantee_endpoint`; returns the grantee's borrow_id. */
extern int32_t wasmos_xfer_buffer_borrow(int32_t grantee_endpoint, int32_t buffer_id, int32_t flags)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_borrow");
/* A grantee sub-grants its own `borrow_id` (rights ⊆ its own) to the context
 * that owns `grantee_endpoint`; returns the downstream borrow_id. */
extern int32_t wasmos_xfer_buffer_reborrow(int32_t grantee_endpoint, int32_t borrow_id,
                                           int32_t flags)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_reborrow");
extern int32_t wasmos_xfer_buffer_release(int32_t buffer_id)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_release");
extern int32_t wasmos_xfer_buffer_unborrow(int32_t borrow_id)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_unborrow");
extern int32_t wasmos_buffer_acquire(int32_t kind, int32_t minimum_size)
    WASMOS_WASM_IMPORT("wasmos", "buffer_acquire");
extern int32_t wasmos_buffer_borrow(int32_t kind, int32_t grantee_endpoint, int32_t buffer_id,
                                    int32_t flags) WASMOS_WASM_IMPORT("wasmos", "buffer_borrow");
extern int32_t wasmos_buffer_reborrow(int32_t kind, int32_t grantee_endpoint, int32_t borrow_id,
                                      int32_t flags)
    WASMOS_WASM_IMPORT("wasmos", "buffer_reborrow");
extern int32_t wasmos_buffer_release(int32_t kind, int32_t buffer_id)
    WASMOS_WASM_IMPORT("wasmos", "buffer_release");
extern int32_t wasmos_buffer_unborrow(int32_t borrow_id)
    WASMOS_WASM_IMPORT("wasmos", "buffer_unborrow");
extern int32_t wasmos_dma_map_borrow(int32_t borrow_id, int32_t offset, int32_t length,
                                     int32_t direction_flags)
    WASMOS_WASM_IMPORT("wasmos", "dma_map_borrow");
extern int32_t wasmos_dma_sync_borrow(int32_t borrow_id, int32_t offset, int32_t length,
                                      int32_t sync_op)
    WASMOS_WASM_IMPORT("wasmos", "dma_sync_borrow");
extern int32_t wasmos_dma_unmap_borrow(int32_t borrow_id)
    WASMOS_WASM_IMPORT("wasmos", "dma_unmap_borrow");
extern int32_t wasmos_ipc_select_one(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_one");
extern int32_t wasmos_ipc_drain(int32_t endpoint) WASMOS_WASM_IMPORT("wasmos", "ipc_drain");
extern int32_t wasmos_ipc_notify(int32_t endpoint) WASMOS_WASM_IMPORT("wasmos", "ipc_notify");
extern int32_t wasmos_ipc_last_field(int32_t field) WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");
/* Select sets: block until any one of N endpoints is ready. */
extern int32_t wasmos_ipc_select_create(void) WASMOS_WASM_IMPORT("wasmos", "ipc_select_create");
extern int32_t wasmos_ipc_select_add(int32_t select_id, int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_add");
/* Block until any watched endpoint is ready; returns the ready endpoint ID. */
extern int32_t wasmos_ipc_select_wait(int32_t select_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_wait");
/* Like ipc_select_wait but bounded by timeout_ms (0 = wait forever). Returns the
 * ready endpoint ID (>= 0), -1 on timeout/spurious wake (poll and retry), or -2
 * on error. Lets a driver poll (e.g. RX rings) on a timer without busy-yielding. */
extern int32_t wasmos_ipc_select_wait_timeout(int32_t select_id, int32_t timeout_ms)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_wait_timeout");
extern int32_t wasmos_ipc_select_destroy(int32_t select_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_destroy");
extern int32_t wasmos_proc_count(void) WASMOS_WASM_IMPORT("wasmos", "proc_count");
extern int32_t wasmos_proc_exit(int32_t status) WASMOS_WASM_IMPORT("wasmos", "proc_exit");
extern int32_t wasmos_proc_notify_ready(void) WASMOS_WASM_IMPORT("wasmos", "proc_notify_ready");
extern int32_t wasmos_sched_ticks(void) WASMOS_WASM_IMPORT("wasmos", "sched_ticks");
extern int32_t wasmos_sched_ready_count(void) WASMOS_WASM_IMPORT("wasmos", "sched_ready_count");
extern int32_t wasmos_sched_current_pid(void) WASMOS_WASM_IMPORT("wasmos", "sched_current_pid");
extern int32_t wasmos_sched_cpu_count(void) WASMOS_WASM_IMPORT("wasmos", "sched_cpu_count");
extern int32_t wasmos_sched_cpu_stats(int32_t cpu_id, int32_t out_ptr)
    WASMOS_WASM_IMPORT("wasmos", "sched_cpu_stats");
/* Physical memory snapshot: total/free bytes as seen by the kernel PFA. */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
} wasmos_physmem_stats_t;
extern int32_t wasmos_physmem_stats(wasmos_physmem_stats_t* out)
    WASMOS_WASM_IMPORT("wasmos", "physmem_stats");
/* Returns 0=wasm3, 1=WARP. Compile-time constant baked into the kernel. */
extern int32_t wasmos_kernel_runtime(void) WASMOS_WASM_IMPORT("wasmos", "kernel_runtime");
extern int32_t wasmos_sched_yield(void) WASMOS_WASM_IMPORT("wasmos", "sched_yield");
extern int32_t wasmos_thread_gettid(void) WASMOS_WASM_IMPORT("wasmos", "thread_gettid");
extern int32_t wasmos_thread_create(int32_t entry_token, int32_t arg0, int32_t arg1, int32_t flags)
    WASMOS_WASM_IMPORT("wasmos", "thread_create");
extern int32_t wasmos_thread_yield(void) WASMOS_WASM_IMPORT("wasmos", "thread_yield");
extern int32_t wasmos_thread_exit(int32_t status) WASMOS_WASM_IMPORT("wasmos", "thread_exit");
extern int32_t wasmos_thread_join(int32_t tid) WASMOS_WASM_IMPORT("wasmos", "thread_join");
extern int32_t wasmos_thread_detach(int32_t tid) WASMOS_WASM_IMPORT("wasmos", "thread_detach");
/* Kernel-backed recursive mutex using the wasmos_mutex_t struct in WASM memory.
 * mutex_ptr is the int32 WASM linear-memory address of the wasmos_mutex_t.
 * try_lock returns 0=acquired, 1=contended (caller must retry), <0=error.
 * unlock returns 0=released, <0=error (e.g. not the owner). */
extern int32_t wasmos_mutex_try_lock_host(int32_t mutex_ptr)
    WASMOS_WASM_IMPORT("wasmos", "mutex_try_lock");
extern int32_t wasmos_mutex_unlock_host(int32_t mutex_ptr)
    WASMOS_WASM_IMPORT("wasmos", "mutex_unlock");
extern int32_t wasmos_proc_info(int32_t index, int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "proc_info");
extern int32_t wasmos_proc_info_ex(int32_t index, int32_t ptr, int32_t len, int32_t parent_ptr)
    WASMOS_WASM_IMPORT("wasmos", "proc_info_ex");
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
extern int32_t wasmos_proc_info_stats(int32_t index, int32_t ptr, int32_t len, int32_t parent_ptr,
                                      int32_t stats_ptr)
    WASMOS_WASM_IMPORT("wasmos", "proc_info_stats");
extern int32_t wasmos_system_halt(void) WASMOS_WASM_IMPORT("wasmos", "system_halt");
extern int32_t wasmos_system_reboot(void) WASMOS_WASM_IMPORT("wasmos", "system_reboot");
extern int32_t wasmos_acpi_rsdp_info(int32_t out_ptr, int32_t out_len_ptr, int32_t max_len)
    WASMOS_WASM_IMPORT("wasmos", "acpi_rsdp_info");
extern int32_t wasmos_boot_module_name(int32_t index, int32_t buf, int32_t buf_len)
    WASMOS_WASM_IMPORT("wasmos", "boot_module_name");
extern int32_t wasmos_initfs_entry_count(void) WASMOS_WASM_IMPORT("wasmos", "initfs_entry_count");
extern int32_t wasmos_initfs_entry_name(int32_t index, int32_t buf, int32_t buf_len)
    WASMOS_WASM_IMPORT("wasmos", "initfs_entry_name");
extern int32_t wasmos_initfs_entry_size(int32_t index)
    WASMOS_WASM_IMPORT("wasmos", "initfs_entry_size");
extern int32_t wasmos_initfs_entry_copy(int32_t index, int32_t buf, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "initfs_entry_copy");
extern int32_t wasmos_initfs_find_path(int32_t path_ptr, int32_t path_len)
    WASMOS_WASM_IMPORT("wasmos", "initfs_find_path");
extern int32_t wasmos_block_buffer_phys(void) WASMOS_WASM_IMPORT("wasmos", "block_buffer_phys");
extern int32_t wasmos_block_buffer_copy(int32_t phys, int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "block_buffer_copy");
extern int32_t wasmos_block_buffer_write(int32_t phys, int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "block_buffer_write");
/* Zero-copy: overlay the caller's own 8 KiB block buffer into linear memory and
 * return its wasm offset (or -1).  Idempotent.  The mapped bytes alias the same
 * physical pages named by wasmos_block_buffer_phys(), so a peer block server
 * filling the buffer by phys is visible here without a block_buffer_copy. */
extern int32_t wasmos_block_buffer_map(void) WASMOS_WASM_IMPORT("wasmos", "block_buffer_map");
extern int32_t wasmos_xfer_buffer_size(void) WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_size");
extern int32_t wasmos_fs_endpoint(void) WASMOS_WASM_IMPORT("wasmos", "fs_endpoint");
extern int32_t wasmos_xfer_buffer_read(int32_t buffer_id, int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_read");
extern int32_t wasmos_xfer_buffer_write(int32_t buffer_id, int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_write");
/* Overlay an OWNED xfer-buffer's backing into this process's WASM linear memory
 * (zero-copy, same pinned-window baseline as shmem/block_buffer_map). Returns the
 * linmem byte offset (>= 0) of the mapping; the buffer's bytes are then directly
 * addressable at that offset for the socket-ring fast path. Idempotent per
 * buffer_id. unmap tears the window down; always unmap before releasing the
 * buffer so the linmem window never outlives its backing. Negative on failure. */
extern int32_t wasmos_xfer_buffer_map(int32_t buffer_id)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_map");
extern int32_t wasmos_xfer_buffer_unmap(int32_t buffer_id)
    WASMOS_WASM_IMPORT("wasmos", "xfer_buffer_unmap");
extern int32_t wasmos_early_log_size(void) WASMOS_WASM_IMPORT("wasmos", "early_log_size");
extern int32_t wasmos_early_log_copy(int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "early_log_copy");
extern int32_t wasmos_boot_config_size(void) WASMOS_WASM_IMPORT("wasmos", "boot_config_size");
extern int32_t wasmos_boot_config_copy(int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "boot_config_copy");
extern int32_t wasmos_io_in8(int32_t port) WASMOS_WASM_IMPORT("wasmos", "io_in8");
extern int32_t wasmos_io_in16(int32_t port) WASMOS_WASM_IMPORT("wasmos", "io_in16");
extern int32_t wasmos_io_in32(int32_t port) WASMOS_WASM_IMPORT("wasmos", "io_in32");
extern int32_t wasmos_io_out8(int32_t port, int32_t value) WASMOS_WASM_IMPORT("wasmos", "io_out8");
extern int32_t wasmos_io_out16(int32_t port, int32_t value)
    WASMOS_WASM_IMPORT("wasmos", "io_out16");
extern int32_t wasmos_io_out32(int32_t port, int32_t value)
    WASMOS_WASM_IMPORT("wasmos", "io_out32");
extern int32_t wasmos_io_wait(void) WASMOS_WASM_IMPORT("wasmos", "io_wait");
typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_gop_pixel_format;
} wasmos_framebuffer_info_t;

extern int32_t wasmos_framebuffer_info(wasmos_framebuffer_info_t* info, int32_t info_len)
    WASMOS_WASM_IMPORT("wasmos", "framebuffer_info");
extern int32_t wasmos_framebuffer_map(int32_t ptr, int32_t size)
    WASMOS_WASM_IMPORT("wasmos", "framebuffer_map");
/* Map a physical address range into WASM linear memory at wasm_offset.
 * phys_lo/phys_hi form a 64-bit physical address; size and wasm_offset must
 * be page-aligned (multiples of 4096). Requires the mmio.map capability. */
extern int32_t wasmos_phys_map(int32_t phys_lo, int32_t phys_hi, int32_t size, int32_t wasm_offset)
    WASMOS_WASM_IMPORT("wasmos", "phys_map");
/* Allocate a driver-owned, pinned, contiguous DMA region below 2 GiB and map it
 * into the caller's WASM linear memory (a real page remap, so writes reach the
 * exact physical pages the device DMAs).  cache_policy is WASMOS_REGION_CACHE_*.
 * Returns the wasm linmem offset of the mapped region (>= 0) and writes the u64
 * physical base to *out_phys, or a negative WASMOS_DMA_STATUS_* on failure.
 * Requires CAP_DMA_BUFFER and an approved DMA window covering the allocation.
 * out_phys must point into the caller's linear memory. */
extern int32_t wasmos_region_alloc(int32_t pages, int32_t cache_policy, uint64_t* out_phys)
    WASMOS_WASM_IMPORT("wasmos", "region_alloc");
extern int32_t wasmos_framebuffer_pixel(int32_t x, int32_t y, int32_t color)
    WASMOS_WASM_IMPORT("wasmos", "framebuffer_pixel");
/* Shared memory API: shmem_create allocates pages of shared memory and
 * returns an id; shmem_grant/revoke control which PIDs may map it;
 * shmem_map/map_auto map the region into WASM linear memory;
 * flush/refresh synchronise dirty regions between processes. */
extern int32_t wasmos_shmem_create(int32_t pages, int32_t flags)
    WASMOS_WASM_IMPORT("wasmos", "shmem_create");
extern int32_t wasmos_shmem_grant(int32_t id, int32_t target_pid)
    WASMOS_WASM_IMPORT("wasmos", "shmem_grant");
extern int32_t wasmos_shmem_revoke(int32_t id, int32_t target_pid)
    WASMOS_WASM_IMPORT("wasmos", "shmem_revoke");
/* On success wasmos_shmem_map/_auto return the mapped guest offset (>= 0).  On
 * failure they return a negative SHMEM_ERR_* reason code (see
 * drivers/include/wasmos_driver_abi.h) rather than a blanket -1, so callers can
 * report why a map failed. */
extern int32_t wasmos_shmem_map(int32_t id, int32_t ptr, int32_t size)
    WASMOS_WASM_IMPORT("wasmos", "shmem_map");
extern int32_t wasmos_shmem_map_auto(int32_t id, int32_t size)
    WASMOS_WASM_IMPORT("wasmos", "shmem_map_auto");
extern int32_t wasmos_shmem_flush(int32_t id, int32_t ptr, int32_t size)
    WASMOS_WASM_IMPORT("wasmos", "shmem_flush");
extern int32_t wasmos_shmem_refresh(int32_t id, int32_t ptr, int32_t size)
    WASMOS_WASM_IMPORT("wasmos", "shmem_refresh");
extern int32_t wasmos_shmem_unmap(int32_t id) WASMOS_WASM_IMPORT("wasmos", "shmem_unmap");
extern int32_t wasmos_irq_route_ipc(int32_t irq_line, int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "irq_route_ipc");
extern int32_t wasmos_irq_ack(int32_t irq_line) WASMOS_WASM_IMPORT("wasmos", "irq_ack");
extern int32_t wasmos_irq_unroute(int32_t irq_line) WASMOS_WASM_IMPORT("wasmos", "irq_unroute");
/* Configure an IRQ line's trigger/polarity (flags: WASMOS_IRQ_TRIGGER_LEVEL /
 * WASMOS_IRQ_POLARITY_LOW). Used by pci-bus to mark PCI INTx lines level/low.
 * Requires the IRQ capability. */
extern int32_t wasmos_irq_configure(int32_t irq_line, int32_t flags)
    WASMOS_WASM_IMPORT("wasmos", "irq_configure");
/* vt keyboard input integration */
extern int32_t wasmos_input_push(int32_t ch) WASMOS_WASM_IMPORT("wasmos", "input_push");
extern int32_t wasmos_input_read(void) WASMOS_WASM_IMPORT("wasmos", "input_read");
extern int32_t wasmos_env_get(const char* name, int32_t name_len, char* buf, int32_t buf_len)
    WASMOS_WASM_IMPORT("wasmos", "env_get");
extern int32_t wasmos_env_set(const char* name, int32_t name_len, const char* value,
                              int32_t val_len) WASMOS_WASM_IMPORT("wasmos", "env_set");
extern int32_t wasmos_env_unset(const char* name, int32_t name_len)
    WASMOS_WASM_IMPORT("wasmos", "env_unset");
/* select-set API: multi-endpoint blocking wait */
extern int32_t wasmos_ipc_select_create(void) WASMOS_WASM_IMPORT("wasmos", "ipc_select_create");
extern int32_t wasmos_ipc_select_add(int32_t select_id, int32_t endpoint_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_add");
extern int32_t wasmos_ipc_select_wait(int32_t select_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_wait");
extern int32_t wasmos_ipc_select_wait_timeout(int32_t select_id, int32_t timeout_ms)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_wait_timeout");
extern int32_t wasmos_ipc_select_destroy(int32_t select_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_destroy");

#ifdef __cplusplus
}
#endif

#endif
