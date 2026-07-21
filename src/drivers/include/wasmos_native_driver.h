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
#include "wasmos_spawn_info.h"

typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_gop_pixel_format;
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
    int (*console_write)(const char* ptr, int len);
    int (*console_read)(char* ptr, int len);

    /* Framebuffer */
    int (*framebuffer_info)(nd_framebuffer_info_t* out);
    int (*framebuffer_pixel)(uint32_t x, uint32_t y, uint32_t color);

    /* I/O ports */
    uint8_t (*io_in8)(uint16_t port);
    uint16_t (*io_in16)(uint16_t port);
    void (*io_out8)(uint16_t port, uint8_t val);
    void (*io_out16)(uint16_t port, uint16_t val);

    /* IPC */
    uint32_t (*ipc_create_endpoint)(void);
    int (*ipc_send)(uint32_t sender_context_id, uint32_t endpoint, const nd_ipc_message_t* message);
    int (*ipc_recv)(uint32_t receiver_context_id, uint32_t endpoint, nd_ipc_message_t* out_message);

    /* Scheduler */
    void (*sched_yield)(void);
    uint32_t (*sched_ticks)(void);
    uint32_t (*sched_current_pid)(void);
    uint32_t (*thread_current_tid)(void);
    int (*mutex_try_lock)(uint64_t mutex_addr);
    int (*mutex_unlock)(uint64_t mutex_addr);

    /* Process */
    void (*proc_exit)(int code);
    void (*proc_notify_ready)(void);

    /* Early kernel log — ring buffer of all serial output before the VT is
     * ready.  early_log_size() returns bytes buffered.  early_log_copy()
     * copies len bytes starting at logical offset into dst.
     * Both are read-only; the ring is never flushed after handoff. */
    uint32_t (*early_log_size)(void);
    void (*early_log_copy)(uint8_t* dst, uint32_t offset, uint32_t len);

    /* Shared memory — general facility for sharing pages between processes.
     * shmem_create allocates pages and returns id plus direct pointer.
     * shmem_map returns identity-mapped kernel pointer for native drivers.
     * shmem_unmap releases this process's mapping reference. */
    int (*shmem_create)(uint64_t pages, uint32_t flags, uint32_t* out_id, void** out_ptr);
    int (*shmem_grant)(uint32_t id, uint32_t target_context_id);
    void* (*shmem_map)(uint32_t id);
    int (*shmem_unmap)(uint32_t id);

    /* Endpoint owner/context lookup for request attribution. */
    int (*ipc_endpoint_owner)(uint32_t endpoint, uint32_t* out_owner_context_id);

    /* Returns the shmem id of the kernel console text ring. */
    uint32_t (*console_ring_id)(void);

    /* Publish framebuffer control endpoint for VT/control-plane clients. */
    int (*console_register_fb)(uint32_t context_id, uint32_t endpoint);

    /* ABI contract for strict kernel/driver compatibility checks. */
    uint32_t abi_magic;
    uint32_t abi_version;

    /* ABI extension hooks (append-only to preserve legacy layout). */
    int (*shmem_flush)(uint32_t id, const void* ptr, uint32_t size);

    /* Startup contract (v7). Fills *out with this process's wasmos_spawn_info_t
     * header and copies the NUL-terminated args blob into args_buf (bounded by
     * args_cap). The native equivalent of the WASM wasmos_spawn_info_buffer()
     * hostcall; the kernel copies directly (trusted code, no buffer mapping).
     * Returns 0 on success, negative if there is no spawn-info for this process. */
    int (*spawn_info)(wasmos_spawn_info_t* out, char* args_buf, uint32_t args_cap);

    /* Transfer-buffer object API (v8) — the native equivalent of the WASM
     * xfer_buffer_* hostcalls, under the owner-push capability model. Native
     * drivers access buffer bytes through a mapping (not read/write hostcalls),
     * so acquire returns a mapped pointer.
     *
     *  - xfer_buffer_acquire: own a new object of `kind` (ND_BUFFER_KIND_*) sized
     *    `size` bytes, map it read/write into the driver's address space, and
     *    return the mapped pointer (NULL on failure). *out_buffer_id receives the
     *    object's buffer_id for use on the IPC wire. The framebuffer is just an
     *    owned object of kind=FRAMEBUFFER (backed by the hardware framebuffer);
     *    it is acquired, never borrowed.
     *  - xfer_buffer_borrow: OWNER grants the context that owns `grantee_endpoint`
     *    `flags` (ND_BUFFER_BORROW_*) rights over `buffer_id`. Returns the
     *    grantee's borrow_id (>0) to ship on the wire, or a negative status.
     *  - xfer_buffer_unborrow: the grantor revokes a borrow it created (cascades
     *    downstream). Returns 0 on success.
     *  - xfer_buffer_release: the owner destroys `buffer_id`, unmapping it and
     *    cascade-revoking every borrow of it. Returns 0 on success. */
    void* (*xfer_buffer_acquire)(uint32_t kind, uint32_t size, uint32_t* out_buffer_id);
    /* Map a transfer buffer granted to this native service. `buffer_id` and
     * `borrow_id` must describe the same active grant; access permissions come
     * from that grant. Unmap it with xfer_buffer_unmap_borrowed when finished. */
    void* (*xfer_buffer_map_borrowed)(uint32_t kind, uint32_t buffer_id, uint32_t borrow_id);
    int (*xfer_buffer_unmap_borrowed)(uint32_t borrow_id);
    int (*xfer_buffer_borrow)(uint32_t grantee_endpoint, uint32_t buffer_id, uint32_t flags);
    int (*xfer_buffer_unborrow)(uint32_t borrow_id);
    int (*xfer_buffer_release)(uint32_t buffer_id);
    /* Block until `endpoint` has a queued message or timeout_ms elapses (0 =
     * forever), WITHOUT dequeuing. Lets a native service sleep at idle instead
     * of yield-spinning its poll loop; drain afterward with ipc_recv. */
    int (*ipc_wait)(uint32_t receiver_context_id, uint32_t endpoint, uint32_t timeout_ms);
} wasmos_driver_api_t;

#define ND_BUFFER_KIND_XFER 1u
#define ND_BUFFER_KIND_FRAMEBUFFER 2u
#define ND_BUFFER_BORROW_READ 0x1u
#define ND_BUFFER_BORROW_WRITE 0x2u

#define WASMOS_NATIVE_ABI_MAGIC 0x574E4150u /* 'WNAP' */
#define WASMOS_NATIVE_ABI_VERSION 10u

/* Entry point that every native driver must provide via ELF e_entry. */
typedef int (*native_driver_entry_fn_t)(wasmos_driver_api_t* api, int module_count, int arg2,
                                        int arg3);

#endif
