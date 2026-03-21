#ifndef WASMOS_LIBC_WASMOS_API_H
#define WASMOS_LIBC_WASMOS_API_H

#include "wasmos/imports.h"

extern int32_t wasmos_console_read(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_read");
extern int32_t wasmos_debug_mark(int32_t tag)
    WASMOS_WASM_IMPORT("wasmos", "debug_mark");
extern int32_t wasmos_ipc_create_endpoint(void)
    WASMOS_WASM_IMPORT("wasmos", "ipc_create_endpoint");
extern int32_t wasmos_ipc_send(int32_t destination_endpoint,
                               int32_t source_endpoint,
                               int32_t type,
                               int32_t request_id,
                               int32_t arg0,
                               int32_t arg1,
                               int32_t arg2,
                               int32_t arg3)
    WASMOS_WASM_IMPORT("wasmos", "ipc_send");
extern int32_t wasmos_ipc_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_recv");
extern int32_t wasmos_ipc_try_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_try_recv");
extern int32_t wasmos_ipc_wait(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_wait");
extern int32_t wasmos_ipc_notify(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_notify");
extern int32_t wasmos_ipc_last_field(int32_t field)
    WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");
extern int32_t wasmos_proc_count(void)
    WASMOS_WASM_IMPORT("wasmos", "proc_count");
extern int32_t wasmos_proc_exit(int32_t status)
    WASMOS_WASM_IMPORT("wasmos", "proc_exit");
extern int32_t wasmos_sched_ticks(void)
    WASMOS_WASM_IMPORT("wasmos", "sched_ticks");
extern int32_t wasmos_sched_ready_count(void)
    WASMOS_WASM_IMPORT("wasmos", "sched_ready_count");
extern int32_t wasmos_sched_current_pid(void)
    WASMOS_WASM_IMPORT("wasmos", "sched_current_pid");
extern int32_t wasmos_sched_yield(void)
    WASMOS_WASM_IMPORT("wasmos", "sched_yield");
extern int32_t wasmos_proc_info(int32_t index, int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "proc_info");
extern int32_t wasmos_proc_info_ex(int32_t index, int32_t ptr, int32_t len, int32_t parent_ptr)
    WASMOS_WASM_IMPORT("wasmos", "proc_info_ex");
extern int32_t wasmos_system_halt(void)
    WASMOS_WASM_IMPORT("wasmos", "system_halt");
extern int32_t wasmos_system_reboot(void)
    WASMOS_WASM_IMPORT("wasmos", "system_reboot");
extern int32_t wasmos_acpi_rsdp_info(int32_t out_ptr, int32_t out_len_ptr, int32_t max_len)
    WASMOS_WASM_IMPORT("wasmos", "acpi_rsdp_info");
extern int32_t wasmos_boot_module_name(int32_t index, int32_t buf, int32_t buf_len)
    WASMOS_WASM_IMPORT("wasmos", "boot_module_name");
extern int32_t wasmos_block_buffer_phys(void)
    WASMOS_WASM_IMPORT("wasmos", "block_buffer_phys");
extern int32_t wasmos_block_buffer_copy(int32_t phys, int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "block_buffer_copy");
extern int32_t wasmos_block_buffer_write(int32_t phys, int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "block_buffer_write");
extern int32_t wasmos_fs_buffer_size(void)
    WASMOS_WASM_IMPORT("wasmos", "fs_buffer_size");
extern int32_t wasmos_fs_endpoint(void)
    WASMOS_WASM_IMPORT("wasmos", "fs_endpoint");
extern int32_t wasmos_fs_buffer_copy(int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "fs_buffer_copy");
extern int32_t wasmos_fs_buffer_write(int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "fs_buffer_write");
extern int32_t wasmos_early_log_size(void)
    WASMOS_WASM_IMPORT("wasmos", "early_log_size");
extern int32_t wasmos_early_log_copy(int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "early_log_copy");
extern int32_t wasmos_boot_config_size(void)
    WASMOS_WASM_IMPORT("wasmos", "boot_config_size");
extern int32_t wasmos_boot_config_copy(int32_t ptr, int32_t len, int32_t offset)
    WASMOS_WASM_IMPORT("wasmos", "boot_config_copy");
extern int32_t wasmos_io_in8(int32_t port)
    WASMOS_WASM_IMPORT("wasmos", "io_in8");
extern int32_t wasmos_io_in16(int32_t port)
    WASMOS_WASM_IMPORT("wasmos", "io_in16");
extern int32_t wasmos_io_out8(int32_t port, int32_t value)
    WASMOS_WASM_IMPORT("wasmos", "io_out8");
extern int32_t wasmos_io_out16(int32_t port, int32_t value)
    WASMOS_WASM_IMPORT("wasmos", "io_out16");
extern int32_t wasmos_io_wait(void)
    WASMOS_WASM_IMPORT("wasmos", "io_wait");
typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_reserved;
} wasmos_framebuffer_info_t;

extern int32_t wasmos_framebuffer_info(wasmos_framebuffer_info_t *info, int32_t info_len)
    WASMOS_WASM_IMPORT("wasmos", "framebuffer_info");
extern int32_t wasmos_framebuffer_map(int32_t ptr, int32_t size)
    WASMOS_WASM_IMPORT("wasmos", "framebuffer_map");
extern int32_t wasmos_framebuffer_pixel(int32_t x, int32_t y, int32_t color)
    WASMOS_WASM_IMPORT("wasmos", "framebuffer_pixel");
extern int32_t wasmos_shmem_create(int32_t pages, int32_t flags)
    WASMOS_WASM_IMPORT("wasmos", "shmem_create");
extern int32_t wasmos_shmem_map(int32_t id, int32_t ptr, int32_t size)
    WASMOS_WASM_IMPORT("wasmos", "shmem_map");
extern int32_t wasmos_shmem_unmap(int32_t id)
    WASMOS_WASM_IMPORT("wasmos", "shmem_unmap");
/* vt keyboard input integration */
extern int32_t wasmos_input_push(int32_t ch)
    WASMOS_WASM_IMPORT("wasmos", "input_push");
extern int32_t wasmos_input_read(void)
    WASMOS_WASM_IMPORT("wasmos", "input_read");
#endif
