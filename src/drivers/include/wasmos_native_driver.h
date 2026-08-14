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

/* Mirror of the kernel's framebuffer_info_t, filled by api->framebuffer_info().
 * `framebuffer_base` is a PHYSICAL address, not something a driver may
 * dereference; `framebuffer_stride` is in PIXELS per scanline, not bytes, and
 * may exceed the width (padded scanlines). `framebuffer_gop_pixel_format` is the
 * UEFI GOP format enum the bootloader recorded. */
typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;
    uint32_t framebuffer_gop_pixel_format;
} nd_framebuffer_info_t;

/* Mirror of the kernel's ipc_message_t — the whole IPC payload, four argument
 * words plus routing. `source` and `destination` are context ids, not pids.
 * The kernel fills `source` on delivery, so a value a sender writes there is not
 * what the receiver sees. Per-opcode meaning of arg0..arg3 comes from
 * abi/opcodes.yaml, and is only defined together with the endpoint the message
 * was sent to (see wasmos_driver_abi.h on range reuse). */
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
    /* Console. console_write sends `len` bytes to the kernel log under a lock,
     * returning 0 on success or -1 for a NULL pointer or len <= 0; the buffer is
     * borrowed for the duration of the call and need not be NUL-terminated.
     * Output is NOT binary-safe: it is re-emitted as a C string, so an embedded
     * NUL truncates the rest of that chunk.
     *
     * console_read is a NON-blocking drain of the serial input queue: it returns
     * the number of bytes stored (0 when nothing is queued, never more than
     * `len`), or -1 for a NULL pointer or len <= 0. The vt owns serial RX, so a
     * driver reading here competes with it for the same bytes. */
    int (*console_write)(const char* ptr, int len);
    int (*console_read)(char* ptr, int len);

    /* Framebuffer. Both return 0 on success, else a negative packed
     * abi/errors.yaml code: WASMOS_ERR_FRAMEBUFFER_NOT_PRESENT when the machine
     * booted without a usable GOP framebuffer, WASMOS_INVAL for a NULL `out` or
     * for x/y outside the reported width/height. framebuffer_pixel writes one
     * pixel through the kernel's own mapping and is a per-call round trip, so a
     * driver painting a region acquires a FRAMEBUFFER transfer buffer (see
     * xfer_buffer_acquire below) instead of looping on it. */
    int (*framebuffer_info)(nd_framebuffer_info_t* out);
    int (*framebuffer_pixel)(uint32_t x, uint32_t y, uint32_t color);

    /* I/O ports. The reads report the value through `out` and the outcome
     * through the return, because 0xFF and 0xFFFF are what an absent device
     * reads back: returning them for a refused read would make a denied
     * io.port capability indistinguishable from missing hardware. The writes
     * report their outcome for the same reason -- a refused write reported as a
     * silent no-op is indistinguishable from a completed one.
     *
     * All four return 0 on success, else a negative packed abi/errors.yaml code:
     * WASMOS_ERR_IO_NOT_AUTHORIZED when the calling process's policy does not
     * grant that port, WASMOS_ERR_KERNEL_NO_CALLER when there is no current
     * process, WASMOS_ERR_KERNEL_BAD_POINTER for a NULL `out`. `port` is an
     * ABSOLUTE port number: the region-relative offsets a driver's manifest
     * grants are resolved by the driver before it calls in here. */
    int (*io_in8)(uint16_t port, uint8_t* out);
    int (*io_in16)(uint16_t port, uint16_t* out);
    int (*io_out8)(uint16_t port, uint8_t val);
    int (*io_out16)(uint16_t port, uint16_t val);

    /* IPC. All three are non-blocking; park with ipc_wait / ipc_select_wait.
     *  - ipc_create_endpoint: a fresh message endpoint owned by the calling
     *    process, or IPC_ENDPOINT_NONE (0xFFFFFFFF) on failure. Compare against
     *    that sentinel, not against 0 -- endpoint 0 is a legitimate id.
     *  - ipc_send: enqueue a COPY of *message on `endpoint`. The message is
     *    borrowed for the call. Returns IPC_OK (0), or a negative IPC_ERR_*:
     *    IPC_ERR_FULL (-3) when the destination queue is full, which is ordinary
     *    backpressure a driver retries rather than a fatal error.
     *  - ipc_recv: dequeue one message into *out_message. Returns IPC_OK (0)
     *    with the message filled, IPC_EMPTY (1) when nothing is queued -- a
     *    POSITIVE non-error result, so testing `!= 0` treats an empty queue as a
     *    failure -- or a negative IPC_ERR_*. Only the endpoint's owner may
     *    receive from it. */
    uint32_t (*ipc_create_endpoint)(void);
    int (*ipc_send)(uint32_t sender_context_id, uint32_t endpoint, const nd_ipc_message_t* message);
    int (*ipc_recv)(uint32_t receiver_context_id, uint32_t endpoint, nd_ipc_message_t* out_message);

    /* Scheduler.
     *  - sched_yield: reschedule, leaving the caller runnable; it returns once
     *    picked again. This is NOT a way to wait for work -- a poll loop built
     *    on it pegs a core; block on ipc_wait / ipc_select_wait instead.
     *  - sched_ticks: monotonic timer ticks since boot (wraps at 2^32).
     *  - sched_current_pid / thread_current_tid: the running process/thread.
     *  - mutex_try_lock: acquire the futex-style mutex at `mutex_addr` (an
     *    address in the CALLING process's own address space) for the current
     *    thread. Returns 0 when acquired or already held recursively, 1 when
     *    another thread holds it (try, so it never blocks), -1 when there is no
     *    current process. mutex_unlock returns 0, or -1 in that same case. */
    void (*sched_yield)(void);
    uint32_t (*sched_ticks)(void);
    uint32_t (*sched_current_pid)(void);
    uint32_t (*thread_current_tid)(void);
    int (*mutex_try_lock)(uint64_t mutex_addr);
    int (*mutex_unlock)(uint64_t mutex_addr);

    /* Process. proc_exit records `code` as the exit status and yields as
     * EXITED -- it does NOT return, so nothing after a call to it runs.
     * proc_notify_ready tells the process manager this driver finished bring-up;
     * a spawner ready-gating on a driver/service stays blocked until it lands,
     * so it must be called exactly once, after the driver can serve requests. */
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
     * shmem_unmap releases this process's mapping reference.
     *
     * The region is refcounted, and every pointer handed out here is a KERNEL
     * higher-half address: usable directly by a native driver (which runs
     * supervisor), never valid to ship across IPC to a WASM guest.
     *  - shmem_create: allocate `pages` whole pages; returns 0 on success with
     *    *out_id and (when non-NULL) *out_ptr set, or -1. Holds one reference on
     *    behalf of the caller.
     *  - shmem_grant: let `target_context_id` map the region. Returns 0 on
     *    success -- including the no-op cases of granting the owner or a
     *    duplicate grant -- and -1 for an unknown region, a target id of 0, or a
     *    full grant table.
     *  - shmem_map: take a reference and return the mapping, or NULL if the id
     *    is unknown or this context was never granted it. The pointer stays
     *    valid until the matching shmem_unmap drops the last reference.
     *  - shmem_unmap: drop one reference; 0 on success, -1 if the id is unknown
     *    or the refcount is already 0. At zero the frames are freed and the id
     *    is invalidated, so any pointer from shmem_map/create dangles. */
    int (*shmem_create)(uint64_t pages, uint32_t flags, uint32_t* out_id, void** out_ptr);
    int (*shmem_grant)(uint32_t id, uint32_t target_context_id);
    void* (*shmem_map)(uint32_t id);
    int (*shmem_unmap)(uint32_t id);

    /* Endpoint owner/context lookup for request attribution. Returns IPC_OK (0)
     * with *out_owner_context_id set, or a negative IPC_ERR_* (IPC_ERR_NOENT for
     * an endpoint that does not exist). A driver needs this to turn the
     * `grantee_endpoint` a client named into the context that must receive a
     * borrow, and to attribute an incoming request to a context. */
    int (*ipc_endpoint_owner)(uint32_t endpoint, uint32_t* out_owner_context_id);

    /* Returns the shmem id of the kernel console text ring. Map it with
     * shmem_map to read what the kernel logged; the buffer is a console_ring_t
     * (see wasmos_driver_abi.h). */
    uint32_t (*console_ring_id)(void);

    /* Publish framebuffer control endpoint for VT/control-plane clients. The
     * process manager records `endpoint` as the machine's framebuffer control
     * plane, so a later publisher displaces the earlier one. `context_id` is
     * ignored. Returns 0, or -1 when `endpoint` is IPC_ENDPOINT_NONE. */
    int (*console_register_fb)(uint32_t context_id, uint32_t endpoint);

    /* ABI contract for strict kernel/driver compatibility checks. A driver
     * verifies abi_magic == WASMOS_NATIVE_ABI_MAGIC and
     * abi_version == WASMOS_NATIVE_ABI_VERSION before touching any other field,
     * because a mismatched table means the function pointers below are at
     * different offsets than this header describes.
     *
     * The version constant is what makes the rest of this table changeable: any
     * edit that moves an existing field, changes a signature, or changes a
     * documented contract obliges a WASMOS_NATIVE_ABI_VERSION bump AND a rebuild
     * of every native driver, since drivers are separately linked ELF objects
     * that only see this struct through the pointer they are handed. Purely
     * APPENDING a hook at the end preserves the layout every older driver reads,
     * but still needs the bump so a new driver can refuse an old kernel that
     * would leave the new slot uninitialised. */
    uint32_t abi_magic;
    uint32_t abi_version;

    /* ABI extension hooks (append-only to preserve legacy layout).
     * shmem_flush copies `size` bytes from the borrowed `ptr` into the start of
     * shared region `id`. Returns 0 on success, -1 for a NULL pointer, a zero
     * size, an unknown/unreachable region, or a `size` larger than the region. */
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
     * from that grant. Unmap it with xfer_buffer_unmap_borrowed when finished.
     * Returns the mapped pointer, or NULL when the pair does not name a live
     * grant to this context. The mapping survives until unmap_borrowed, or until
     * the grantor unborrows / the owner releases the object -- after either the
     * pointer is stale, so a driver holding a long-lived mapping must not assume
     * it outlives the peer that granted it.
     *
     * The mapping slots are a fixed GLOBAL pool shared by all native services,
     * so a driver that maps without unmapping starves every other one. Each of
     * the four calls below returns 0 on success and -1 on failure, except
     * xfer_buffer_borrow, which returns the grantee's borrow_id (> 0). */
    void* (*xfer_buffer_map_borrowed)(uint32_t kind, uint32_t buffer_id, uint32_t borrow_id);
    int (*xfer_buffer_unmap_borrowed)(uint32_t borrow_id);
    int (*xfer_buffer_borrow)(uint32_t grantee_endpoint, uint32_t buffer_id, uint32_t flags);
    int (*xfer_buffer_unborrow)(uint32_t borrow_id);
    int (*xfer_buffer_release)(uint32_t buffer_id);
    /* Block until `endpoint` has a queued message or timeout_ms elapses (0 =
     * forever), WITHOUT dequeuing. Lets a native service sleep at idle instead
     * of yield-spinning its poll loop; drain afterward with ipc_recv.
     *
     * Returns IPC_OK (0) on any wake -- a timeout and a real arrival are not
     * distinguished, and a racing waiter may have taken the message already, so
     * the caller must tolerate the following ipc_recv reporting IPC_EMPTY and
     * loop. A negative IPC_ERR_* means the endpoint is not a message endpoint
     * this context owns. Only the owner may wait on an endpoint. */
    int (*ipc_wait)(uint32_t receiver_context_id, uint32_t endpoint, uint32_t timeout_ms);
    /* Multi-endpoint blocking wait. ipc_select_listen registers a set watching
     * endpoints[0..count) (<= 8) owned by owner_context_id and returns its id.
     * ipc_select_wait blocks until any watched endpoint has a message/notify or
     * timeout_ms elapses (0 = forever), reporting the ready endpoint; drain it
     * afterward with ipc_recv. ipc_select_destroy releases the set. This is what
     * lets a native service listening on several endpoints sleep at idle instead
     * of yield-spinning.
     *
     * ipc_select_listen returns IPC_OK (0) with *out_select_id set, or a
     * negative IPC_ERR_* (IPC_ERR_INVALID for a NULL array, a NULL out pointer,
     * or count == 0); on failure the partially built set is destroyed, so there
     * is nothing to clean up. `endpoints` is borrowed for the call only.
     *
     * ipc_select_wait returns IPC_OK (0) with *out_ready_ep naming the endpoint
     * to drain, or IPC_EMPTY (1) -- a POSITIVE non-error result covering a
     * timeout, a spurious wake, and a set destroyed underneath the waiter, all
     * of which mean "loop and wait again". Negative values are IPC_ERR_*. It
     * reports ONE ready endpoint per call even when several are ready, so a
     * caller with multiple endpoints keeps calling until it gets IPC_EMPTY.
     *
     * ipc_select_destroy releases the set and wakes anything parked on it; the
     * select_id is invalid afterwards. It does not close the watched endpoints. */
    int (*ipc_select_listen)(uint32_t owner_context_id, const uint32_t* endpoints, uint32_t count,
                             uint32_t* out_select_id);
    int (*ipc_select_wait)(uint32_t select_id, uint32_t owner_context_id, uint32_t* out_ready_ep,
                           uint32_t timeout_ms);
    void (*ipc_select_destroy)(uint32_t select_id, uint32_t owner_context_id);
    /* Anonymous page mapping for a native service's heap. `vm_map` returns a
     * page-aligned, writable region of at least `size` bytes (rounded up to
     * whole pages) or NULL; `vm_unmap` returns a region from `vm_map` given the
     * same size. The pointers are kernel higher-half (native services run
     * supervisor and can touch them directly); never hand them across the IPC
     * boundary. The native stdlib shim (a slab allocator) layers
     * malloc/free/calloc/realloc on top of these. */
    void* (*vm_map)(uint32_t size);
    void (*vm_unmap)(void* addr, uint32_t size);
} wasmos_driver_api_t;

/* `kind` argument to the xfer_buffer_* calls, naming what backs the object.
 * XFER is ordinary anonymous pages; FRAMEBUFFER is backed by the hardware
 * scanout, so exactly one may exist and acquiring it takes over the display.
 * Borrow and unborrow are transfer-kind only. */
#define ND_BUFFER_KIND_XFER 1u
#define ND_BUFFER_KIND_FRAMEBUFFER 2u
/* `flags` argument to xfer_buffer_borrow: the rights the grantee gets over the
 * object, from the OWNER's side. At least one bit must be set, and no other bit
 * is accepted; a grantee mapping the object gets exactly these permissions. */
#define ND_BUFFER_BORROW_READ 0x1u
#define ND_BUFFER_BORROW_WRITE 0x2u

/* Compatibility stamp in the api table's abi_magic/abi_version fields. A driver
 * checks both before using any function pointer; see those fields for what a
 * version bump obliges. */
#define WASMOS_NATIVE_ABI_MAGIC 0x574E4150u /* 'WNAP' */
#define WASMOS_NATIVE_ABI_VERSION 13u

/* Entry point that every native driver must provide via ELF e_entry.
 *
 * `api` points at a kernel-owned table that stays valid for as long as this call
 * has not returned -- which is the driver's whole life, since a driver's main
 * loop does not return. The driver must not free it or retain the pointer past a
 * return.
 *
 * module_count, arg2 and arg3 are ALL passed as zero. A driver reads its startup
 * values from api->spawn_info() instead; the parameters remain only because they
 * are part of the ELF entry signature.
 *
 * Returning at all is the failure path (the loader logs it and propagates the
 * value; -2 is additionally reported as an ABI mismatch). A driver that fails
 * bring-up returns a packed abi/errors.yaml code -- WASMOS_ERR_DRIVER_* -- never
 * a bare -1, and one that means to terminate normally calls api->proc_exit,
 * which does not return. */
typedef int (*native_driver_entry_fn_t)(wasmos_driver_api_t* api, int module_count, int arg2,
                                        int arg3);

#endif
