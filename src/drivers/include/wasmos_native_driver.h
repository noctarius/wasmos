#ifndef WASMOS_NATIVE_DRIVER_H
#define WASMOS_NATIVE_DRIVER_H

/*
 * Self-contained ABI header shared between the kernel loader and native drivers.
 * Only depends on <stdint.h> so it compiles in freestanding driver builds.
 *
 * Struct layouts must match framebuffer_info_t and ipc_message_t in the kernel.
 * native_driver.c enforces this with _Static_assert.
 */

#include <stdint.h>

typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_reserved;
} nd_framebuffer_info_t;

typedef struct {
    uint32_t type;
    uint32_t source;
    uint32_t destination;
    uint32_t request_id;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
} nd_ipc_message_t;

/*
 * Kernel API function table passed to every native driver's initialize().
 * All pointers resolve to shared higher-half kernel code, valid from any
 * process address space.
 */
typedef struct wasmos_driver_api {
    /* Console */
    int      (*console_write)(const char *ptr, int len);
    int      (*console_read)(char *ptr, int len);

    /* Framebuffer — framebuffer_map maps the physical framebuffer into the
     * driver's device address region and returns a virtual pointer to it. */
    int      (*framebuffer_info)(nd_framebuffer_info_t *out);
    void    *(*buffer_borrow)(uint32_t kind, uint32_t source_context_id,
                              uint32_t flags, uint32_t size);
    int      (*buffer_release)(uint32_t kind);
    /* Deprecated legacy helper; use buffer_borrow with kind=FRAMEBUFFER. */
    void    *(*framebuffer_map)(uint32_t size);
    int      (*framebuffer_pixel)(uint32_t x, uint32_t y, uint32_t color);

    /* I/O ports */
    uint8_t  (*io_in8)(uint16_t port);
    uint16_t (*io_in16)(uint16_t port);
    void     (*io_out8)(uint16_t port, uint8_t val);
    void     (*io_out16)(uint16_t port, uint16_t val);

    /* IPC */
    uint32_t (*ipc_create_endpoint)(void);
    int      (*ipc_send)(uint32_t sender_context_id, uint32_t endpoint,
                         const nd_ipc_message_t *message);
    int      (*ipc_recv)(uint32_t receiver_context_id, uint32_t endpoint,
                         nd_ipc_message_t *out_message);

    /* Scheduler */
    void     (*sched_yield)(void);
    uint32_t (*sched_current_pid)(void);

    /* Process */
    void     (*proc_exit)(int code);

    /* Early kernel log — ring buffer of all serial output before the VT is
     * ready.  early_log_size() returns bytes buffered.  early_log_copy()
     * copies len bytes starting at logical offset into dst.
     * Both are read-only; the ring is never flushed after handoff. */
    uint32_t (*early_log_size)(void);
    void     (*early_log_copy)(uint8_t *dst, uint32_t offset, uint32_t len);

    /* Shared memory — general facility for sharing pages between processes.
     * shmem_create allocates pages and returns id plus direct pointer.
     * shmem_map returns identity-mapped kernel pointer for native drivers.
     * shmem_unmap releases this process's mapping reference. */
    int      (*shmem_create)(uint64_t pages, uint32_t flags,
                             uint32_t *out_id, void **out_ptr);
    void    *(*shmem_map)(uint32_t id);
    int      (*shmem_unmap)(uint32_t id);

    /* Returns the shmem id of the kernel console text ring. */
    uint32_t (*console_ring_id)(void);

    /* Publish framebuffer control endpoint for VT/control-plane clients. */
    int      (*console_register_fb)(uint32_t context_id, uint32_t endpoint);
} wasmos_driver_api_t;

#define ND_BUFFER_KIND_FS          1u
#define ND_BUFFER_KIND_FRAMEBUFFER 2u
#define ND_BUFFER_BORROW_READ      0x1u
#define ND_BUFFER_BORROW_WRITE     0x2u

/* Entry point that every native driver must provide via ELF e_entry. */
typedef int (*native_driver_entry_fn_t)(wasmos_driver_api_t *api,
                                        int module_count,
                                        int arg2, int arg3);

#endif
