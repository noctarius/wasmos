/* warp/link.cpp - WARP host-call registration (wasmos imports → vb::NativeSymbol).
 *
 * Host functions follow the WARP V1 import convention:
 *   ReturnType fn(TypedArgs..., void *ctx)
 * where ctx → WarpCallContext (module pointer + pid + boot_info).
 *
 * Memory pointer args: unlike wasm3's automatic `*` translation, WARP passes
 * raw i32 offsets.  Wrappers call ctx->module->getLinearMemoryRegion(off, n)
 * to bounds-check and get the host-side pointer.
 *
 * IPC slot state mirrors wasm3/link.c; both maintain independent copies until
 * a follow-on refactor extracts the shared state into a common module.
 *
 * All wasm3/link.c imports are now implemented here.
 */

/* The WASMOS_SYMBOLS macro references static host functions by taking their
 * address.  Clang may not track this as an ODR-use and spuriously reports
 * them as unused when the reference is inside a nested macro expansion. */
#pragma clang diagnostic ignored "-Wunused-function"

#include <cstdint>
#include <cstring>
#include <array>

extern "C" {
#include "boot.h"
#include "warp/shim.h"
#include "ipc.h"
#include "process.h"
#include "process_manager.h"
#include "memory.h"
#include "physmem.h"
#include "thread.h"
#include "futex.h"
#include "klog.h"
#include "io.h"
#include "irq.h"
#include "serial.h"
#include "capability.h"
#include "timer.h"
#include "policy.h"
#include "paging.h"
#include "framebuffer.h"
#include "wasm_driver.h"
#include "wasmos_driver_abi.h"
#include "arch/x86_64/smp.h"
}

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/basedataoffsets.hpp"
#include "src/core/common/NativeSymbol.hpp"
#include "src/core/common/Span.hpp"
#include "src/core/common/function_traits.hpp"
#include "link.h"
#ifdef WASMOS_WARP_RING3
#include "warp_ring3.h"
#include "syscall.h"
#endif

// ---------------------------------------------------------------------------
// Call context
// ---------------------------------------------------------------------------

struct WarpCallContext {
    vb::WasmModule   *module;
    uint32_t          pid;
    const boot_info_t *boot_info;
};

// Per-PID singleton contexts.  A per-driver table comes later once the WARP
// wasm driver is written; this is sufficient for initial single-module runs.
static WarpCallContext g_ctx_table[PROCESS_MAX_COUNT];

/* Page-aligned scratch page used by warp_phys_map for ACPI/physical memory
 * reads.  ACPI physical pages are not necessarily in the kernel direct map
 * (they are EfiACPIReclaimMemory regions), so we cannot use phys|HIGHER_HALF
 * directly.  Instead we remap this kernel BSS page (which IS in the page
 * tables) to the target physical page, copy the data, then restore. */
static uint8_t g_phys_scratch[4096] __attribute__((aligned(4096)));

static inline uint8_t *
warp_mem(WarpCallContext *ctx, uint32_t offset, uint32_t size)
{
    /* Use the full getLinearMemoryRegion(offset, size) path with non-zero size.
     * With LINEAR_MEMORY_BOUNDS_CHECKS=1, this triggers probe() →
     * ensureLinearSize() which zero-initialises newly-committed WASM pages.
     * When warp_mem is used to obtain a WRITE destination, the zeroing
     * happens BEFORE our write — so our data overwrites the zeros safely.
     * Future probe() calls for those offsets then short-circuit (already
     * committed) without zeroing our data. */
    if (!ctx || !ctx->module) return nullptr;
    return ctx->module->getLinearMemoryRegion(offset, size);
}

static inline uint8_t *
warp_linear_mem_window(WarpCallContext *ctx, uint32_t offset, uint32_t size)
{
    if (!ctx || !ctx->module) {
        return nullptr;
    }
    uint64_t mem_size = (uint64_t)ctx->module->getLinearMemorySizeInPages() << 16;
    if ((uint64_t)offset + (uint64_t)size > mem_size) {
        return nullptr;
    }
    uint8_t *base = ctx->module->getLinearMemoryRegion(0, 0);
    if (!base) {
        return nullptr;
    }
    return base + offset;
}

static inline WarpCallContext *
warp_call_ctx(void *ctx_)
{
    uint32_t pid = process_current_pid();
    if (pid < PROCESS_MAX_COUNT && g_ctx_table[pid].module) {
        return &g_ctx_table[pid];
    }
    auto *ctx = static_cast<WarpCallContext *>(ctx_);
    if (!ctx) {
        return nullptr;
    }
    if (ctx->pid < PROCESS_MAX_COUNT && ctx == &g_ctx_table[ctx->pid]) {
        return ctx;
    }
    auto *module = static_cast<vb::WasmModule *>(ctx_);
    return static_cast<WarpCallContext *>(module->getContext());
}

// ---------------------------------------------------------------------------
// Per-PID IPC last-message slots (mirrors wasm3/link.c state)
// ---------------------------------------------------------------------------

struct WarpIpcLastSlot {
    uint32_t     pid;
    uint8_t      valid;
    ipc_message_t message;
};

static WarpIpcLastSlot g_ipc_last[PROCESS_MAX_COUNT];

struct WarpFsPeerSlot {
    uint32_t pid;
    uint8_t  valid;
    uint32_t peer_context_id;
};

static WarpFsPeerSlot g_fs_peer_slots[PROCESS_MAX_COUNT];

static void
warp_ipc_slots_init(void)
{
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_ipc_last[i].pid   = 0;
        g_ipc_last[i].valid = 0;
        g_fs_peer_slots[i].pid = 0;
        g_fs_peer_slots[i].valid = 0;
        g_fs_peer_slots[i].peer_context_id = 0;
    }
}

static WarpIpcLastSlot *
warp_ipc_slot_for_pid(uint32_t pid)
{
    if (!pid) return nullptr;
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_ipc_last[i].valid && g_ipc_last[i].pid == pid) return &g_ipc_last[i];
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (!g_ipc_last[i].valid) { g_ipc_last[i].pid = pid; return &g_ipc_last[i]; }
    }
    return nullptr;
}

static WarpFsPeerSlot *
warp_fs_peer_slot_for_pid(uint32_t pid)
{
    WarpFsPeerSlot *empty = nullptr;
    if (!pid) {
        return nullptr;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_fs_peer_slots[i].pid == pid) {
            return &g_fs_peer_slots[i];
        }
        if (!empty && g_fs_peer_slots[i].pid == 0) {
            empty = &g_fs_peer_slots[i];
        }
    }
    if (empty) {
        empty->pid = pid;
        empty->valid = 0;
        empty->peer_context_id = 0;
    }
    return empty;
}

// ---------------------------------------------------------------------------
// Internal helper — mirrors wasm3/link.c's static warp_current_context_id()
// ---------------------------------------------------------------------------

static int
warp_current_context_id(uint32_t *out)
{
    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);
    if (!proc || !out) return -1;
    *out = proc->context_id;
    return 0;
}

static int
warp_process_name_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static uint8_t
warp_dbg_ipc_trace_process(process_t *proc)
{
    (void)proc;
    return 0;
}

static void *
warp_fs_buffer_for_pid(uint32_t pid, uint32_t context_id)
{
    uint32_t target_context = context_id;
    WarpFsPeerSlot *peer = warp_fs_peer_slot_for_pid(pid);
    process_t *proc = process_get(pid);
    uint8_t is_fs_manager =
        (proc && proc->name && warp_process_name_eq(proc->name, "fs-manager")) ? 1u : 0u;

    if (is_fs_manager) {
        uint32_t borrowed_source = process_manager_buffer_borrow_source_context(
            PM_BUFFER_KIND_FILESYSTEM, context_id);
        if (borrowed_source != 0) {
            target_context = borrowed_source;
        }
        return process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, target_context);
    }
    if (peer && peer->valid && peer->peer_context_id != 0) {
        target_context = peer->peer_context_id;
    }
    return process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, target_context);
}

// ---------------------------------------------------------------------------
// Host call wrappers — IPC
// ---------------------------------------------------------------------------

static uint32_t
warp_ipc_create_endpoint(void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0, endpoint = IPC_ENDPOINT_NONE;
    (void)ctx;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    if (ipc_endpoint_create(context_id, &endpoint) != IPC_OK) return (uint32_t)-1;
    return endpoint;
}

static uint32_t
warp_ipc_endpoint_owner(uint32_t endpoint, void *ctx_)
{
    (void)ctx_;
    uint32_t owner = 0;
    if (ipc_endpoint_owner(endpoint, &owner) != IPC_OK || !owner) return (uint32_t)-1;
    return owner;
}

static uint32_t
warp_ipc_send(uint32_t dest, uint32_t src, uint32_t type,
              uint32_t req_id, uint32_t a0, uint32_t a1,
              uint32_t a2, uint32_t a3, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    ipc_message_t msg;
    msg.type       = type;
    msg.source     = src;
    msg.destination = dest;
    msg.request_id = req_id;
    msg.arg0       = a0;
    msg.arg1       = a1;
    msg.arg2       = a2;
    msg.arg3       = a3;
    return (uint32_t)ipc_send_from(context_id, dest, &msg);
}

static uint32_t
warp_ipc_select_one(uint32_t endpoint, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    process_t *process = nullptr;

    if ((int32_t)endpoint < 0 || warp_current_context_id(&context_id) != 0) {
        return (uint32_t)-1;
    }
    WarpIpcLastSlot *slot = warp_ipc_slot_for_pid(pid);
    if (!slot) {
        return (uint32_t)-1;
    }
    process = process_get(pid);
    if (!process) {
        return (uint32_t)-1;
    }
    process->in_hostcall = 1;

    for (;;) {
        process->block_reason = PROCESS_BLOCK_IPC;
        if (!process->ready && !process->require_explicit_ready) {
            process->ready = 1;
        }

        int rc = ipc_recv_blocking_for(context_id, endpoint, &slot->message);
        if (rc == IPC_EMPTY) {
            /* Spurious wake — another waiter claimed the message first.  Yield
             * so the scheduler can clear need_resched; without this the
             * in_hostcall guard in process_preempt_from_irq prevents timer
             * preemption and triggers the watchdog stall detector. */
            process_yield(PROCESS_RUN_YIELDED);
            continue;
        }
        if (rc != IPC_OK) {
            process->block_reason = PROCESS_BLOCK_NONE;
            process->in_hostcall = 0;
            return (uint32_t)-1;
        }

        process->block_reason = PROCESS_BLOCK_NONE;
        process->in_hostcall = 0;
        slot->valid = 1;
        WarpFsPeerSlot *peer = warp_fs_peer_slot_for_pid(pid);
        if (peer &&
            slot->message.type >= FS_IPC_OPEN_REQ &&
            slot->message.type <= FS_IPC_READ_APP_REQ) {
            uint32_t owner_context = 0;
            if (ipc_endpoint_owner(slot->message.source, &owner_context) == IPC_OK &&
                owner_context != 0) {
                peer->valid = 1;
                peer->peer_context_id = owner_context;
            } else {
                peer->valid = 0;
                peer->peer_context_id = 0;
            }
        }
        if (warp_dbg_ipc_trace_process(process)) {
            klog_write("[dbg-r3-ipc] recv pid=");
            serial_write_hex64(pid);
            klog_write(" ep=");
            serial_write_hex64(endpoint);
            klog_write(" type=");
            serial_write_hex64(slot->message.type);
            klog_write(" req=");
            serial_write_hex64(slot->message.request_id);
            klog_write(" src=");
            serial_write_hex64(slot->message.source);
            klog_write(" dst=");
            serial_write_hex64(slot->message.destination);
            klog_write(" a0=");
            serial_write_hex64(slot->message.arg0);
            klog_write(" a1=");
            serial_write_hex64(slot->message.arg1);
            klog_write(" a2=");
            serial_write_hex64(slot->message.arg2);
            klog_write(" a3=");
            serial_write_hex64(slot->message.arg3);
            klog_write("\n");
        }
        return 1;
    }
}

static uint32_t
warp_ipc_drain(uint32_t endpoint, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    if ((int32_t)endpoint < 0 || warp_current_context_id(&context_id) != 0) {
        return (uint32_t)-1;
    }
    WarpIpcLastSlot *slot = warp_ipc_slot_for_pid(pid);
    if (!slot) return (uint32_t)-1;
    int rc = ipc_recv_for(context_id, endpoint, &slot->message);
    if (rc == IPC_EMPTY) return 0;
    if (rc != IPC_OK)   return (uint32_t)-1;
    slot->valid = 1;
    WarpFsPeerSlot *peer = warp_fs_peer_slot_for_pid(pid);
    if (peer &&
        slot->message.type >= FS_IPC_OPEN_REQ &&
        slot->message.type <= FS_IPC_READ_APP_REQ) {
        uint32_t owner_context = 0;
        if (ipc_endpoint_owner(slot->message.source, &owner_context) == IPC_OK &&
            owner_context != 0) {
            peer->valid = 1;
            peer->peer_context_id = owner_context;
        } else {
            peer->valid = 0;
            peer->peer_context_id = 0;
        }
    }
    return 1;
}

static uint32_t
warp_ipc_notify(uint32_t endpoint, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    return (uint32_t)ipc_notify_from(context_id, endpoint);
}

static uint32_t
warp_ipc_last_field(uint32_t field, void *ctx_)
{
    (void)ctx_;
    uint32_t pid = process_current_pid();
    WarpIpcLastSlot *slot = warp_ipc_slot_for_pid(pid);
    process_t *proc = process_get(pid);
    uint32_t value = 0;
    if (!slot || !slot->valid) return (uint32_t)-1;
    switch (field) {
        case 0: value = slot->message.type; break;
        case 1: value = slot->message.request_id; break;
        case 2: value = slot->message.arg0; break;
        case 3: value = slot->message.arg1; break;
        case 4: value = slot->message.source; break;
        case 5: value = slot->message.destination; break;
        case 6: value = slot->message.arg2; break;
        case 7: value = slot->message.arg3; break;
        default: return (uint32_t)-1;
    }
    if (warp_dbg_ipc_trace_process(proc)) {
        klog_write("[dbg-r3-ipc] last pid=");
        serial_write_hex64(pid);
        klog_write(" field=");
        serial_write_hex64(field);
        klog_write(" value=");
        serial_write_hex64(value);
        klog_write("\n");
    }
    return value;
}

// ---------------------------------------------------------------------------
// Host call wrappers — console
// ---------------------------------------------------------------------------

static uint32_t
warp_console_read(uint32_t buf_offset, uint32_t len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)len <= 0) return (uint32_t)-1;
    uint8_t *buf = warp_mem(ctx, buf_offset, 1);
    if (!buf) return (uint32_t)-1;
    uint8_t ch = 0;
    int rc = serial_read_char(&ch);
    if (rc <= 0) return (uint32_t)rc;
    *buf = ch;
    return 1;
}

static uint32_t
warp_console_write(uint32_t buf_offset, uint32_t len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)len <= 0) return (uint32_t)-1;
    uint8_t *buf = warp_mem(ctx, buf_offset, len);
    if (!buf) return (uint32_t)-1;
    /* write in 127-byte chunks so klog_write always gets a null-terminated
     * string (we temporarily null-terminate at chunk boundaries). */
    uint32_t written = 0;
    char tmp[128];
    while (written < len) {
        uint32_t chunk = len - written;
        if (chunk > 127) chunk = 127;
        __builtin_memcpy(tmp, buf + written, chunk);
        tmp[chunk] = '\0';
        klog_write(tmp);
        written += chunk;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Host call wrappers — process / scheduler
// ---------------------------------------------------------------------------

static uint32_t
warp_proc_exit(uint32_t code, void *ctx_)
{
    (void)ctx_;
    process_t *proc = process_get(process_current_pid());
    if (proc) {
        process_set_exit_status(proc, static_cast<int32_t>(code));
        process_yield(PROCESS_RUN_EXITED);
    }
    return 0;
}

static uint32_t
warp_proc_notify_ready(void *ctx_)
{
    (void)ctx_;
    process_t *proc = process_get(process_current_pid());
    if (proc) process_notify_ready(proc);
    return 0;
}

static uint32_t
warp_sched_yield(void *ctx_)
{
    (void)ctx_;
    process_yield(PROCESS_RUN_YIELDED);
    return 0;
}

static uint32_t
warp_sched_current_pid(void *ctx_)
{
    (void)ctx_;
    return (uint32_t)process_current_pid();
}

static uint32_t
warp_thread_gettid(void *ctx_)
{
    (void)ctx_;
    return (uint32_t)thread_current_tid();
}

// ---------------------------------------------------------------------------
// Host call wrappers — futex
// ---------------------------------------------------------------------------

static uint32_t
warp_futex_wait(uint32_t addr_off, uint32_t val, uint32_t timeout_ms, void *ctx_)
{
    /* futex_wait takes the raw WASM linear-memory offset, not a host pointer. */
    auto *ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    (void)ctx;
    return (uint32_t)futex_wait(addr_off, val, timeout_ms, context_id);
}

static uint32_t
warp_futex_wake(uint32_t addr_off, uint32_t count, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    (void)ctx;
    return (uint32_t)futex_wake(addr_off, count, context_id);
}

// ---------------------------------------------------------------------------
// IPC select set operations
// ---------------------------------------------------------------------------

static uint32_t
warp_ipc_select_create(void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0, sel_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    if (ipc_select_create(context_id, &sel_id) != IPC_OK) return (uint32_t)-1;
    return sel_id;
}

static uint32_t
warp_ipc_select_add(uint32_t sel_id, uint32_t ep_id, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    return (uint32_t)(ipc_select_add(sel_id, ep_id, context_id) == IPC_OK ? 0 : -1);
}

static uint32_t
warp_ipc_select_wait(uint32_t sel_id, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    uint32_t ready = IPC_ENDPOINT_NONE;
    for (;;) {
        int rc = ipc_select_wait(sel_id, context_id, &ready);
        if (rc == IPC_OK) return ready;
        if (rc != IPC_EMPTY) return (uint32_t)-1;
    }
}

static uint32_t
warp_ipc_select_destroy(uint32_t sel_id, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    ipc_select_destroy(sel_id, context_id);
    return 0;
}

// ---------------------------------------------------------------------------
// FS shared buffer
// ---------------------------------------------------------------------------

static uint32_t
warp_fs_buffer_size(void *ctx_)
{
    (void)ctx_;
    return (uint32_t)process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM);
}

static uint32_t
warp_fs_endpoint(void *ctx_)
{
    (void)ctx_;
    uint32_t ep = process_manager_fs_endpoint();
    return (ep == IPC_ENDPOINT_NONE) ? (uint32_t)-1 : ep;
}

static uint32_t
warp_fs_buffer_copy(uint32_t ptr_off, uint32_t len, uint32_t offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if (!len) return 0;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    uint32_t max_len = process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM);
    if (offset + len > max_len) return (uint32_t)-1;
    uint8_t *wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr) return (uint32_t)-1;
    uint32_t borrow_flags =
        process_manager_buffer_borrow_flags(PM_BUFFER_KIND_FILESYSTEM, context_id);
    if (borrow_flags != 0 && (borrow_flags & 0x1u) == 0) return (uint32_t)-1;
    void *buf = warp_fs_buffer_for_pid(pid, context_id);
    if (!buf) return (uint32_t)-1;
    __builtin_memcpy(wasm_ptr, static_cast<uint8_t *>(buf) + offset, len);
    return 0;
}

static uint32_t
warp_fs_buffer_write(uint32_t ptr_off, uint32_t len, uint32_t offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if (!len) return 0;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    uint32_t max_len = process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM);
    if (offset + len > max_len) return (uint32_t)-1;
    uint8_t *wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr) return (uint32_t)-1;
    uint32_t borrow_flags =
        process_manager_buffer_borrow_flags(PM_BUFFER_KIND_FILESYSTEM, context_id);
    if (borrow_flags != 0 && (borrow_flags & 0x2u) == 0) return (uint32_t)-1;
    void *buf = warp_fs_buffer_for_pid(pid, context_id);
    if (!buf) return (uint32_t)-1;
    __builtin_memcpy(static_cast<uint8_t *>(buf) + offset, wasm_ptr, len);
    return 0;
}

// ---------------------------------------------------------------------------
// Generic shared buffer borrow/release
// ---------------------------------------------------------------------------

static uint32_t
warp_buffer_borrow(uint32_t kind, uint32_t src_ep, uint32_t flags, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    /* Resolve endpoint → owner context_id, same as wasm3's wasm_buffer_borrow_impl.
     * Signature: (kind, borrower_context_id, source_context_id, flags) */
    uint32_t source_owner = 0;
    if (ipc_endpoint_owner(src_ep, &source_owner) != IPC_OK || source_owner == 0)
        return (uint32_t)-1;
    return (uint32_t)process_manager_buffer_borrow_context(kind, context_id, source_owner, flags);
}

static uint32_t
warp_buffer_release(uint32_t kind, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    return (uint32_t)process_manager_buffer_release_context(kind, context_id);
}

// ---------------------------------------------------------------------------
// Block DMA buffer
// ---------------------------------------------------------------------------

#define WARP_BLOCK_BUF_PAGES 2u  /* 8 KB block buffer per process */

struct WarpBlockSlot { uint32_t pid; uint64_t phys; };
static WarpBlockSlot g_block_slots[PROCESS_MAX_COUNT];

/* Find or allocate a block slot for this PID (used by block_buffer_phys). */
static WarpBlockSlot *warp_block_slot(uint32_t pid)
{
    for (auto &s : g_block_slots) if (s.pid == pid || !s.pid) { s.pid = pid; return &s; }
    return nullptr;
}
/* Find a block slot by physical address (used by block_buffer_write/copy,
 * which may be called by a different process than the one that called phys). */
static WarpBlockSlot *warp_block_slot_by_phys(uint64_t phys)
{
    for (auto &s : g_block_slots) if (s.phys == phys) return &s;
    return nullptr;
}

static uint32_t
warp_block_buffer_phys(void *ctx_)
{
    (void)ctx_;
    uint32_t pid = process_current_pid();
    auto *slot = warp_block_slot(pid);
    if (!slot) return (uint32_t)-1;
    if (!slot->phys) {
        /* Must be < 512MB: that's the kernel's higher-half identity mapping
         * window AND within ATA's 32-bit DMA address range. */
        slot->phys = pfa_alloc_pages_below(WARP_BLOCK_BUF_PAGES, 512ULL * 1024 * 1024);
        if (!slot->phys) return (uint32_t)-1;
    }
    return (uint32_t)slot->phys;
}

static uint32_t
warp_block_buffer_copy(uint32_t phys, uint32_t ptr_off, uint32_t len, uint32_t offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    auto *slot = warp_block_slot_by_phys((uint64_t)phys);
    if (!slot) return (uint32_t)-1;
    uint32_t buf_bytes = WARP_BLOCK_BUF_PAGES * 4096;
    if (offset + len > buf_bytes) return (uint32_t)-1;
    uint8_t *wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr) return (uint32_t)-1;
    uint8_t *buf = reinterpret_cast<uint8_t *>(slot->phys | 0xFFFFFFFF80000000ULL);
    __builtin_memcpy(wasm_ptr, buf + offset, len);
    return 0;
}

static uint32_t
warp_block_buffer_write(uint32_t phys, uint32_t ptr_off, uint32_t len, uint32_t offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    auto *slot = warp_block_slot_by_phys((uint64_t)phys);
    if (!slot) return (uint32_t)-1;
    uint32_t buf_bytes = WARP_BLOCK_BUF_PAGES * 4096;
    if (offset + len > buf_bytes) return (uint32_t)-1;
    uint8_t *wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr) return (uint32_t)-1;
    uint8_t *buf = reinterpret_cast<uint8_t *>(slot->phys | 0xFFFFFFFF80000000ULL);
    __builtin_memcpy(buf + offset, wasm_ptr, len);
    return 0;
}

// ---------------------------------------------------------------------------
// I/O port access
// ---------------------------------------------------------------------------

static uint32_t warp_io_in8(uint32_t port, void *ctx_)
    { (void)ctx_; return (port > 0xFFFF) ? (uint32_t)-1 : (uint32_t)inb((uint16_t)port); }
static uint32_t warp_io_in16(uint32_t port, void *ctx_)
    { (void)ctx_; return (port > 0xFFFF) ? (uint32_t)-1 : (uint32_t)inw((uint16_t)port); }
static uint32_t warp_io_in32(uint32_t port, void *ctx_)
    { (void)ctx_; return (port > 0xFFFF) ? (uint32_t)-1 : (uint32_t)inl((uint16_t)port); }
static uint32_t warp_io_out8(uint32_t port, uint32_t val, void *ctx_)
    { (void)ctx_; if (port > 0xFFFF) return (uint32_t)-1; outb((uint16_t)port,(uint8_t)val); return 0; }
static uint32_t warp_io_out16(uint32_t port, uint32_t val, void *ctx_)
    { (void)ctx_; if (port > 0xFFFF) return (uint32_t)-1; outw((uint16_t)port,(uint16_t)val); return 0; }
static uint32_t warp_io_out32(uint32_t port, uint32_t val, void *ctx_)
    { (void)ctx_; if (port > 0xFFFF) return (uint32_t)-1; outl((uint16_t)port,(uint32_t)val); return 0; }
static uint32_t warp_io_wait(void *ctx_)
    { (void)ctx_; io_wait(); return 0; }

// ---------------------------------------------------------------------------
// ACPI / boot info
// ---------------------------------------------------------------------------

static const boot_info_t *g_warp_boot_info = nullptr;

static uint32_t
warp_acpi_rsdp_info(uint32_t out_off, uint32_t out_len_off, uint32_t max_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if (!g_warp_boot_info || !g_warp_boot_info->rsdp || !g_warp_boot_info->rsdp_length) return (uint32_t)-1;
    uint32_t len = g_warp_boot_info->rsdp_length;
    if (len > max_len) return (uint32_t)-1;
    /* warp_mem uses getLinearMemoryRegion(offset, size) which triggers probe()
     * → ensureLinearSize() BEFORE we write — so zeroing happens first. */
    uint8_t *out = warp_mem(ctx, out_off, len);
    uint32_t *out_len = reinterpret_cast<uint32_t *>(warp_mem(ctx, out_len_off, sizeof(uint32_t)));
    if (!out || !out_len) return (uint32_t)-1;
    __builtin_memcpy(out, g_warp_boot_info->rsdp, len);
    *out_len = len;
    return 0;
}

static uint32_t
warp_boot_module_name(uint32_t index, uint32_t out_off, uint32_t out_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if (!g_warp_boot_info) return (uint32_t)-1;
    if (index >= g_warp_boot_info->module_count) return (uint32_t)-1;
    const boot_module_t *mod = static_cast<const boot_module_t *>(g_warp_boot_info->modules) + index;
    uint32_t name_len = (uint32_t)__builtin_strlen(mod->name);
    if (name_len >= out_len) name_len = out_len - 1;
    uint8_t *out = warp_mem(ctx, out_off, out_len);
    if (!out) return (uint32_t)-1;
    __builtin_memcpy(out, mod->name, name_len);
    out[name_len] = '\0';
    return (uint32_t)name_len;
}

static uint32_t
warp_sync_user_read(uint32_t ptr_off, uint32_t len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if (!len) return 0;
    uint8_t *p = warp_mem(ctx, ptr_off, len);
    if (!p) return (uint32_t)-1;
    /* In kernel context WASM linear memory IS kernel memory; a volatile read
     * ensures the compiler does not elide the access. */
    volatile uint8_t dummy = 0;
    for (uint32_t i = 0; i < len; ++i) dummy = p[i];
    (void)dummy;
    return 0;
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

static uint32_t warp_system_halt(void *ctx_)
    { (void)ctx_; for (;;) { __asm__ volatile("hlt"); } }
static uint32_t warp_system_reboot(void *ctx_)
    { (void)ctx_; outb(0x64, 0xFE); for (;;) { __asm__ volatile("hlt"); } }

// ---------------------------------------------------------------------------
// Scheduler extras
// ---------------------------------------------------------------------------

static uint32_t warp_sched_ticks(void *ctx_)
    { (void)ctx_; return (uint32_t)timer_ticks(); }
static uint32_t warp_proc_count(void *ctx_)
    { (void)ctx_; return (uint32_t)process_count_active(); }

// ---------------------------------------------------------------------------
// initfs access (mirrors initfs_header_get / initfs_entry_at in wasm3/link.c)
// ---------------------------------------------------------------------------

static int
warp_initfs_header_get(const wasmos_initfs_header_t **out_hdr, const uint8_t **out_base)
{
    if (!g_warp_boot_info ||
        !(g_warp_boot_info->flags & BOOT_INFO_FLAG_INITFS_PRESENT) ||
        !g_warp_boot_info->initfs ||
        g_warp_boot_info->initfs_size < sizeof(wasmos_initfs_header_t)) return -1;
    const uint8_t *base = static_cast<const uint8_t *>(g_warp_boot_info->initfs);
    const wasmos_initfs_header_t *hdr = reinterpret_cast<const wasmos_initfs_header_t *>(base);
    if (__builtin_memcmp(hdr->magic, WASMOS_INITFS_MAGIC, sizeof(hdr->magic)) != 0 ||
        hdr->version != WASMOS_INITFS_VERSION ||
        hdr->header_size < sizeof(wasmos_initfs_header_t) ||
        hdr->entry_size != sizeof(wasmos_initfs_entry_t)) return -1;
    if ((uint64_t)hdr->header_size + (uint64_t)hdr->entry_count * hdr->entry_size >
            (uint64_t)g_warp_boot_info->initfs_size) return -1;
    *out_hdr = hdr; *out_base = base; return 0;
}

static int
warp_initfs_entry_at(uint32_t index, wasmos_initfs_entry_t *out)
{
    const wasmos_initfs_header_t *hdr = nullptr; const uint8_t *base = nullptr;
    if (!out || warp_initfs_header_get(&hdr, &base) != 0 || index >= hdr->entry_count) return -1;
    const wasmos_initfs_entry_t *e = reinterpret_cast<const wasmos_initfs_entry_t *>(
        base + hdr->header_size + (uint64_t)index * hdr->entry_size);
    if ((uint64_t)e->offset + e->size > (uint64_t)g_warp_boot_info->initfs_size) return -1;
    *out = *e; return 0;
}

static uint32_t
warp_initfs_entry_count(void *ctx_)
{
    (void)ctx_;
    const wasmos_initfs_header_t *hdr = nullptr; const uint8_t *base = nullptr;
    if (warp_initfs_header_get(&hdr, &base) != 0) return (uint32_t)-1;
    return (uint32_t)hdr->entry_count;
}

static uint32_t
warp_initfs_entry_name(uint32_t index, uint32_t out_off, uint32_t out_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    wasmos_initfs_entry_t e;
    if (warp_initfs_entry_at(index, &e) != 0) return (uint32_t)-1;
    uint32_t nlen = 0;
    while (nlen < (uint32_t)sizeof(e.path) && e.path[nlen]) ++nlen;
    if (nlen >= out_len) nlen = out_len - 1;
    uint8_t *out = warp_mem(ctx, out_off, out_len);
    if (!out) return (uint32_t)-1;
    __builtin_memcpy(out, e.path, nlen);
    out[nlen] = '\0';
    return nlen;
}

static uint32_t
warp_initfs_entry_size(uint32_t index, void *ctx_)
{
    (void)ctx_;
    wasmos_initfs_entry_t e;
    return (warp_initfs_entry_at(index, &e) == 0) ? (uint32_t)e.size : (uint32_t)-1;
}

static uint32_t
warp_initfs_entry_copy(uint32_t index, uint32_t out_off, uint32_t len, uint32_t offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    wasmos_initfs_entry_t e;
    if (warp_initfs_entry_at(index, &e) != 0) return (uint32_t)-1;
    if (offset + len > e.size) return (uint32_t)-1;
    uint8_t *out = warp_mem(ctx, out_off, len);
    if (!out) return (uint32_t)-1;
    const uint8_t *src = static_cast<const uint8_t *>(g_warp_boot_info->initfs) + e.offset + offset;
    __builtin_memcpy(out, src, len);
    return len;   /* bytes copied, matches wasm3 which returns (int32_t)len */
}

// ---------------------------------------------------------------------------
// DMA buffer operations
// ---------------------------------------------------------------------------

static uint32_t
warp_dma_map_borrow(uint32_t kind, uint32_t src_ep, uint32_t offset,
                    uint32_t length, uint32_t flags, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)WASMOS_DMA_STATUS_DENY;
    /* Resolve endpoint → owner context, same as wasm3 */
    uint32_t src_owner = 0;
    if (ipc_endpoint_owner(src_ep, &src_owner) != IPC_OK || src_owner == 0
        || src_owner == context_id) return (uint32_t)WASMOS_DMA_STATUS_DENY;
    uint64_t dev_addr = 0;
    /* (kind, borrower=ATA context_id, source=fs-fat src_owner, ...) */
    int rc = process_manager_buffer_dma_map(kind, context_id, src_owner, offset, length, flags, &dev_addr);
    return (rc == 0) ? (uint32_t)dev_addr : (uint32_t)WASMOS_DMA_STATUS_DENY;
}

static uint32_t
warp_dma_sync_borrow(uint32_t kind, uint32_t offset, uint32_t length, uint32_t op, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)WASMOS_DMA_STATUS_DENY;
    int rc = process_manager_buffer_dma_sync(kind, context_id, offset, length, op);
    return (rc == 0) ? 0 : (uint32_t)WASMOS_DMA_STATUS_DENY;
}

static uint32_t
warp_dma_unmap_borrow(uint32_t kind, uint32_t src_ep, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0, src_owner = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)WASMOS_DMA_STATUS_DENY;
    if (ipc_endpoint_owner(src_ep, &src_owner) != IPC_OK || src_owner == 0)
        return (uint32_t)WASMOS_DMA_STATUS_DENY;
    /* (kind, borrower=ATA context_id, source=fs_fat src_owner) */
    int rc = process_manager_buffer_dma_unmap(kind, context_id, src_owner);
    return (rc == 0) ? 0 : (uint32_t)WASMOS_DMA_STATUS_DENY;
}

// ---------------------------------------------------------------------------
// Physical memory mapping into WASM linear memory
// ---------------------------------------------------------------------------

static uint32_t
warp_phys_map(uint32_t phys_lo, uint32_t phys_hi, uint32_t size, uint32_t wasm_offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if (!size || (size & 0xFFF) || (wasm_offset & 0xFFF)) return (uint32_t)-1;
    uint64_t phys = ((uint64_t)phys_hi << 32) | (uint64_t)phys_lo;
    if (!phys) return (uint32_t)-1;
    /* Map physical pages into WASM linear memory.
     * With the page-aligned allocator fix in shim.cpp, the linear memory base
     * is 4 KB-aligned, so base + wasm_offset is page-aligned when wasm_offset
     * is a multiple of 4096. */
    uint8_t *lmem = warp_linear_mem_window(ctx, wasm_offset, size);
    if (!lmem) return (uint32_t)-1;
    /* WARP's linear memory base has a fixed sub-page offset equal to
     * basedataLength (WARP internal metadata), so base + wasm_offset is never
     * page-aligned even if wasm_offset is 4 KB-aligned.  Page-remapping via
     * paging_map_4k requires aligned VAs; misaligned remaps produce reads from
     * the wrong physical offset.  For the only current user (acpi_bus reading
     * ACPI tables from regular RAM), copy directly from the kernel direct map.
     * This is always correct and avoids corrupting WARP's basedata metadata. */
    /* ACPI physical pages are in EfiACPIReclaimMemory regions which may not be
     * in the kernel direct map.  Map each physical page to the page-aligned
     * kernel scratch buffer, copy 4 KB of data, then restore the scratch page
     * to its original physical backing.  This is single-threaded (WARP runs
     * in the kernel scheduler context) so no locking is needed. */
    /* ActiveMemoryManager::ensureLinearSize() is called by probe() when the JIT
     * first accesses an uncommitted WASM linear memory region. It zero-initialises
     * the newly committed range before marking it usable.  If we write ACPI data
     * to lmem BEFORE the range is committed, the JIT's first probe() for any byte
     * in [lmem, lmem+size) will zero-initialize that range, wiping our write.
     *
     * Fix: trigger probe() NOW (before our write) by calling getLinearMemoryRegion
     * with non-zero size.  This causes ensureLinearSize to zero the region first
     * and mark it as usable.  Future probe() calls then short-circuit immediately
     * (offset < usableLinMemBytes_) without any zeroing.  We then write ACPI data
     * on top of the zeros, safe from future zeroing. */
    ctx->module->getLinearMemoryRegion(wasm_offset + size - 1, 1);
    /* Re-fetch lmem after the probe (ensureCapacityForLinearSize inside
     * ensureLinearSize may have called syncBasedataStart, changing the base). */
    lmem = ctx->module->getLinearMemoryRegion(0, 0);
    if (!lmem) return (uint32_t)-1;
    lmem += wasm_offset;

    uint64_t scratch_va  = (uint64_t)(uintptr_t)g_phys_scratch;
    uint64_t scratch_phys = scratch_va - KERNEL_HIGHER_HALF_BASE;
    for (uint32_t off = 0; off < size; off += 4096) {
        paging_map_4k(scratch_va, phys + off, 3ULL);
        __builtin_memcpy(lmem + off, g_phys_scratch, 4096);
        paging_map_4k(scratch_va, scratch_phys, 3ULL);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Misc stubs: debug_mark, kmap_dump, scheduler extras
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Environment variables (kenv) — mirrors the static table in wasm3/link.c
// ---------------------------------------------------------------------------

#define WARP_KENV_MAX_ENTRIES 64
#define WARP_KENV_KEY_MAX     33
#define WARP_KENV_VAL_MAX     129

struct WarpKenvEntry {
    uint8_t in_use;
    char    key[WARP_KENV_KEY_MAX];
    char    value[WARP_KENV_VAL_MAX];
};

static WarpKenvEntry g_warp_kenv[WARP_KENV_MAX_ENTRIES];

static int warp_kenv_find(const char *key) {
    for (int i = 0; i < WARP_KENV_MAX_ENTRIES; i++) {
        if (g_warp_kenv[i].in_use && __builtin_strcmp(g_warp_kenv[i].key, key) == 0)
            return i;
    }
    return -1;
}

static uint32_t
warp_env_get(uint32_t name_off, uint32_t name_len, uint32_t buf_off, uint32_t buf_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || (int32_t)buf_len <= 0) return (uint32_t)-1;
    if (name_len >= WARP_KENV_KEY_MAX) return (uint32_t)-1;
    const uint8_t *name = warp_mem(ctx, name_off, name_len);
    uint8_t *buf = warp_mem(ctx, buf_off, buf_len);
    if (!name || !buf) return (uint32_t)-1;
    char local_name[WARP_KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    int idx = warp_kenv_find(local_name);
    if (idx < 0) return (uint32_t)-1;
    uint32_t val_len = 0;
    while (g_warp_kenv[idx].value[val_len]) val_len++;
    uint32_t write_len = val_len < buf_len - 1u ? val_len : buf_len - 1u;
    __builtin_memcpy(buf, g_warp_kenv[idx].value, write_len);
    buf[write_len] = '\0';
    return write_len;
}

static uint32_t
warp_env_set(uint32_t name_off, uint32_t name_len, uint32_t val_off, uint32_t val_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || name_len >= WARP_KENV_KEY_MAX) return (uint32_t)-1;
    if ((int32_t)val_len < 0 || val_len >= WARP_KENV_VAL_MAX) return (uint32_t)-1;
    const uint8_t *name = warp_mem(ctx, name_off, name_len);
    if (!name) return (uint32_t)-1;
    char local_name[WARP_KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    char local_val[WARP_KENV_VAL_MAX];
    local_val[0] = '\0';
    if (val_len > 0) {
        const uint8_t *val = warp_mem(ctx, val_off, val_len);
        if (!val) return (uint32_t)-1;
        __builtin_memcpy(local_val, val, val_len);
        local_val[val_len] = '\0';
    }
    int idx = warp_kenv_find(local_name);
    if (idx < 0) {
        for (int i = 0; i < WARP_KENV_MAX_ENTRIES; i++) {
            if (!g_warp_kenv[i].in_use) { idx = i; break; }
        }
        if (idx < 0) return (uint32_t)-1;
        g_warp_kenv[idx].in_use = 1;
        __builtin_memcpy(g_warp_kenv[idx].key, local_name, name_len + 1u);
    }
    __builtin_memcpy(g_warp_kenv[idx].value, local_val, val_len + 1u);
    return 0;
}

static uint32_t
warp_env_unset(uint32_t name_off, uint32_t name_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || name_len >= WARP_KENV_KEY_MAX) return 0;
    const uint8_t *name = warp_mem(ctx, name_off, name_len);
    if (!name) return 0;
    char local_name[WARP_KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    int idx = warp_kenv_find(local_name);
    if (idx >= 0) g_warp_kenv[idx].in_use = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

static int warp_require_dma_capability(uint32_t context_id)
    { return policy_authorize(context_id, POLICY_ACTION_DMA_BUFFER, 0); }
static int warp_require_mmio_capability(uint32_t context_id)
    { return policy_authorize(context_id, POLICY_ACTION_MMIO_MAP, 0); }
static int warp_require_irq_capability(uint32_t context_id)
    { return policy_authorize(context_id, POLICY_ACTION_IRQ_CONTROL, 0); }

// ---------------------------------------------------------------------------
// Shmem map tracking — mirrors g_wasm_shmem_maps in wasm3/link.c
// ---------------------------------------------------------------------------

#define WARP_SHMEM_MAP_SLOTS (PROCESS_MAX_COUNT * 32)

struct WarpShmemLinearMap {
    uint32_t pid;
    uint32_t shmem_id;
    uint32_t offset;
    uint32_t size;
    uint8_t  valid;
};

static WarpShmemLinearMap g_warp_shmem_maps[WARP_SHMEM_MAP_SLOTS];

static void warp_shmem_map_track(uint32_t pid, uint32_t id, uint32_t offset, uint32_t size)
{
    WarpShmemLinearMap *empty = nullptr;
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i) {
        WarpShmemLinearMap *s = &g_warp_shmem_maps[i];
        if (s->valid && s->pid == pid && s->shmem_id == id && s->offset == offset) {
            s->size = size; return;
        }
        if (!empty && !s->valid) empty = s;
    }
    if (empty) { empty->pid = pid; empty->shmem_id = id;
                 empty->offset = offset; empty->size = size; empty->valid = 1; }
}

static void warp_shmem_map_untrack(uint32_t pid, uint32_t id)
{
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i)
        if (g_warp_shmem_maps[i].valid && g_warp_shmem_maps[i].pid == pid
            && g_warp_shmem_maps[i].shmem_id == id)
            g_warp_shmem_maps[i].valid = 0;
}

static WarpShmemLinearMap *warp_shmem_map_find(uint32_t pid, uint32_t id)
{
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i) {
        WarpShmemLinearMap *slot = &g_warp_shmem_maps[i];
        if (slot->valid && slot->pid == pid && slot->shmem_id == id) {
            return slot;
        }
    }
    return nullptr;
}

static uint8_t warp_shmem_map_overlaps(uint32_t pid, uint32_t offset, uint32_t size)
{
    uint64_t a0 = offset, a1 = (uint64_t)offset + size;
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i) {
        const WarpShmemLinearMap *s = &g_warp_shmem_maps[i];
        if (!s->valid || s->pid != pid || s->size == 0) continue;
        uint64_t b0 = s->offset, b1 = b0 + s->size;
        if (a0 < b1 && b0 < a1) return 1;
    }
    return 0;
}

static uint32_t
warp_linear_memory_active_size(WarpCallContext *ctx)
{
    uint8_t *base = warp_mem(ctx, 0, 0);
    if (!base) {
        return 0;
    }
    return *(uint32_t *)(void *)(base - Basedata::FromEnd::actualLinMemByteSize);
}

static int
warp_restore_linear_window(WarpCallContext *ctx, uint32_t offset, uint32_t size)
{
    if (!ctx || !ctx->module || size == 0) {
        return -1;
    }
    uint8_t *base = warp_linear_mem_window(ctx, offset, size);
    if (!base) {
        return -1;
    }
    uint64_t virt = (uint64_t)(uintptr_t)base;
    if (virt < KERNEL_HIGHER_HALF_BASE || (virt & 0xFFFULL) != 0) {
        return -1;
    }
    uint64_t pages = ((uint64_t)size + 0xFFFULL) / 0x1000ULL;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t page_virt = virt + i * 0x1000ULL;
        uint64_t page_phys = page_virt - KERNEL_HIGHER_HALF_BASE;
        if (paging_map_4k(page_virt,
                          page_phys,
                          MEM_REGION_FLAG_READ |
                              MEM_REGION_FLAG_WRITE |
                              MEM_REGION_FLAG_EXEC) != 0) {
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// fs_buffer_borrow / fs_buffer_release
// ---------------------------------------------------------------------------

static uint32_t
warp_fs_buffer_borrow(uint32_t source_endpoint, uint32_t flags, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0, source_owner = 0;
    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);
    if (flags == 0 || (flags & ~0x3u) != 0) return (uint32_t)IPC_ERR_INVALID;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)IPC_ERR_PERM;
    if (!proc || (!warp_process_name_eq(proc->name, "fs-manager")
                  && !capability_has(context_id, CAP_DMA_BUFFER)))
        return (uint32_t)IPC_ERR_PERM;
    if (ipc_endpoint_owner((uint32_t)source_endpoint, &source_owner) != IPC_OK
        || source_owner == 0 || source_owner == context_id)
        return (uint32_t)IPC_ERR_PERM;
    return (uint32_t)process_manager_buffer_borrow_context(
        (uint32_t)PM_BUFFER_KIND_FILESYSTEM, context_id, source_owner, flags);
}

static uint32_t
warp_fs_buffer_release(void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)IPC_ERR_PERM;
    if (!proc || (!warp_process_name_eq(proc->name, "fs-manager")
                  && !capability_has(context_id, CAP_DMA_BUFFER)))
        return (uint32_t)IPC_ERR_PERM;
    return (uint32_t)process_manager_buffer_release_context(
        (uint32_t)PM_BUFFER_KIND_FILESYSTEM, context_id);
}

// ---------------------------------------------------------------------------
// sched_cpu_stats
// ---------------------------------------------------------------------------

static uint32_t
warp_sched_cpu_stats(uint32_t cpu_id, uint32_t out_off, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    typedef struct { uint32_t ready_count; uint32_t running_pid;
                     uint32_t steal_count; uint32_t dispatch_count;
                     uint32_t last_pid; } cpu_stats_t;
    if (cpu_id >= g_cpu_count) return (uint32_t)-1;
    uint8_t *raw = warp_mem(ctx, out_off, sizeof(cpu_stats_t));
    if (!raw) return (uint32_t)-1;
    cpu_sched_t *cs = &g_cpus[cpu_id].sched;
    uint32_t ready = 0;
    for (int p = 0; p < SCHED_PRIO_MAX; p++) ready += cs->thread_count[p];
    cpu_stats_t st;
    st.ready_count    = ready;
    st.running_pid    = g_cpus[cpu_id].current_process
                        ? g_cpus[cpu_id].current_process->pid : 0;
    st.steal_count    = g_cpus[cpu_id].steal_count;
    st.dispatch_count = g_cpus[cpu_id].dispatch_count;
    st.last_pid       = g_cpus[cpu_id].last_dispatched_pid;
    __builtin_memcpy(raw, &st, sizeof(st));
    return 0;
}

// ---------------------------------------------------------------------------
// proc_info / proc_info_ex / proc_info_stats
// ---------------------------------------------------------------------------

static uint32_t
warp_proc_info(uint32_t index, uint32_t buf_off, uint32_t buf_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)buf_len <= 0) return (uint32_t)-1;
    uint8_t *buf = warp_mem(ctx, buf_off, buf_len);
    if (!buf) return (uint32_t)-1;
    uint32_t pid = 0; const char *name = nullptr;
    if (process_info_at(index, &pid, &name) != 0) return (uint32_t)-1;
    uint32_t nlen = 0;
    if (name) while (name[nlen] && nlen + 1u < buf_len) nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

static uint32_t
warp_proc_info_ex(uint32_t index, uint32_t buf_off, uint32_t buf_len,
                  uint32_t parent_off, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)buf_len <= 0) return (uint32_t)-1;
    uint8_t *buf = warp_mem(ctx, buf_off, buf_len);
    uint8_t *par = warp_mem(ctx, parent_off, sizeof(uint32_t));
    if (!buf || !par) return (uint32_t)-1;
    uint32_t pid = 0, parent_pid = 0; const char *name = nullptr;
    if (process_info_at_ex(index, &pid, &parent_pid, &name) != 0) return (uint32_t)-1;
    __builtin_memcpy(par, &parent_pid, sizeof(parent_pid));
    uint32_t nlen = 0;
    if (name) while (name[nlen] && nlen + 1u < buf_len) nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

static uint32_t
warp_proc_info_stats(uint32_t index, uint32_t buf_off, uint32_t buf_len,
                     uint32_t parent_off, uint32_t stats_off, void *ctx_)
{
    typedef struct {
        uint32_t state; uint32_t block_reason; uint32_t is_wasm;
        uint32_t thread_count; uint32_t live_thread_count;
        uint32_t current_tid; uint32_t context_id;
        uint64_t cpu_ticks; uint64_t vm_total_bytes;
        uint64_t thread_kstack_total_bytes; uint64_t heap_committed_bytes;
        uint64_t rss_est_bytes; uint32_t last_cpu;
    } wasm_proc_stats_t;
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)buf_len <= 0) return (uint32_t)-1;
    uint8_t *buf  = warp_mem(ctx, buf_off,    buf_len);
    uint8_t *par  = warp_mem(ctx, parent_off, sizeof(uint32_t));
    uint8_t *stp  = warp_mem(ctx, stats_off,  sizeof(wasm_proc_stats_t));
    if (!buf || !par || !stp) return (uint32_t)-1;
    uint32_t pid = 0, parent_pid = 0; const char *name = nullptr;
    process_stats_t stats;
    if (process_info_at_stats(index, &pid, &parent_pid, &name, &stats) != 0)
        return (uint32_t)-1;
    __builtin_memcpy(par, &parent_pid, sizeof(parent_pid));
    auto *out = reinterpret_cast<wasm_proc_stats_t *>(stp);
    out->state                     = stats.state;
    out->block_reason              = stats.block_reason;
    out->is_wasm                   = stats.is_wasm;
    out->thread_count              = stats.thread_count;
    out->live_thread_count         = stats.live_thread_count;
    out->current_tid               = stats.current_tid;
    out->context_id                = stats.context_id;
    out->cpu_ticks                 = stats.cpu_ticks;
    out->vm_total_bytes            = stats.vm_total_bytes;
    out->thread_kstack_total_bytes = stats.thread_kstack_total_bytes;
    out->heap_committed_bytes      = stats.heap_committed_bytes;
    out->rss_est_bytes             = stats.rss_est_bytes;
    out->last_cpu                  = stats.last_cpu;
    uint32_t nlen = 0;
    if (name) while (name[nlen] && nlen + 1u < buf_len) nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

// ---------------------------------------------------------------------------
// Thread operations
// ---------------------------------------------------------------------------

static uint32_t
warp_thread_create(uint32_t entry_off, uint32_t arg0, uint32_t arg1,
                   uint32_t flags, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)entry_off <= 0) return (uint32_t)-1;
    const uint8_t *name_raw = warp_mem(ctx, entry_off, 1);
    if (!name_raw) return (uint32_t)-1;
    /* Scan for NUL within 64 bytes */
    uint32_t mem_size = ctx->module->getLinearMemorySizeInPages() << 16;
    if (entry_off >= mem_size) return (uint32_t)-1;
    uint32_t avail = mem_size - entry_off;
    if (avail > 64u) avail = 64u;
    uint8_t *nm = warp_mem(ctx, entry_off, avail);
    if (!nm) return (uint32_t)-1;
    uint8_t ok = 0;
    for (uint32_t i = 0; i < avail; ++i) if (nm[i] == '\0') { ok = 1; break; }
    if (!ok) return (uint32_t)-1;
    const char *entry_name = reinterpret_cast<const char *>(nm);
    uint32_t argc = (flags & 0x1u) ? 2u : 0u;
    uint32_t argv[2] = { arg0, arg1 };
    uint32_t tid = 0;
    if (wasm_driver_spawn_vm_thread(ctx->pid, entry_name, argc, argv, &tid) != 0)
        return (uint32_t)-1;
    return tid;
}

static uint32_t warp_thread_yield(void *ctx_)
    { (void)ctx_; process_yield(PROCESS_RUN_YIELDED); return 0; }

static uint32_t
warp_thread_exit(uint32_t status, void *ctx_)
{
    (void)ctx_;
    process_t *proc = process_get(process_current_pid());
    if (!proc) return (uint32_t)-1;
    process_set_exit_status(proc, (int32_t)status);
    process_yield(PROCESS_RUN_THREAD_EXITED);
    return 0;
}

static uint32_t
warp_thread_join(uint32_t tid, void *ctx_)
{
    (void)ctx_;
    process_t *proc = process_get(process_current_pid());
    if (!proc) return (uint32_t)-1;
    int32_t exit_status = 0;
    int rc = process_thread_join(proc, tid, &exit_status);
    if (rc > 0) { process_yield(PROCESS_RUN_BLOCKED); return 0; }
    if (rc < 0) return (uint32_t)-1;
    return (uint32_t)exit_status;
}

static uint32_t
warp_thread_detach(uint32_t tid, void *ctx_)
{
    (void)ctx_;
    process_t *proc = process_get(process_current_pid());
    if (!proc) return (uint32_t)-1;
    return (uint32_t)process_thread_detach(proc, tid);
}

// ---------------------------------------------------------------------------
// Shared memory
// ---------------------------------------------------------------------------

static uint32_t
warp_shmem_create(uint32_t pages, uint32_t flags, void *ctx_)
{
    (void)ctx_;
    if ((int32_t)pages <= 0) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_dma_capability(context_id) != 0) return (uint32_t)-1;
    uint32_t id = 0; uint64_t phys = 0;
    uint32_t cflags = flags ? flags : (MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE);
    if (mm_shared_create(context_id, (uint64_t)pages, cflags, &id, &phys) != 0)
        return (uint32_t)-1;
    (void)phys;
    return id;
}

static uint32_t
warp_shmem_grant(uint32_t id, uint32_t target_pid, void *ctx_)
{
    (void)ctx_;
    if ((int32_t)id <= 0 || (int32_t)target_pid <= 0) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_dma_capability(context_id) != 0) return (uint32_t)-1;
    process_t *tgt = process_get(target_pid);
    if (!tgt || tgt->context_id == 0) return (uint32_t)-1;
    return (uint32_t)mm_shared_grant(context_id, id, tgt->context_id);
}

static uint32_t
warp_shmem_revoke(uint32_t id, uint32_t target_pid, void *ctx_)
{
    (void)ctx_;
    if ((int32_t)id <= 0 || (int32_t)target_pid <= 0) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_dma_capability(context_id) != 0) return (uint32_t)-1;
    process_t *tgt = process_get(target_pid);
    if (!tgt || tgt->context_id == 0) return (uint32_t)-1;
    return (uint32_t)mm_shared_revoke(context_id, id, tgt->context_id);
}

static uint32_t
warp_shmem_map(uint32_t id, uint32_t wasm_off, uint32_t size, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0 || (size & 0xFFF)) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_dma_capability(context_id) != 0) return (uint32_t)-1;
    uint64_t phys_base = 0; uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0
        || shared_pages == 0) return (uint32_t)-1;
    if ((uint64_t)size < shared_pages * 0x1000ULL) return (uint32_t)-1;
    /* Commit the range via probe() BEFORE paging_map_4k (see warp_shmem_map_auto
     * for the rationale — ensureLinearSize would zero the shmem pages otherwise). */
    ctx->module->getLinearMemoryRegion(wasm_off + size - 1, 1);
    uint8_t *lmem = ctx->module->getLinearMemoryRegion(0, 0);
    if (!lmem) return (uint32_t)-1;
    lmem += wasm_off;
    if ((uint64_t)(uintptr_t)lmem & 0xFFF) return (uint32_t)-1;
    uint64_t virt = (uint64_t)(uintptr_t)lmem;
    for (uint64_t i = 0; i < shared_pages; ++i) {
        paging_map_4k(virt + i * 0x1000ULL, phys_base + i * 0x1000ULL, 3ULL);
    }
    if (mm_shared_retain(context_id, id) != 0) return (uint32_t)-1;
    warp_shmem_map_track(ctx->pid, id, wasm_off, size);
    return 0;
}

static uint32_t
warp_shmem_map_auto(uint32_t id, uint32_t size, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0 || (size & 0xFFF)) {
        klog_printf("[warp-shmem] map_auto invalid id=%u size=%u\n", id, size);
        return (uint32_t)-1;
    }
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_dma_capability(context_id) != 0) {
        klog_printf("[warp-shmem] map_auto ctx/cap deny id=%u\n", id);
        return (uint32_t)-1;
    }
    uint64_t phys_base = 0; uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0
        || shared_pages == 0) {
        klog_printf("[warp-shmem] map_auto get_phys fail ctx=%u id=%u\n", context_id, id);
        return (uint32_t)-1;
    }
    if ((uint64_t)size < shared_pages * 0x1000ULL) {
        klog_printf("[warp-shmem] map_auto size short id=%u size=%u pages=%llu\n",
                    id, size, (unsigned long long)shared_pages);
        return (uint32_t)-1;
    }
    /* Scan linear memory for a free, page-aligned, non-overlapping window.
     * Start from the current active/committed linear-memory size instead of a
     * fixed 2 MiB floor so the first shmem map stays inside memory WARP has
     * already extended. That avoids the Could_not_extend_linear_memory fault
     * seen when gfx_smoke/menu_bar perform their first libui buffer map.
     * TODO(warp-shmem-map-auto): reserve windows against actual heap growth
     * instead of relying on a fixed post-active guard band. */
    /* Use the WARP-configured heap limit rather than the WASM binary's
     * declared page count.  Zig freestanding binaries declare only the
     * minimum memory they need statically (often 1 page = 64 KB) regardless
     * of heap_pages in linker.metadata, so getLinearMemorySizeInPages() gives
     * too small an upper bound for the shmem scan.  Fall back to the
     * configured heap ceiling when it is larger. */
    uint32_t mem_pages = ctx->module->getLinearMemorySizeInPages();
    uint64_t cfg_bytes = warp_heap_committed_bytes(ctx->pid);
    if (cfg_bytes > (uint64_t)mem_pages << 16)
        mem_pages = (uint32_t)((cfg_bytes + 0xFFFFULL) >> 16);
    uint64_t mem_size  = (uint64_t)mem_pages << 16;
    uint64_t scan_min = (uint64_t)warp_linear_memory_active_size(ctx) + 0x10000ULL;
    uint8_t *base = ctx->module->getLinearMemoryRegion(0, 0);
    uint64_t base_mod = base ? ((uint64_t)(uintptr_t)base & 0xFFFULL) : 0;
    if (scan_min < 0x4000ULL) {
        scan_min = 0x4000ULL;
    }
    uint32_t found_off = 0; uint8_t found = 0;
    /* WARP's host linear-memory base is not guaranteed to be page-aligned.
     * Choose guest offsets that make the resulting host virtual address
     * page-aligned, because paging_map_4k remaps host pages, not guest
     * offsets. */
    for (uint64_t off = ((scan_min + base_mod + 0xFFFULL) & ~0xFFFULL) - base_mod;
         off + size <= mem_size; off += 0x1000ULL) {
        uint32_t off32 = (uint32_t)off;
        if (warp_shmem_map_overlaps(ctx->pid, off32, size)) continue;
        /* warp_linear_mem_window uses getLinearMemorySizeInPages() which may
         * reflect only the WASM binary's declared minimum (e.g. 1 page for
         * Zig freestanding apps), not the configured heap.  Compute alignment
         * directly and validate against our extended mem_size instead. */
        if (!base) continue;
        uint8_t *p = base + off32;
        if ((uint64_t)(uintptr_t)p & 0xFFF) continue;
        found_off = off32; found = 1; break;
    }
    if (!found) {
        klog_printf("[warp-shmem] map_auto no-window id=%u size=%u active=%u mem=%llu\n",
                    id, size, warp_linear_memory_active_size(ctx),
                    (unsigned long long)mem_size);
        return (uint32_t)-1;
    }
    /* Commit the target range via probe() BEFORE paging_map_4k.
     * ensureLinearSize() zero-initialises newly committed WASM pages.  If the
     * probe fires AFTER paging_map_4k, the memset writes zeros to the remapped
     * shmem physical pages — corrupting the shared buffer. */
    ctx->module->getLinearMemoryRegion(found_off + size - 1, 1);
    /* Re-fetch lmem after potential syncBasedataStart from ensureLinearSize. */
    uint8_t *lmem = ctx->module->getLinearMemoryRegion(0, 0);
    if (!lmem) return (uint32_t)-1;
    lmem += found_off;
    if ((uint64_t)(uintptr_t)lmem & 0xFFF) return (uint32_t)-1;  /* alignment */

    uint64_t virt = (uint64_t)(uintptr_t)lmem;
    for (uint64_t i = 0; i < shared_pages; ++i) {
        paging_map_4k(virt + i * 0x1000ULL, phys_base + i * 0x1000ULL, 3ULL);
    }
    if (mm_shared_retain(context_id, id) != 0) {
        klog_printf("[warp-shmem] map_auto retain fail ctx=%u id=%u\n", context_id, id);
        return (uint32_t)-1;
    }
    warp_shmem_map_track(ctx->pid, id, found_off, size);
    return found_off;
}

static uint32_t
warp_shmem_unmap(uint32_t id, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    WarpShmemLinearMap *slot = warp_shmem_map_find(process_current_pid(), id);
    if (slot && warp_restore_linear_window(ctx, slot->offset, slot->size) != 0) {
        return (uint32_t)-1;
    }
    warp_shmem_map_untrack(process_current_pid(), id);
    return (uint32_t)mm_shared_release(context_id, id);
}

static uint32_t
warp_shmem_flush(uint32_t id, uint32_t wasm_off, uint32_t size, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_dma_capability(context_id) != 0) return (uint32_t)-1;
    uint64_t phys_base = 0; uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0
        || shared_pages == 0 || phys_base == 0) return (uint32_t)-1;
    if ((uint64_t)size > shared_pages * 0x1000ULL) return (uint32_t)-1;
    const uint8_t *src = warp_linear_mem_window(ctx, wasm_off, size);
    if (!src) return (uint32_t)-1;
    __builtin_memcpy((void *)(uintptr_t)(phys_base | KERNEL_HIGHER_HALF_BASE), src, size);
    return 0;
}

static uint32_t
warp_shmem_refresh(uint32_t id, uint32_t wasm_off, uint32_t size, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_dma_capability(context_id) != 0) return (uint32_t)-1;
    uint64_t phys_base = 0; uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0
        || shared_pages == 0 || phys_base == 0) return (uint32_t)-1;
    if ((uint64_t)size > shared_pages * 0x1000ULL) return (uint32_t)-1;
    /* shmem_refresh writes into a region already committed by shmem_map_auto.
     * Use warp_linear_mem_window (size=0 probe, no ensureLinearSize) instead of
     * getLinearMemoryRegion with a non-zero size, which would trigger
     * ensureLinearSize and potentially extend the linear memory past its current
     * limit causing a page fault at the new boundary. */
    uint8_t *dst = warp_linear_mem_window(ctx, wasm_off, size);
    if (!dst) return (uint32_t)-1;
    __builtin_memcpy(dst, (const void *)(uintptr_t)(phys_base | KERNEL_HIGHER_HALF_BASE), size);
    return 0;
}

// ---------------------------------------------------------------------------
// IRQ routing
// ---------------------------------------------------------------------------

static uint32_t
warp_irq_route_ipc(uint32_t irq_line, uint32_t endpoint, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_irq_capability(context_id) != 0) return (uint32_t)-1;
    return (uint32_t)irq_register(context_id, irq_line, endpoint);
}

static uint32_t
warp_irq_ack(uint32_t irq_line, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    return (uint32_t)irq_ack(context_id, irq_line);
}

static uint32_t
warp_irq_unroute(uint32_t irq_line, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_irq_capability(context_id) != 0) return (uint32_t)-1;
    return (uint32_t)irq_unregister(context_id, irq_line);
}

// ---------------------------------------------------------------------------
// Serial / input
// ---------------------------------------------------------------------------

static uint32_t warp_serial_register(uint32_t endpoint, void *ctx_)
    { (void)ctx_; return (uint32_t)serial_register_remote_driver(endpoint); }

static uint32_t warp_input_push(uint32_t ch, void *ctx_)
    { (void)ctx_; serial_input_push((uint8_t)(ch & 0xFF)); return 0; }

static uint32_t
warp_input_read(void *ctx_)
{
    (void)ctx_;
    uint8_t ch = 0;
    return serial_input_read(&ch) ? (uint32_t)ch : (uint32_t)-1;
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

static uint32_t warp_framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color, void *ctx_)
    { (void)ctx_; return (uint32_t)framebuffer_put_pixel(x, y, color); }

static uint32_t
warp_framebuffer_info(uint32_t out_off, uint32_t len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)len < (int32_t)sizeof(framebuffer_info_t)) return (uint32_t)-1;
    framebuffer_info_t info;
    __builtin_memset(&info, 0, sizeof(info));
    if (framebuffer_get_info(&info) != 0) return (uint32_t)-1;
    uint8_t *out = warp_mem(ctx, out_off, sizeof(framebuffer_info_t));
    if (!out) return (uint32_t)-1;
    __builtin_memcpy(out, &info, sizeof(info));
    return 0;
}

static uint32_t
warp_framebuffer_map(uint32_t wasm_off, uint32_t size, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)size <= 0 || (size & 0xFFF)) return (uint32_t)-1;
    framebuffer_info_t info;
    __builtin_memset(&info, 0, sizeof(info));
    if (framebuffer_get_info(&info) != 0) return (uint32_t)-1;
    if (size < info.framebuffer_size) return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0
        || warp_require_mmio_capability(context_id) != 0) return (uint32_t)-1;
    uint8_t *lmem = warp_linear_mem_window(ctx, wasm_off, size);
    if (!lmem || ((uint64_t)(uintptr_t)lmem & 0xFFF)) return (uint32_t)-1;
    uint64_t virt = (uint64_t)(uintptr_t)lmem;
    uint64_t phys = info.framebuffer_base;
    uint64_t pages = (uint64_t)size / 0x1000ULL;
    for (uint64_t i = 0; i < pages; ++i) {
        paging_unmap_4k(virt + i * 0x1000ULL);
        if (paging_map_4k(virt + i * 0x1000ULL, phys + i * 0x1000ULL, 3ULL) < 0)
            return (uint32_t)-1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Boot config
// ---------------------------------------------------------------------------

static uint32_t
warp_boot_config_size(void *ctx_)
{
    (void)ctx_;
    if (!g_warp_boot_info || !g_warp_boot_info->boot_config
        || g_warp_boot_info->boot_config_size == 0) return (uint32_t)-1;
    return (uint32_t)g_warp_boot_info->boot_config_size;
}

static uint32_t
warp_boot_config_copy(uint32_t buf_off, uint32_t len, uint32_t offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if (!g_warp_boot_info || !g_warp_boot_info->boot_config) return (uint32_t)-1;
    uint32_t total = (uint32_t)g_warp_boot_info->boot_config_size;
    if (offset > total || len > total - offset) return (uint32_t)-1;
    if (len == 0) return 0;
    uint8_t *dst = warp_mem(ctx, buf_off, len);
    if (!dst) return (uint32_t)-1;
    const uint8_t *src = static_cast<const uint8_t *>(g_warp_boot_info->boot_config);
    __builtin_memcpy(dst, src + offset, len);
    return 0;
}

// ---------------------------------------------------------------------------
// initfs_find_path
// ---------------------------------------------------------------------------

static uint32_t
warp_initfs_find_path(uint32_t path_off, uint32_t path_len, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    if ((int32_t)path_len <= 0 || path_len >= 112u) return (uint32_t)-1;
    const uint8_t *raw = warp_mem(ctx, path_off, path_len);
    if (!raw) return (uint32_t)-1;
    char local_path[112];
    __builtin_memcpy(local_path, raw, path_len);
    local_path[path_len] = '\0';
    uint32_t ri = 0;
    while (local_path[ri] == '/') ri++;
    if ((local_path[ri]=='i'||local_path[ri]=='I') &&
        (local_path[ri+1]=='n'||local_path[ri+1]=='N') &&
        (local_path[ri+2]=='i'||local_path[ri+2]=='I') &&
        (local_path[ri+3]=='t'||local_path[ri+3]=='T') &&
        local_path[ri+4]=='/')
        ri += 5;
    if (local_path[ri] == '\0') return (uint32_t)-1;
    const wasmos_initfs_header_t *hdr = nullptr; const uint8_t *base = nullptr;
    if (warp_initfs_header_get(&hdr, &base) != 0) return (uint32_t)-1;
    for (uint32_t i = 0; i < hdr->entry_count; ++i) {
        wasmos_initfs_entry_t e;
        if (warp_initfs_entry_at(i, &e) != 0) continue;
        if (__builtin_strcmp(e.path, &local_path[ri]) == 0) return i;
        const char *bn = e.path;
        for (uint32_t j = 0; e.path[j]; ++j)
            if (e.path[j] == '/') bn = &e.path[j+1];
        if (__builtin_strcmp(bn, &local_path[ri]) == 0) return i;
    }
    return (uint32_t)-1;
}

// ---------------------------------------------------------------------------
// Early log
// ---------------------------------------------------------------------------

static uint32_t warp_early_log_size(void *ctx_)
    { (void)ctx_; return (uint32_t)serial_early_log_size(); }

static uint32_t
warp_early_log_copy(uint32_t buf_off, uint32_t len, uint32_t offset, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    uint32_t total = (uint32_t)serial_early_log_size();
    if (offset > total || len > total - offset) return (uint32_t)-1;
    if (len == 0) return 0;
    uint8_t *dst = warp_mem(ctx, buf_off, len);
    if (!dst) return (uint32_t)-1;
    serial_early_log_copy(dst, offset, len);
    return 0;
}

static uint32_t warp_debug_mark(uint32_t /*tag*/, void *ctx_)
    { (void)ctx_; return 0; }
static uint32_t warp_kmap_dump(void *ctx_)
    { (void)ctx_; return 0; }
static uint32_t warp_kmap_dump_all(void *ctx_)
    { (void)ctx_; return 0; }
static uint32_t warp_sched_ready_count(void *ctx_)
    { (void)ctx_; return 0; }
static uint32_t warp_sched_cpu_count(void *ctx_)
    { (void)ctx_; return (uint32_t)g_cpu_count; }
static uint32_t warp_kernel_runtime(void *ctx_)
    { (void)ctx_; return 1u; /* WARP */ }

static uint32_t
warp_physmem_stats(uint32_t out_off, void *ctx_)
{
    auto *ctx = warp_call_ctx(ctx_);
    typedef struct { uint64_t total_bytes; uint64_t free_bytes; } physmem_stats_t;
    uint8_t *raw = warp_mem(ctx, out_off, sizeof(physmem_stats_t));
    if (!raw) return (uint32_t)-1;
    physmem_stats_t tmp;
    tmp.total_bytes = pfa_total_bytes();
    tmp.free_bytes  = pfa_free_bytes();
    __builtin_memcpy(raw, &tmp, sizeof(tmp));
    return 0;
}

// ---------------------------------------------------------------------------
// Symbol accessor.
//
// The NativeSymbol table is built as a function-local static so that
/* AssemblyScript runtime abort(msg, file, line, column) → exit process.
 * Called by AS-compiled modules on assertion failure or trap.
 * WASM signature: void(i32,i32,i32,i32). */
static void
warp_env_abort(uint32_t msg, uint32_t file, uint32_t line, uint32_t column, void *ctx_)
{
    (void)msg; (void)file; (void)line; (void)column; (void)ctx_;
    process_t *proc = process_get(process_current_pid());
    if (proc) {
        process_set_exit_status(proc, -1);
        process_yield(PROCESS_RUN_EXITED);
    }
}

/* WASMOS_SYMBOLS(LINK) expands to the full hostcall symbol list using LINK as
 * the linkage macro (STATIC_LINK or DYNAMIC_LINK).  Having a single source of
 * truth avoids the two tables getting out of sync.
 *
 * - STATIC_LINK: bakes function pointers into the JIT-compiled code at
 *   compile time.  Used for initFromBytecode (JIT path) — leaner basedata.
 * - DYNAMIC_LINK: uses an indirection table resolved at load time.  Required
 *   by initFromCompiledBinary (AOT path) which throws Wrong_type for STATIC. */
#define WASMOS_SYMBOLS(LINK) \
    LINK("wasmos", "ipc_create_endpoint", warp_ipc_create_endpoint), \
    LINK("wasmos", "ipc_endpoint_owner",  warp_ipc_endpoint_owner), \
    LINK("wasmos", "ipc_send",             warp_ipc_send), \
    LINK("wasmos", "ipc_select_one",       warp_ipc_select_one), \
    LINK("wasmos", "ipc_recv",             warp_ipc_select_one), \
    LINK("wasmos", "ipc_drain",            warp_ipc_drain), \
    LINK("wasmos", "ipc_try_recv",         warp_ipc_drain), \
    LINK("wasmos", "ipc_notify",           warp_ipc_notify), \
    LINK("wasmos", "ipc_last_field",       warp_ipc_last_field), \
    LINK("wasmos", "console_read",         warp_console_read), \
    LINK("wasmos", "console_write",        warp_console_write), \
    LINK("wasmos", "proc_exit",            warp_proc_exit), \
    LINK("wasmos", "proc_notify_ready",    warp_proc_notify_ready), \
    LINK("wasmos", "sched_yield",          warp_sched_yield), \
    LINK("wasmos", "sched_current_pid",    warp_sched_current_pid), \
    LINK("wasmos", "thread_gettid",        warp_thread_gettid), \
    LINK("wasmos", "futex_wait",           warp_futex_wait), \
    LINK("wasmos", "futex_wake",           warp_futex_wake), \
    LINK("wasmos", "ipc_select_create",    warp_ipc_select_create), \
    LINK("wasmos", "ipc_select_add",       warp_ipc_select_add), \
    LINK("wasmos", "ipc_select_wait",      warp_ipc_select_wait), \
    LINK("wasmos", "ipc_select_destroy",   warp_ipc_select_destroy), \
    LINK("wasmos", "sys_select_create",    warp_ipc_select_create), \
    LINK("wasmos", "sys_select_add",       warp_ipc_select_add), \
    LINK("wasmos", "sys_select_wait",      warp_ipc_select_wait), \
    LINK("wasmos", "sys_select_destroy",   warp_ipc_select_destroy), \
    LINK("wasmos", "fs_buffer_size",       warp_fs_buffer_size), \
    LINK("wasmos", "fs_endpoint",          warp_fs_endpoint), \
    LINK("wasmos", "fs_buffer_copy",       warp_fs_buffer_copy), \
    LINK("wasmos", "fs_buffer_write",      warp_fs_buffer_write), \
    LINK("wasmos", "buffer_borrow",        warp_buffer_borrow), \
    LINK("wasmos", "buffer_release",       warp_buffer_release), \
    LINK("wasmos", "block_buffer_phys",    warp_block_buffer_phys), \
    LINK("wasmos", "block_buffer_copy",    warp_block_buffer_copy), \
    LINK("wasmos", "block_buffer_write",   warp_block_buffer_write), \
    LINK("wasmos", "io_in8",               warp_io_in8), \
    LINK("wasmos", "io_in16",              warp_io_in16), \
    LINK("wasmos", "io_in32",              warp_io_in32), \
    LINK("wasmos", "io_out8",              warp_io_out8), \
    LINK("wasmos", "io_out16",             warp_io_out16), \
    LINK("wasmos", "io_out32",             warp_io_out32), \
    LINK("wasmos", "io_wait",              warp_io_wait), \
    LINK("wasmos", "acpi_rsdp_info",       warp_acpi_rsdp_info), \
    LINK("wasmos", "boot_module_name",     warp_boot_module_name), \
    LINK("wasmos", "sync_user_read",       warp_sync_user_read), \
    LINK("wasmos", "system_halt",          warp_system_halt), \
    LINK("wasmos", "system_reboot",        warp_system_reboot), \
    LINK("wasmos", "sched_ticks",          warp_sched_ticks), \
    LINK("wasmos", "proc_count",           warp_proc_count), \
    LINK("wasmos", "sched_ready_count",    warp_sched_ready_count), \
    LINK("wasmos", "sched_cpu_count",      warp_sched_cpu_count), \
    LINK("wasmos", "physmem_stats",        warp_physmem_stats), \
    LINK("wasmos", "kernel_runtime",       warp_kernel_runtime), \
    LINK("wasmos", "debug_mark",           warp_debug_mark), \
    LINK("wasmos", "kmap_dump",            warp_kmap_dump), \
    LINK("wasmos", "kmap_dump_all",        warp_kmap_dump_all), \
    LINK("wasmos", "initfs_entry_count",   warp_initfs_entry_count), \
    LINK("wasmos", "initfs_entry_name",    warp_initfs_entry_name), \
    LINK("wasmos", "initfs_entry_size",    warp_initfs_entry_size), \
    LINK("wasmos", "initfs_entry_copy",    warp_initfs_entry_copy), \
    LINK("wasmos", "dma_map_borrow",       warp_dma_map_borrow), \
    LINK("wasmos", "dma_sync_borrow",      warp_dma_sync_borrow), \
    LINK("wasmos", "dma_unmap_borrow",     warp_dma_unmap_borrow), \
    LINK("wasmos", "phys_map",             warp_phys_map), \
    LINK("wasmos", "proc_info",            warp_proc_info), \
    LINK("wasmos", "proc_info_ex",         warp_proc_info_ex), \
    LINK("wasmos", "proc_info_stats",      warp_proc_info_stats), \
    LINK("wasmos", "fs_buffer_borrow",     warp_fs_buffer_borrow), \
    LINK("wasmos", "fs_buffer_release",    warp_fs_buffer_release), \
    LINK("wasmos", "sched_cpu_stats",      warp_sched_cpu_stats), \
    LINK("wasmos", "thread_create",        warp_thread_create), \
    LINK("wasmos", "thread_yield",         warp_thread_yield), \
    LINK("wasmos", "thread_exit",          warp_thread_exit), \
    LINK("wasmos", "thread_join",          warp_thread_join), \
    LINK("wasmos", "thread_detach",        warp_thread_detach), \
    LINK("wasmos", "shmem_create",         warp_shmem_create), \
    LINK("wasmos", "shmem_grant",          warp_shmem_grant), \
    LINK("wasmos", "shmem_revoke",         warp_shmem_revoke), \
    LINK("wasmos", "shmem_map",            warp_shmem_map), \
    LINK("wasmos", "shmem_map_auto",       warp_shmem_map_auto), \
    LINK("wasmos", "shmem_flush",          warp_shmem_flush), \
    LINK("wasmos", "shmem_refresh",        warp_shmem_refresh), \
    LINK("wasmos", "shmem_unmap",          warp_shmem_unmap), \
    LINK("wasmos", "irq_route_ipc",        warp_irq_route_ipc), \
    LINK("wasmos", "irq_ack",              warp_irq_ack), \
    LINK("wasmos", "irq_unroute",          warp_irq_unroute), \
    LINK("wasmos", "serial_register",      warp_serial_register), \
    LINK("wasmos", "input_push",           warp_input_push), \
    LINK("wasmos", "input_read",           warp_input_read), \
    LINK("wasmos", "framebuffer_info",     warp_framebuffer_info), \
    LINK("wasmos", "framebuffer_map",      warp_framebuffer_map), \
    LINK("wasmos", "framebuffer_pixel",    warp_framebuffer_pixel), \
    LINK("wasmos", "boot_config_size",     warp_boot_config_size), \
    LINK("wasmos", "boot_config_copy",     warp_boot_config_copy), \
    LINK("wasmos", "initfs_find_path",     warp_initfs_find_path), \
    LINK("wasmos", "early_log_size",       warp_early_log_size), \
    LINK("wasmos", "early_log_copy",       warp_early_log_copy), \
    LINK("wasmos", "env_get",              warp_env_get), \
    LINK("wasmos", "env_set",              warp_env_set), \
    LINK("wasmos", "env_unset",            warp_env_unset), \
    LINK("env",    "abort",                warp_env_abort)

vb::Span<vb::NativeSymbol const>
warp_wasmos_symbols(void)
{
    // STATIC_LINK: bakes function pointers into call stubs at JIT compile time.
    // Smaller basedata → no reallocation pressure during JIT execution.
    static vb::NativeSymbol syms[] = { WASMOS_SYMBOLS(STATIC_LINK) };
    return vb::Span<vb::NativeSymbol const>(syms, sizeof(syms) / sizeof(syms[0]));
}

// Used by initFromCompiledBinary (AOT load path).  DYNAMIC_LINK is required
// because initFromCompiledBinary throws Wrong_type on any STATIC symbol.
vb::Span<vb::NativeSymbol const>
warp_wasmos_symbols_for_aot_load(void)
{
    static vb::NativeSymbol syms[] = { WASMOS_SYMBOLS(DYNAMIC_LINK) };
    return vb::Span<vb::NativeSymbol const>(syms, sizeof(syms) / sizeof(syms[0]));
}

// Context accessor — called from warp_driver.cpp after WasmModule construction
// to wire up the per-module call context so host functions can access linear
// memory and the calling pid.
void
warp_bind_module(vb::WasmModule *module, uint32_t pid)
{
    if (pid >= PROCESS_MAX_COUNT) return;
    g_ctx_table[pid].module    = module;
    g_ctx_table[pid].pid       = pid;
    module->setContext(&g_ctx_table[pid]);
}

void *
warp_context_for_pid(uint32_t pid)
{
    if (pid >= PROCESS_MAX_COUNT) {
        return nullptr;
    }
    return &g_ctx_table[pid];
}

#ifdef WASMOS_WARP_RING3
// ---------------------------------------------------------------------------
// Ring-3 DYNAMIC_LINK symbol table — trampoline VAs as function pointers.
//
// Each R3_LINK entry uses the HC trampoline stub for that HC ID as the `ptr`.
// When WARP's JIT code in ring-3 calls a DYNAMIC_LINK function, it loads the
// ptr from the basedata indirection table (at user VA linMem - offset) and
// calls it.  The ptr is the user-space HC stub address.  The stub fires
// `int 0x80` with RAX = WARP_HC_SYSCALL_BASE + hc_id; the kernel dispatches
// to warp_ring3_dispatch(hc_id, frame).
//
// R3_LINK uses __COUNTER__ to assign sequential HC IDs matching the
// WASMOS_SYMBOLS expansion order. g_r3_sym_counter_base must be declared
// immediately before the expansion so the counter offset is 0-indexed.
// ---------------------------------------------------------------------------

vb::Span<vb::NativeSymbol const>
warp_wasmos_symbols_ring3(void)
{
    static constexpr int g_r3_sym_counter_base = __COUNTER__;
#define R3_LINK(module, name, fnc) \
    vb::NativeSymbol { \
        vb::NativeSymbol::Linkage::DYNAMIC, \
        module, \
        name, \
        vb::function_traits<vb::remove_noexcept_t<decltype(fnc)>>::getSignature(), \
        reinterpret_cast<void const *>(WARP_R3_HC_TRAMPOLINE + \
            (uint64_t)(__COUNTER__ - g_r3_sym_counter_base - 1) * 8ULL) \
    }
    static vb::NativeSymbol syms[] = { WASMOS_SYMBOLS(R3_LINK) };
#undef R3_LINK
    return vb::Span<vb::NativeSymbol const>(syms, sizeof(syms) / sizeof(syms[0]));
}

// ---------------------------------------------------------------------------
// Ring-3 hostcall dispatch
//
// Called from x86_syscall_handler when a ring-3 int 0x80 fires with
// RAX in [WARP_HC_SYSCALL_BASE, WARP_HC_SYSCALL_BASE + WARP_HC_MAX).
// hc_id = RAX - WARP_HC_SYSCALL_BASE (== HC index in WASMOS_SYMBOLS order).
//
// Arguments from ring-3 registers (SysV: RDI, RSI, RDX, RCX, R8, R9) are
// in the saved syscall_frame_t.  For HC_IPC_SEND (9 args), extra args come
// from the user stack at frame+sizeof(syscall_frame_t).
// ctx_ is always the last argument, passed by the JIT from the DYNAMIC_LINK
// basedata slot (which holds the kernel WarpCallContext* set by warp_bind_module).
// ---------------------------------------------------------------------------

extern "C" uint32_t
warp_ring3_dispatch(uint32_t hc_id, void *frame_ptr)
{
    syscall_frame_t *frame = static_cast<syscall_frame_t *>(frame_ptr);

    /* Convenience: user_rsp is just past the end of syscall_frame_t
     * (pushed by CPU for ring-3 → ring-0 INT transition). */
    uint64_t user_rsp = *reinterpret_cast<uint64_t *>(
        reinterpret_cast<uint8_t *>(frame) + sizeof(syscall_frame_t));

    /* Extract common register args. */
    uint64_t a0 = frame->rdi;
    uint64_t a1 = frame->rsi;
    uint64_t a2 = frame->rdx;
    uint64_t a3 = frame->rcx;
    uint64_t a4 = frame->r8;
    uint64_t a5 = frame->r9;
    /* Stack args (past return address at [user_rsp+0]). */
    auto stack_u64 = [user_rsp](uint32_t n) -> uint64_t {
        return *reinterpret_cast<uint64_t *>(user_rsp + 8 + (uint64_t)n * 8);
    };

    /* ctx_ is the last argument in each hostcall. With the SysV ABI used by
     * the ring-3 HC trampolines, the first five argument positions arrive in
     * RDI, RSI, RDX, RCX, and R8 respectively. */
    void *ctx1  = reinterpret_cast<void *>(a0);  /* 1-arg fn: (ctx_) */
    void *ctx2  = reinterpret_cast<void *>(a1);  /* 2-arg fn: (a0,ctx_) */
    void *ctx3  = reinterpret_cast<void *>(a2);  /* 3-arg fn: (a0,a1,ctx_) */
    void *ctx4  = reinterpret_cast<void *>(a3);  /* 4-arg fn */
    void *ctx5  = reinterpret_cast<void *>(a4);  /* 5-arg fn */
    (void)ctx1; (void)ctx2; (void)ctx3; (void)ctx4; (void)ctx5;

    switch (hc_id) {
    /* 0 */ case HC_IPC_CREATE_ENDPOINT:
        return warp_ipc_create_endpoint(reinterpret_cast<void *>(a0));
    /* 1 */ case HC_IPC_ENDPOINT_OWNER:
        return warp_ipc_endpoint_owner((uint32_t)a0, ctx2);
    /* 2 */ case HC_IPC_SEND: {
        /* 9 args: RDI RSI RDX RCX R8 R9 [rsp+8] [rsp+16] [rsp+24] */
        uint32_t s_a2  = (uint32_t)stack_u64(0);
        uint32_t s_a3  = (uint32_t)stack_u64(1);
        void    *s_ctx = reinterpret_cast<void *>(stack_u64(2));
        return warp_ipc_send((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                             (uint32_t)a3, (uint32_t)a4, (uint32_t)a5,
                             s_a2, s_a3, s_ctx);
    }
    /* 3 */ case HC_IPC_SELECT_ONE: /* fall-through: same fn */
    /* 4 */ case HC_IPC_RECV:
        return warp_ipc_select_one((uint32_t)a0, ctx2);
    /* 5 */ case HC_IPC_DRAIN:    /* fall-through */
    /* 6 */ case HC_IPC_TRY_RECV:
        return warp_ipc_drain((uint32_t)a0, ctx2);
    /* 7 */ case HC_IPC_NOTIFY:
        return warp_ipc_notify((uint32_t)a0, ctx2);
    /* 8 */ case HC_IPC_LAST_FIELD:
        return warp_ipc_last_field((uint32_t)a0, ctx2);
    /* 9 */ case HC_CONSOLE_READ:
        return warp_console_read((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 10 */ case HC_CONSOLE_WRITE:
        return warp_console_write((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 11 */ case HC_PROC_EXIT:
        return warp_proc_exit((uint32_t)a0, ctx2);
    /* 12 */ case HC_PROC_NOTIFY_READY:
        return warp_proc_notify_ready(reinterpret_cast<void *>(a0));
    /* 13 */ case HC_SCHED_YIELD:
        return warp_sched_yield(reinterpret_cast<void *>(a0));
    /* 14 */ case HC_SCHED_CURRENT_PID:
        return warp_sched_current_pid(reinterpret_cast<void *>(a0));
    /* 15 */ case HC_THREAD_GETTID:
        return warp_thread_gettid(reinterpret_cast<void *>(a0));
    /* 16 */ case HC_FUTEX_WAIT:
        return warp_futex_wait((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 17 */ case HC_FUTEX_WAKE:
        return warp_futex_wake((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 18 */ case HC_IPC_SELECT_CREATE: /* fall-through */
    /* 22 */ case HC_SYS_SELECT_CREATE:
        return warp_ipc_select_create(reinterpret_cast<void *>(a0));
    /* 19 */ case HC_IPC_SELECT_ADD: /* fall-through */
    /* 23 */ case HC_SYS_SELECT_ADD:
        return warp_ipc_select_add((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 20 */ case HC_IPC_SELECT_WAIT: /* fall-through */
    /* 24 */ case HC_SYS_SELECT_WAIT:
        return warp_ipc_select_wait((uint32_t)a0, ctx2);
    /* 21 */ case HC_IPC_SELECT_DESTROY: /* fall-through */
    /* 25 */ case HC_SYS_SELECT_DESTROY:
        return warp_ipc_select_destroy((uint32_t)a0, ctx2);
    /* 26 */ case HC_FS_BUFFER_SIZE:
        return warp_fs_buffer_size(reinterpret_cast<void *>(a0));
    /* 27 */ case HC_FS_ENDPOINT:
        return warp_fs_endpoint(reinterpret_cast<void *>(a0));
    /* 28 */ case HC_FS_BUFFER_COPY:
        return warp_fs_buffer_copy((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 29 */ case HC_FS_BUFFER_WRITE:
        return warp_fs_buffer_write((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 30 */ case HC_BUFFER_BORROW:
        return warp_buffer_borrow((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 31 */ case HC_BUFFER_RELEASE:
        return warp_buffer_release((uint32_t)a0, ctx2);
    /* 32 */ case HC_BLOCK_BUFFER_PHYS:
        return warp_block_buffer_phys(reinterpret_cast<void *>(a0));
    /* 33 */ case HC_BLOCK_BUFFER_COPY:
        return warp_block_buffer_copy((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                      (uint32_t)a3, ctx5);
    /* 34 */ case HC_BLOCK_BUFFER_WRITE:
        return warp_block_buffer_write((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                       (uint32_t)a3, ctx5);
    /* 35 */ case HC_IO_IN8:
        return warp_io_in8((uint32_t)a0, ctx2);
    /* 36 */ case HC_IO_IN16:
        return warp_io_in16((uint32_t)a0, ctx2);
    /* 37 */ case HC_IO_IN32:
        return warp_io_in32((uint32_t)a0, ctx2);
    /* 38 */ case HC_IO_OUT8:
        return warp_io_out8((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 39 */ case HC_IO_OUT16:
        return warp_io_out16((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 40 */ case HC_IO_OUT32:
        return warp_io_out32((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 41 */ case HC_IO_WAIT:
        return warp_io_wait(reinterpret_cast<void *>(a0));
    /* 42 */ case HC_ACPI_RSDP_INFO:
        return warp_acpi_rsdp_info((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 43 */ case HC_BOOT_MODULE_NAME:
        return warp_boot_module_name((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 44 */ case HC_SYNC_USER_READ:
        return warp_sync_user_read((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 45 */ case HC_SYSTEM_HALT:
        return warp_system_halt(reinterpret_cast<void *>(a0));
    /* 46 */ case HC_SYSTEM_REBOOT:
        return warp_system_reboot(reinterpret_cast<void *>(a0));
    /* 47 */ case HC_SCHED_TICKS:
        return warp_sched_ticks(reinterpret_cast<void *>(a0));
    /* 48 */ case HC_PROC_COUNT:
        return warp_proc_count(reinterpret_cast<void *>(a0));
    /* 49 */ case HC_SCHED_READY_COUNT:
        return warp_sched_ready_count(reinterpret_cast<void *>(a0));
    /* 50 */ case HC_SCHED_CPU_COUNT:
        return warp_sched_cpu_count(reinterpret_cast<void *>(a0));
    /* 51 */ case HC_PHYSMEM_STATS:
        return warp_physmem_stats((uint32_t)a0, ctx2);
    /* 52 */ case HC_KERNEL_RUNTIME:
        return warp_kernel_runtime(reinterpret_cast<void *>(a0));
    /* 53 */ case HC_DEBUG_MARK:
        return warp_debug_mark((uint32_t)a0, ctx2);
    /* 54 */ case HC_KMAP_DUMP:
        return warp_kmap_dump(reinterpret_cast<void *>(a0));
    /* 55 */ case HC_KMAP_DUMP_ALL:
        return warp_kmap_dump_all(reinterpret_cast<void *>(a0));
    /* 56 */ case HC_INITFS_ENTRY_COUNT:
        return warp_initfs_entry_count(reinterpret_cast<void *>(a0));
    /* 57 */ case HC_INITFS_ENTRY_NAME:
        return warp_initfs_entry_name((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 58 */ case HC_INITFS_ENTRY_SIZE:
        return warp_initfs_entry_size((uint32_t)a0, ctx2);
    /* 59 */ case HC_INITFS_ENTRY_COPY:
        return warp_initfs_entry_copy((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                      (uint32_t)a3, ctx5);
    /* 60 */ case HC_DMA_MAP_BORROW:
        return warp_dma_map_borrow((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                   (uint32_t)a3, (uint32_t)a4, ctx5);
    /* 61 */ case HC_DMA_SYNC_BORROW:
        return warp_dma_sync_borrow((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                    (uint32_t)a3, ctx5);
    /* 62 */ case HC_DMA_UNMAP_BORROW:
        return warp_dma_unmap_borrow((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 63 */ case HC_PHYS_MAP:
        return warp_phys_map((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, ctx5);
    /* 64 */ case HC_PROC_INFO:
        return warp_proc_info((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 65 */ case HC_PROC_INFO_EX:
        return warp_proc_info_ex((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                 (uint32_t)a3, ctx5);
    /* 66 */ case HC_PROC_INFO_STATS:
        return warp_proc_info_stats((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                    (uint32_t)a3, (uint32_t)a4, ctx5);
    /* 67 */ case HC_FS_BUFFER_BORROW:
        return warp_fs_buffer_borrow((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 68 */ case HC_FS_BUFFER_RELEASE:
        return warp_fs_buffer_release(reinterpret_cast<void *>(a0));
    /* 69 */ case HC_SCHED_CPU_STATS:
        return warp_sched_cpu_stats((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 70 */ case HC_THREAD_CREATE:
        return warp_thread_create((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                                  (uint32_t)a3, ctx5);
    /* 71 */ case HC_THREAD_YIELD:
        return warp_thread_yield(reinterpret_cast<void *>(a0));
    /* 72 */ case HC_THREAD_EXIT:
        return warp_thread_exit((uint32_t)a0, ctx2);
    /* 73 */ case HC_THREAD_JOIN:
        return warp_thread_join((uint32_t)a0, ctx2);
    /* 74 */ case HC_THREAD_DETACH:
        return warp_thread_detach((uint32_t)a0, ctx2);
    /* 75 */ case HC_SHMEM_CREATE:
        return warp_shmem_create((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 76 */ case HC_SHMEM_GRANT:
        return warp_shmem_grant((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 77 */ case HC_SHMEM_REVOKE:
        return warp_shmem_revoke((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 78 */ case HC_SHMEM_MAP:
        return warp_shmem_map((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 79 */ case HC_SHMEM_MAP_AUTO:
        return warp_shmem_map_auto((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 80 */ case HC_SHMEM_FLUSH:
        return warp_shmem_flush((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 81 */ case HC_SHMEM_REFRESH:
        return warp_shmem_refresh((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 82 */ case HC_SHMEM_UNMAP:
        return warp_shmem_unmap((uint32_t)a0, ctx2);
    /* 83 */ case HC_IRQ_ROUTE_IPC:
        return warp_irq_route_ipc((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 84 */ case HC_IRQ_ACK:
        return warp_irq_ack((uint32_t)a0, ctx2);
    /* 85 */ case HC_IRQ_UNROUTE:
        return warp_irq_unroute((uint32_t)a0, ctx2);
    /* 86 */ case HC_SERIAL_REGISTER:
        return warp_serial_register((uint32_t)a0, ctx2);
    /* 87 */ case HC_INPUT_PUSH:
        return warp_input_push((uint32_t)a0, ctx2);
    /* 88 */ case HC_INPUT_READ:
        return warp_input_read(reinterpret_cast<void *>(a0));
    /* 89 */ case HC_FRAMEBUFFER_INFO:
        return warp_framebuffer_info((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 90 */ case HC_FRAMEBUFFER_MAP:
        return warp_framebuffer_map((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 91 */ case HC_FRAMEBUFFER_PIXEL:
        return warp_framebuffer_pixel((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 92 */ case HC_BOOT_CONFIG_SIZE:
        return warp_boot_config_size(reinterpret_cast<void *>(a0));
    /* 93 */ case HC_BOOT_CONFIG_COPY:
        return warp_boot_config_copy((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 94 */ case HC_INITFS_FIND_PATH:
        return warp_initfs_find_path((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 95 */ case HC_EARLY_LOG_SIZE:
        return warp_early_log_size(reinterpret_cast<void *>(a0));
    /* 96 */ case HC_EARLY_LOG_COPY:
        return warp_early_log_copy((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, ctx4);
    /* 97 */ case HC_ENV_GET:
        return warp_env_get((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, ctx5);
    /* 98 */ case HC_ENV_SET:
        return warp_env_set((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, ctx5);
    /* 99 */ case HC_ENV_UNSET:
        return warp_env_unset((uint32_t)a0, (uint32_t)a1, ctx3);
    /* 100 */ case HC_ENV_ABORT:
        warp_env_abort((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, ctx5);
        return 0;
    default:
        return (uint32_t)-1;
    }
}
#endif /* WASMOS_WARP_RING3 */

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

void
warp_link_init(const boot_info_t *boot_info)
{
    g_warp_boot_info = boot_info;
    warp_ipc_slots_init();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_ctx_table[i] = WarpCallContext{nullptr, i, boot_info};
    }
}

} // extern "C"
