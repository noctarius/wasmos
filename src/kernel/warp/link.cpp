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

#include "../include/xfer_buffer.h"

extern "C" {
#include "boot.h"
#include "warp/shim.h"
#include "ipc.h"
#include "process.h"
#include "process_manager.h"
#include "list.h"
#include "hashmap.h"
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
#include "system_control.h"
#include "wasm_driver.h"
#include "wasmos_driver_abi.h"
#include "wasmos_status.h"
#include "arch/x86_64/smp.h"
}

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/basedataoffsets.hpp"
#include "src/core/common/NativeSymbol.hpp"
#include "src/core/common/Span.hpp"
#include "src/core/common/function_traits.hpp"
#include "link.h"
#ifdef WASMOS_WASM_RUNTIME_WARP
#include "warp_ring3.h"
#include "syscall.h"
extern "C" int warp_mem_ring3_map_linmem(uint64_t user_root, uint8_t const* linmem_kernel_ptr);
extern "C" uint64_t warp_mem_alias_phys(uint64_t virt);
#endif

// ---------------------------------------------------------------------------
// Call context
// ---------------------------------------------------------------------------

struct WarpCallContext {
    vb::WasmModule* module;
    uint32_t pid;
    const boot_info_t* boot_info;
};

static int warp_require_system_control_capability(uint32_t context_id);
#ifdef WASMOS_WASM_RUNTIME_WARP
static int warp_ring3_sync_user_range(WarpCallContext* ctx, uint32_t wasm_off, uint32_t size);
#endif

// Per-process WARP call contexts, keyed by pid.  Backed by a growable hashmap
// (no fixed process-count bound): an entry is created when a process's module
// is bound and removed when the process exits (warp_ctx_release_pid), so the
// set tracks live processes 1:1.  Hashmap value addresses are stable for a
// key's lifetime (chaining + rehash relinks, never moves nodes), so the
// pointer handed to WasmModule::setContext() stays valid.  Pre-sized so the
// spawn hot path (which runs under the preempt-guard drain) does not rehash.
// TODO(smp-warp): lookups/alloc here are unsynchronised; safe under the WARP
// single-CPU invariant (see warp/shim.cpp), revisit for SMP.
static hashmap_t g_ctx_map;

static WarpCallContext* ctx_find(uint32_t pid) {
    if (pid == 0) {
        return nullptr;
    }
    return static_cast<WarpCallContext*>(hashmap_get(&g_ctx_map, pid));
}

/* Page-aligned scratch page used by warp_phys_map for ACPI/physical memory
 * reads.  ACPI physical pages are not necessarily in the kernel direct map
 * (they are EfiACPIReclaimMemory regions), so we cannot use phys|HIGHER_HALF
 * directly.  Instead we remap this kernel BSS page (which IS in the page
 * tables) to the target physical page, copy the data, then restore. */
static uint8_t g_phys_scratch[4096] __attribute__((aligned(4096)));

static inline uint8_t* warp_mem(WarpCallContext* ctx, uint32_t offset, uint32_t size) {
    /* Use the full getLinearMemoryRegion(offset, size) path with non-zero size.
     * With LINEAR_MEMORY_BOUNDS_CHECKS=1, this triggers probe() →
     * ensureLinearSize() which zero-initialises newly-committed WASM pages.
     * When warp_mem is used to obtain a WRITE destination, the zeroing
     * happens BEFORE our write — so our data overwrites the zeros safely.
     * Future probe() calls for those offsets then short-circuit (already
     * committed) without zeroing our data. */
    if (!ctx || !ctx->module)
        return nullptr;
    return ctx->module->getLinearMemoryRegion(offset, size);
}

static inline uint8_t* warp_linear_mem_window(WarpCallContext* ctx, uint32_t offset,
                                              uint32_t size) {
    if (!ctx || !ctx->module) {
        return nullptr;
    }
    uint64_t mem_size = (uint64_t)ctx->module->getLinearMemorySizeInPages() << 16;
    if ((uint64_t)offset + (uint64_t)size > mem_size) {
        return nullptr;
    }
    uint8_t* base = ctx->module->getLinearMemoryRegion(0, 0);
    if (!base) {
        return nullptr;
    }
    return base + offset;
}

static inline WarpCallContext* warp_call_ctx(void* ctx_) {
    WarpCallContext* cur = ctx_find(process_current_pid());
    if (cur && cur->module) {
        return cur;
    }
    auto* ctx = static_cast<WarpCallContext*>(ctx_);
    if (!ctx) {
        return nullptr;
    }
    if (ctx == ctx_find(ctx->pid)) {
        return ctx;
    }
    auto* module = static_cast<vb::WasmModule*>(ctx_);
    return static_cast<WarpCallContext*>(module->getContext());
}

static int warp_require_io_capability(uint32_t context_id, uint16_t port);

// ---------------------------------------------------------------------------
// Per-PID IPC last-message slots (mirrors wasm3/link.c state)
// ---------------------------------------------------------------------------

struct WarpIpcLastSlot {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
};

/* Per-pid IPC last-message slots, keyed by pid in a growable hashmap (no fixed
 * process-count bound).  Created on first use for a pid and removed on exit via
 * warp_release_pid.  Value addresses are stable for a key's lifetime. */
static hashmap_t g_ipc_last_map;

struct WarpFsPeerSlot {
    uint32_t pid;
    uint8_t valid;
    uint32_t peer_context_id;
};

static hashmap_t g_fs_peer_map;

static void warp_block_slots_init(void);

static void warp_ipc_slots_init(void) {
    hashmap_init(&g_ipc_last_map, sizeof(WarpIpcLastSlot), 64);
    hashmap_init(&g_fs_peer_map, sizeof(WarpFsPeerSlot), 64);
    warp_block_slots_init();
}

static WarpIpcLastSlot* warp_ipc_slot_for_pid(uint32_t pid) {
    if (!pid)
        return nullptr;
    auto* slot = static_cast<WarpIpcLastSlot*>(hashmap_put(&g_ipc_last_map, pid));
    if (slot)
        slot->pid = pid;
    return slot;
}

static WarpFsPeerSlot* warp_fs_peer_slot_for_pid(uint32_t pid) {
    if (!pid) {
        return nullptr;
    }
    auto* slot = static_cast<WarpFsPeerSlot*>(hashmap_put(&g_fs_peer_map, pid));
    if (slot)
        slot->pid = pid;
    return slot;
}

// ---------------------------------------------------------------------------
// Internal helper — mirrors wasm3/link.c's static warp_current_context_id()
// ---------------------------------------------------------------------------

static int warp_current_context_id(uint32_t* out) {
    uint32_t pid = process_current_pid();
    process_t* proc = process_get(pid);
    if (!proc || !out)
        return -1;
    *out = proc->context_id;
    return 0;
}

static int warp_process_name_eq(const char* a, const char* b) {
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

static uint8_t warp_dbg_ipc_trace_process(process_t* proc) {
    (void)proc;
    return 0;
}

// ---------------------------------------------------------------------------
// Host call wrappers — IPC
// ---------------------------------------------------------------------------

static uint32_t warp_ipc_create_endpoint(void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0, endpoint = IPC_ENDPOINT_NONE;
    (void)ctx;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    if (ipc_endpoint_create(context_id, &endpoint) != IPC_OK)
        return (uint32_t)-1;
    return endpoint;
}

static uint32_t warp_ipc_endpoint_owner(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t owner = 0;
    if (ipc_endpoint_owner(endpoint, &owner) != IPC_OK || !owner)
        return (uint32_t)-1;
    return owner;
}

static uint32_t warp_ipc_send(uint32_t dest, uint32_t src, uint32_t type, uint32_t req_id,
                              uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    ipc_message_t msg;
    msg.type = type;
    msg.source = src;
    msg.destination = dest;
    msg.request_id = req_id;
    msg.arg0 = a0;
    msg.arg1 = a1;
    msg.arg2 = a2;
    msg.arg3 = a3;
    return (uint32_t)ipc_send_from(context_id, dest, &msg);
}

static uint32_t warp_ipc_select_one(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    process_t* process = nullptr;

    if ((int32_t)endpoint < 0 || warp_current_context_id(&context_id) != 0) {
        return (uint32_t)-1;
    }
    WarpIpcLastSlot* slot = warp_ipc_slot_for_pid(pid);
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
        WarpFsPeerSlot* peer = warp_fs_peer_slot_for_pid(pid);
        if (peer && slot->message.type >= FS_IPC_OPEN_REQ &&
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

static uint32_t warp_ipc_drain(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    if ((int32_t)endpoint < 0 || warp_current_context_id(&context_id) != 0) {
        return (uint32_t)-1;
    }
    WarpIpcLastSlot* slot = warp_ipc_slot_for_pid(pid);
    if (!slot)
        return (uint32_t)-1;
    int rc = ipc_recv_for(context_id, endpoint, &slot->message);
    if (rc == IPC_EMPTY)
        return 0;
    if (rc != IPC_OK)
        return (uint32_t)-1;
    slot->valid = 1;
    WarpFsPeerSlot* peer = warp_fs_peer_slot_for_pid(pid);
    if (peer && slot->message.type >= FS_IPC_OPEN_REQ &&
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

static uint32_t warp_ipc_notify(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    return (uint32_t)ipc_notify_from(context_id, endpoint);
}

static uint32_t warp_ipc_last_field(uint32_t field, void* ctx_) {
    (void)ctx_;
    uint32_t pid = process_current_pid();
    WarpIpcLastSlot* slot = warp_ipc_slot_for_pid(pid);
    process_t* proc = process_get(pid);
    uint32_t value = 0;
    if (!slot || !slot->valid)
        return (uint32_t)-1;
    switch (field) {
    case 0:
        value = slot->message.type;
        break;
    case 1:
        value = slot->message.request_id;
        break;
    case 2:
        value = slot->message.arg0;
        break;
    case 3:
        value = slot->message.arg1;
        break;
    case 4:
        value = slot->message.source;
        break;
    case 5:
        value = slot->message.destination;
        break;
    case 6:
        value = slot->message.arg2;
        break;
    case 7:
        value = slot->message.arg3;
        break;
    default:
        return (uint32_t)-1;
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

static uint32_t warp_console_read(uint32_t buf_offset, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)len <= 0)
        return (uint32_t)-1;
    uint8_t* buf = warp_mem(ctx, buf_offset, 1);
    if (!buf)
        return (uint32_t)-1;
    uint8_t ch = 0;
    int rc = serial_read_char(&ch);
    if (rc <= 0)
        return (uint32_t)rc;
    *buf = ch;
    return 1;
}

static uint32_t warp_console_write(uint32_t buf_offset, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)len <= 0)
        return (uint32_t)-1;
    uint8_t* buf = warp_mem(ctx, buf_offset, len);
    if (!buf)
        return (uint32_t)-1;
    /* write in 127-byte chunks so klog_write always gets a null-terminated
     * string (we temporarily null-terminate at chunk boundaries). */
    preempt_disable();
    uint32_t written = 0;
    char tmp[128];
    while (written < len) {
        uint32_t chunk = len - written;
        if (chunk > 127)
            chunk = 127;
        __builtin_memcpy(tmp, buf + written, chunk);
        tmp[chunk] = '\0';
        klog_write(tmp);
        written += chunk;
    }
    preempt_enable();
    return 0;
}

// ---------------------------------------------------------------------------
// Host call wrappers — process / scheduler
// ---------------------------------------------------------------------------

static uint32_t warp_proc_exit(uint32_t code, void* ctx_) {
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
    if (proc) {
        process_set_exit_status(proc, static_cast<int32_t>(code));
        process_yield(PROCESS_RUN_EXITED);
    }
    return 0;
}

static void warp_wasi_proc_exit(uint32_t code, void* ctx_) {
    (void)warp_proc_exit(code, ctx_);
}

static uint32_t warp_wasi_random_get(uint32_t buf_offset, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (len == 0) {
        return 0;
    }
    uint8_t* buf = warp_mem(ctx, buf_offset, len);
    if (!buf) {
        return (uint32_t)-1;
    }
    /* Minimal WASI compatibility for guest runtimes that probe randomness
     * during startup. Deterministic zero-fill is sufficient for current WASMOS
     * guests, which only require the call not to trap. */
    __builtin_memset(buf, 0, len);
    return 0;
}

static uint32_t warp_proc_notify_ready(void* ctx_) {
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
    if (proc)
        process_notify_ready(proc);
    return 0;
}

static uint32_t warp_sched_yield(void* ctx_) {
    (void)ctx_;
    process_yield(PROCESS_RUN_YIELDED);
    return 0;
}

static uint32_t warp_sched_current_pid(void* ctx_) {
    (void)ctx_;
    return (uint32_t)process_current_pid();
}

static uint32_t warp_thread_gettid(void* ctx_) {
    (void)ctx_;
    return (uint32_t)thread_current_tid();
}

// ---------------------------------------------------------------------------
// Host call wrappers — futex
// ---------------------------------------------------------------------------

static uint32_t warp_futex_wait(uint32_t addr_off, uint32_t val, uint32_t timeout_ms, void* ctx_) {
    /* futex_wait takes the raw WASM linear-memory offset, not a host pointer. */
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    (void)ctx;
    return (uint32_t)futex_wait(addr_off, val, timeout_ms, context_id);
}

static uint32_t warp_futex_wake(uint32_t addr_off, uint32_t count, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    (void)ctx;
    return (uint32_t)futex_wake(addr_off, count, context_id);
}

// ---------------------------------------------------------------------------
// IPC select set operations
// ---------------------------------------------------------------------------

static uint32_t warp_ipc_select_create(void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0, sel_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    if (ipc_select_create(context_id, &sel_id) != IPC_OK)
        return (uint32_t)-1;
    return sel_id;
}

static uint32_t warp_ipc_select_add(uint32_t sel_id, uint32_t ep_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    return (uint32_t)(ipc_select_add(sel_id, ep_id, context_id) == IPC_OK ? 0 : -1);
}

static uint32_t warp_ipc_select_wait(uint32_t sel_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    uint32_t ready = IPC_ENDPOINT_NONE;
    for (;;) {
        int rc = ipc_select_wait(sel_id, context_id, &ready, 0);
        if (rc == IPC_OK)
            return ready;
        if (rc != IPC_EMPTY)
            return (uint32_t)-1;
    }
}

/* Timed select wait: block until a watched endpoint is ready OR timeout_ms
 * elapses. Returns the ready endpoint id (>= 0), -1 on timeout/spurious wake
 * (caller should poll and retry), or -2 on error. Unlike warp_ipc_select_wait
 * this does NOT loop on IPC_EMPTY, so a deadline reliably returns control. */
static uint32_t warp_ipc_select_wait_timeout(uint32_t sel_id, uint32_t timeout_ms, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-2;
    uint32_t ready = IPC_ENDPOINT_NONE;
    int rc = ipc_select_wait(sel_id, context_id, &ready, timeout_ms);
    if (rc == IPC_OK)
        return ready;
    if (rc == IPC_EMPTY)
        return (uint32_t)-1; /* timeout or spurious wake */
    return (uint32_t)-2;
}

static uint32_t warp_ipc_select_destroy(uint32_t sel_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    ipc_select_destroy(sel_id, context_id);
    return 0;
}

// ---------------------------------------------------------------------------
// FS shared buffer
// ---------------------------------------------------------------------------

static uint32_t warp_xfer_buffer_size(void* ctx_) {
    (void)ctx_;
    return (uint32_t)xfer_buffer_size(BUFFER_KIND_TRANSFER);
}

static uint32_t warp_fs_endpoint(void* ctx_) {
    (void)ctx_;
    uint32_t ep = process_manager_fs_endpoint();
    return (ep == IPC_ENDPOINT_NONE) ? (uint32_t)-1 : ep;
}

static uint32_t warp_xfer_buffer_read(uint32_t buffer_id, uint32_t ptr_off, uint32_t len,
                                      uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!len)
        return 0;
    uint32_t context_id = 0;
    if ((int32_t)buffer_id <= 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    /* Look up the object the caller named; describe confirms owner-or-borrower
     * access, can_access confirms the READ right for this operation. */
    xfer_buffer_t desc = {};
    int rc = xfer_buffer_describe(buffer_id, BUFFER_KIND_TRANSFER, context_id, &desc);
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    if (!xfer_buffer_can_access(&desc, context_id, BUFFER_BORROW_READ))
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    if (offset + len > desc.size_bytes)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_RANGE;
    uint8_t* wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_RANGE;
    uint64_t phys = xfer_buffer_object_phys(&desc);
    if (!phys)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    const uint8_t* src =
        reinterpret_cast<const uint8_t*>(uintptr_t(phys | KERNEL_HIGHER_HALF_BASE));
    __builtin_memcpy(wasm_ptr, src + offset, len);
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_sync_user_range(ctx, ptr_off, len) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_RANGE;
#endif
    return (uint32_t)WASMOS_ERR_NONE;
}

static uint32_t warp_xfer_buffer_write(uint32_t buffer_id, uint32_t ptr_off, uint32_t len,
                                       uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!len)
        return 0;
    uint32_t context_id = 0;
    if ((int32_t)buffer_id <= 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    xfer_buffer_t desc = {};
    int rc = xfer_buffer_describe(buffer_id, BUFFER_KIND_TRANSFER, context_id, &desc);
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    if (!xfer_buffer_can_access(&desc, context_id, BUFFER_BORROW_WRITE))
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    if (offset + len > desc.size_bytes)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_RANGE;
    uint8_t* wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_RANGE;
    uint64_t phys = xfer_buffer_object_phys(&desc);
    if (!phys)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    uint8_t* dst = reinterpret_cast<uint8_t*>(uintptr_t(phys | KERNEL_HIGHER_HALF_BASE));
    __builtin_memcpy(dst + offset, wasm_ptr, len);
    return (uint32_t)WASMOS_ERR_NONE;
}

// ---------------------------------------------------------------------------
// Generic shared buffer acquire/borrow/release/unborrow (stateless id ABI,
// mirroring wasm3 link.c)
// ---------------------------------------------------------------------------

/* Whether the calling context may own/lend transfer buffers. Owning a transfer
 * buffer is like opening a file descriptor: any real process may acquire, borrow
 * and release one. DMA is gated separately at dma_map_borrow
 * (warp_require_dma_capability); mirrors wasm3's wasm_buffer_role_allowed. */
static int warp_buffer_role_allowed(uint32_t context_id, process_t* proc) {
    (void)context_id;
    return proc != nullptr;
}

static uint32_t warp_buffer_acquire(uint32_t kind, uint32_t minimum_size, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    process_t* proc = process_get(process_current_pid());
    if (kind != (uint32_t)BUFFER_KIND_TRANSFER)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    if ((int32_t)minimum_size <= 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_SIZE;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    if (!warp_buffer_role_allowed(context_id, proc))
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    xfer_buffer_owner_t owner;
    int rc = xfer_buffer_acquire(kind, context_id, minimum_size, &owner);
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    return owner.buffer.buffer_id;
}

/* Owner-driven grant: the caller must own buffer_id and names the grantee by an
 * endpoint it owns. Assigns the grantee `flags` rights and returns the borrow_id
 * (the grantee's handle). Mirrors wasm3's wasm_buffer_borrow_impl. */
static uint32_t warp_buffer_borrow(uint32_t kind, uint32_t grantee_ep, uint32_t buffer_id,
                                   uint32_t flags, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0, grantee_context = 0;
    process_t* proc = process_get(process_current_pid());
    if (kind != (uint32_t)BUFFER_KIND_TRANSFER)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    if ((int32_t)buffer_id <= 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    if (flags == 0 || (flags & ~0x3u) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_FLAGS;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    if (!warp_buffer_role_allowed(context_id, proc))
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    if (ipc_endpoint_owner(grantee_ep, &grantee_context) != IPC_OK || grantee_context == 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    xfer_buffer_t key = {kind, buffer_id, 0u};
    xfer_buffer_owner_t owner;
    int rc = xfer_buffer_get_owned(&key, context_id, &owner); /* caller must be owner */
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    xfer_buffer_borrow_t out;
    rc = xfer_buffer_borrow(&owner, grantee_context, flags, &out);
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    return out.borrow_id;
}

/* reborrow: a current borrower extends a rights-narrowed sub-grant of its own
 * borrow (borrow_id) to the context that owns grantee_ep. Mirrors wasm3's
 * wasm_buffer_reborrow_impl. */
static uint32_t warp_buffer_reborrow(uint32_t kind, uint32_t grantee_ep, uint32_t borrow_id,
                                     uint32_t flags, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0, grantee_context = 0;
    if (kind != (uint32_t)BUFFER_KIND_TRANSFER)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    if ((int32_t)borrow_id <= 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    if (flags == 0 || (flags & ~0x3u) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_FLAGS;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    if (ipc_endpoint_owner(grantee_ep, &grantee_context) != IPC_OK || grantee_context == 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    xfer_buffer_borrow_t upstream;
    int rc =
        xfer_buffer_get_borrowed(borrow_id, context_id, &upstream, 0); /* caller must be borrower */
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    xfer_buffer_borrow_t out;
    rc = xfer_buffer_reborrow(&upstream, grantee_context, flags, &out);
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    return out.borrow_id;
}

static uint32_t warp_buffer_release(uint32_t kind, uint32_t buffer_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    process_t* proc = process_get(process_current_pid());
    if (kind != (uint32_t)BUFFER_KIND_TRANSFER)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    if ((int32_t)buffer_id <= 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    if (!warp_buffer_role_allowed(context_id, proc))
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    xfer_buffer_t key = {kind, buffer_id, 0u};
    xfer_buffer_owner_t owner;
    int rc = xfer_buffer_get_owned(&key, context_id, &owner);
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    return (uint32_t)xfer_buffer_release_owned(&owner);
}

/* unborrow: the GRANTOR (lender) of a (re)borrow drops it (owner-push) — resolved
 * via get_lent, not get_borrowed. Mirrors wasm3's wasm_buffer_unborrow_impl. */
static uint32_t warp_buffer_unborrow(uint32_t borrow_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)borrow_id <= 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    xfer_buffer_borrow_t borrow;
    int rc = xfer_buffer_get_lent(borrow_id, context_id, &borrow);
    if (rc != WASMOS_ERR_NONE)
        return (uint32_t)rc;
    return (uint32_t)xfer_buffer_unborrow(&borrow);
}

// ---------------------------------------------------------------------------
// Block DMA buffer
// ---------------------------------------------------------------------------

#define WARP_BLOCK_BUF_PAGES 2u /* 8 KB block buffer per process */

struct WarpBlockSlot {
    uint32_t pid;
    uint64_t phys;
    uint32_t map_off;
};

/* Per-pid block DMA slots, keyed by pid in a growable hashmap (no fixed
 * process-count bound).  Created on first use and removed on exit via
 * warp_release_pid. */
static hashmap_t g_block_map;

static void warp_block_slots_init(void) {
    hashmap_init(&g_block_map, sizeof(WarpBlockSlot), 64);
}

/* Find or allocate a block slot for this PID (used by block_buffer_phys). */
static WarpBlockSlot* warp_block_slot(uint32_t pid) {
    if (!pid)
        return nullptr;
    auto* s = static_cast<WarpBlockSlot*>(hashmap_put(&g_block_map, pid));
    if (s)
        s->pid = pid;
    return s;
}
/* Find a block slot by physical address (used by block_buffer_write/copy,
 * which may be called by a different process than the one that called phys). */
static WarpBlockSlot* warp_block_slot_by_phys(uint64_t phys) {
    hashmap_iter_t it;
    uint32_t key = 0;
    for (auto* s = static_cast<WarpBlockSlot*>(hashmap_first(&g_block_map, &it, &key)); s;
         s = static_cast<WarpBlockSlot*>(hashmap_next(&it, &key))) {
        if (s->phys == phys)
            return s;
    }
    return nullptr;
}

static uint32_t warp_block_buffer_phys(void* ctx_) {
    (void)ctx_;
    uint32_t pid = process_current_pid();
    auto* slot = warp_block_slot(pid);
    if (!slot)
        return (uint32_t)-1;
    if (!slot->phys) {
        /* Must be < 512MB: that's the kernel's higher-half identity mapping
         * window AND within ATA's 32-bit DMA address range. */
        slot->phys = pfa_alloc_pages_below(WARP_BLOCK_BUF_PAGES, 512ULL * 1024 * 1024);
        if (!slot->phys)
            return (uint32_t)-1;
    }
    return (uint32_t)slot->phys;
}

static uint32_t warp_block_buffer_copy(uint32_t phys, uint32_t ptr_off, uint32_t len,
                                       uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    auto* slot = warp_block_slot_by_phys((uint64_t)phys);
    if (!slot)
        return (uint32_t)-1;
    uint32_t buf_bytes = WARP_BLOCK_BUF_PAGES * 4096;
    if (offset + len > buf_bytes)
        return (uint32_t)-1;
    uint8_t* wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr)
        return (uint32_t)-1;
    uint8_t* buf = reinterpret_cast<uint8_t*>(slot->phys | 0xFFFFFFFF80000000ULL);
    __builtin_memcpy(wasm_ptr, buf + offset, len);
    return 0;
}

static uint32_t warp_block_buffer_write(uint32_t phys, uint32_t ptr_off, uint32_t len,
                                        uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    auto* slot = warp_block_slot_by_phys((uint64_t)phys);
    if (!slot)
        return (uint32_t)-1;
    uint32_t buf_bytes = WARP_BLOCK_BUF_PAGES * 4096;
    if (offset + len > buf_bytes)
        return (uint32_t)-1;
    uint8_t* wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr)
        return (uint32_t)-1;
    uint8_t* buf = reinterpret_cast<uint8_t*>(slot->phys | 0xFFFFFFFF80000000ULL);
    __builtin_memcpy(buf + offset, wasm_ptr, len);
    return 0;
}

// ---------------------------------------------------------------------------
// I/O port access
// ---------------------------------------------------------------------------

static uint32_t warp_io_in8(uint32_t port, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (port > 0xFFFF || warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)-1;
    }
    return (uint32_t)inb((uint16_t)port);
}
static uint32_t warp_io_in16(uint32_t port, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (port > 0xFFFF || warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)-1;
    }
    return (uint32_t)inw((uint16_t)port);
}
static uint32_t warp_io_in32(uint32_t port, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (port > 0xFFFF || warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)-1;
    }
    return (uint32_t)inl((uint16_t)port);
}
static uint32_t warp_io_out8(uint32_t port, uint32_t val, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (port > 0xFFFF || warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)-1;
    }
    outb((uint16_t)port, (uint8_t)val);
    return 0;
}
static uint32_t warp_io_out16(uint32_t port, uint32_t val, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (port > 0xFFFF || warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)-1;
    }
    outw((uint16_t)port, (uint16_t)val);
    return 0;
}
static uint32_t warp_io_out32(uint32_t port, uint32_t val, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (port > 0xFFFF || warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)-1;
    }
    outl((uint16_t)port, (uint32_t)val);
    return 0;
}
static uint32_t warp_io_wait(void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, 0x80u) != 0) {
        return (uint32_t)-1;
    }
    io_wait();
    return 0;
}

// ---------------------------------------------------------------------------
// ACPI / boot info
// ---------------------------------------------------------------------------

static const boot_info_t* g_warp_boot_info = nullptr;

static uint32_t warp_acpi_rsdp_info(uint32_t out_off, uint32_t out_len_off, uint32_t max_len,
                                    void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!g_warp_boot_info || !g_warp_boot_info->rsdp || !g_warp_boot_info->rsdp_length)
        return (uint32_t)-1;
    uint32_t len = g_warp_boot_info->rsdp_length;
    if (len > max_len)
        return (uint32_t)-1;
    /* warp_mem uses getLinearMemoryRegion(offset, size) which triggers probe()
     * → ensureLinearSize() BEFORE we write — so zeroing happens first. */
    uint8_t* out = warp_mem(ctx, out_off, len);
    uint32_t* out_len = reinterpret_cast<uint32_t*>(warp_mem(ctx, out_len_off, sizeof(uint32_t)));
    if (!out || !out_len)
        return (uint32_t)-1;
    __builtin_memcpy(out, g_warp_boot_info->rsdp, len);
    *out_len = len;
    return 0;
}

static uint32_t warp_boot_module_name(uint32_t index, uint32_t out_off, uint32_t out_len,
                                      void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!g_warp_boot_info)
        return (uint32_t)-1;
    if (index >= g_warp_boot_info->module_count)
        return (uint32_t)-1;
    const boot_module_t* mod = static_cast<const boot_module_t*>(g_warp_boot_info->modules) + index;
    uint32_t name_len = (uint32_t)__builtin_strlen(mod->name);
    if (name_len >= out_len)
        name_len = out_len - 1;
    uint8_t* out = warp_mem(ctx, out_off, out_len);
    if (!out)
        return (uint32_t)-1;
    __builtin_memcpy(out, mod->name, name_len);
    out[name_len] = '\0';
    return (uint32_t)name_len;
}

static uint32_t warp_sync_user_read(uint32_t ptr_off, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!len)
        return 0;
    uint8_t* p = warp_mem(ctx, ptr_off, len);
    if (!p)
        return (uint32_t)-1;
    /* In kernel context WASM linear memory IS kernel memory; a volatile read
     * ensures the compiler does not elide the access. */
    volatile uint8_t dummy = 0;
    for (uint32_t i = 0; i < len; ++i)
        dummy = p[i];
    (void)dummy;
    return 0;
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

static uint32_t warp_system_halt(void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_system_control_capability(context_id) != 0) {
        return (uint32_t)-1;
    }
    kernel_system_poweroff();
}
static uint32_t warp_system_reboot(void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_system_control_capability(context_id) != 0) {
        return (uint32_t)-1;
    }
    kernel_system_reboot();
}

// ---------------------------------------------------------------------------
// Scheduler extras
// ---------------------------------------------------------------------------

static uint32_t warp_sched_ticks(void* ctx_) {
    (void)ctx_;
    return (uint32_t)timer_ticks();
}
static uint32_t warp_proc_count(void* ctx_) {
    (void)ctx_;
    return (uint32_t)process_count_active();
}

// ---------------------------------------------------------------------------
// initfs access (mirrors initfs_header_get / initfs_entry_at in wasm3/link.c)
// ---------------------------------------------------------------------------

static int warp_initfs_header_get(const wasmos_initfs_header_t** out_hdr,
                                  const uint8_t** out_base) {
    if (!g_warp_boot_info || !(g_warp_boot_info->flags & BOOT_INFO_FLAG_INITFS_PRESENT) ||
        !g_warp_boot_info->initfs || g_warp_boot_info->initfs_size < sizeof(wasmos_initfs_header_t))
        return -1;
    const uint8_t* base = static_cast<const uint8_t*>(g_warp_boot_info->initfs);
    const wasmos_initfs_header_t* hdr = reinterpret_cast<const wasmos_initfs_header_t*>(base);
    if (__builtin_memcmp(hdr->magic, WASMOS_INITFS_MAGIC, sizeof(hdr->magic)) != 0 ||
        hdr->version != WASMOS_INITFS_VERSION ||
        hdr->header_size < sizeof(wasmos_initfs_header_t) ||
        hdr->entry_size != sizeof(wasmos_initfs_entry_t))
        return -1;
    if ((uint64_t)hdr->header_size + (uint64_t)hdr->entry_count * hdr->entry_size >
        (uint64_t)g_warp_boot_info->initfs_size)
        return -1;
    *out_hdr = hdr;
    *out_base = base;
    return 0;
}

static int warp_initfs_entry_at(uint32_t index, wasmos_initfs_entry_t* out) {
    const wasmos_initfs_header_t* hdr = nullptr;
    const uint8_t* base = nullptr;
    if (!out || warp_initfs_header_get(&hdr, &base) != 0 || index >= hdr->entry_count)
        return -1;
    const wasmos_initfs_entry_t* e = reinterpret_cast<const wasmos_initfs_entry_t*>(
        base + hdr->header_size + (uint64_t)index * hdr->entry_size);
    if ((uint64_t)e->offset + e->size > (uint64_t)g_warp_boot_info->initfs_size)
        return -1;
    *out = *e;
    return 0;
}

static uint32_t warp_initfs_entry_count(void* ctx_) {
    (void)ctx_;
    const wasmos_initfs_header_t* hdr = nullptr;
    const uint8_t* base = nullptr;
    if (warp_initfs_header_get(&hdr, &base) != 0)
        return (uint32_t)-1;
    return (uint32_t)hdr->entry_count;
}

static uint32_t warp_initfs_entry_name(uint32_t index, uint32_t out_off, uint32_t out_len,
                                       void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    wasmos_initfs_entry_t e;
    if (warp_initfs_entry_at(index, &e) != 0)
        return (uint32_t)-1;
    uint32_t nlen = 0;
    while (nlen < (uint32_t)sizeof(e.path) && e.path[nlen])
        ++nlen;
    if (nlen >= out_len)
        nlen = out_len - 1;
    uint8_t* out = warp_mem(ctx, out_off, out_len);
    if (!out)
        return (uint32_t)-1;
    __builtin_memcpy(out, e.path, nlen);
    out[nlen] = '\0';
    return nlen;
}

static uint32_t warp_initfs_entry_size(uint32_t index, void* ctx_) {
    (void)ctx_;
    wasmos_initfs_entry_t e;
    return (warp_initfs_entry_at(index, &e) == 0) ? (uint32_t)e.size : (uint32_t)-1;
}

static uint32_t warp_initfs_entry_copy(uint32_t index, uint32_t out_off, uint32_t len,
                                       uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)index < 0 || (int32_t)len <= 0 || (int32_t)offset < 0)
        return (uint32_t)-1;
    wasmos_initfs_entry_t e;
    if (warp_initfs_entry_at(index, &e) != 0)
        return (uint32_t)-1;
    /* Match wasm3: at/after EOF returns 0, and a trailing chunk longer than the
     * remainder is clamped and the short count returned (not rejected). */
    if (offset >= e.size)
        return 0;
    uint32_t copy_len = len;
    uint32_t available = e.size - offset;
    if (copy_len > available)
        copy_len = available;
    uint8_t* out = warp_mem(ctx, out_off, copy_len);
    if (!out)
        return (uint32_t)-1;
    const uint8_t* src = static_cast<const uint8_t*>(g_warp_boot_info->initfs) + e.offset + offset;
    __builtin_memcpy(out, src, copy_len);
    return copy_len; /* bytes copied, matches wasm3 which returns (int32_t)copy_len */
}

// ---------------------------------------------------------------------------
// DMA buffer operations
// ---------------------------------------------------------------------------

static uint32_t warp_dma_map_borrow(uint32_t borrow_id, uint32_t offset, uint32_t length,
                                    uint32_t flags, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)borrow_id <= 0 || (int32_t)length <= 0 || flags == 0)
        return (uint32_t)WASMOS_ERR_DMA_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    /* Resolve the caller's borrow handle; dma_map_borrow enforces
     * direction ⊆ borrow rights and the range check. */
    xfer_buffer_borrow_t borrow;
    if (xfer_buffer_get_borrowed(borrow_id, context_id, &borrow, nullptr) != WASMOS_ERR_NONE)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    xfer_buffer_dma_mapping_t mapping;
    if (xfer_buffer_dma_map_borrow(&borrow, offset, length, flags, &mapping) != WASMOS_ERR_NONE)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    if (mapping.device_addr > 0x7FFFFFFFULL) {
        (void)xfer_buffer_dma_unmap(&mapping);
        return (uint32_t)WASMOS_ERR_DMA_UNAVAILABLE;
    }
    return (uint32_t)mapping.device_addr;
}

static uint32_t warp_dma_sync_borrow(uint32_t borrow_id, uint32_t offset, uint32_t length,
                                     uint32_t op, void* ctx_) {
    (void)ctx_;
    (void)op;
    uint32_t context_id = 0;
    if ((int32_t)borrow_id <= 0)
        return (uint32_t)WASMOS_ERR_DMA_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;
    if (xfer_buffer_get_borrowed(borrow_id, context_id, &borrow, &mapping) != WASMOS_ERR_NONE)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    if (xfer_buffer_dma_sync(&mapping, offset, length) != WASMOS_ERR_NONE)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    return (uint32_t)WASMOS_ERR_NONE;
}

static uint32_t warp_dma_unmap_borrow(uint32_t borrow_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)borrow_id <= 0)
        return (uint32_t)WASMOS_ERR_DMA_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;
    if (xfer_buffer_get_borrowed(borrow_id, context_id, &borrow, &mapping) != WASMOS_ERR_NONE)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    if (xfer_buffer_dma_unmap(&mapping) != WASMOS_ERR_NONE)
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    return (uint32_t)WASMOS_ERR_NONE;
}

// ---------------------------------------------------------------------------
// Physical memory mapping into WASM linear memory
// ---------------------------------------------------------------------------

static uint32_t warp_phys_map(uint32_t phys_lo, uint32_t phys_hi, uint32_t size,
                              uint32_t wasm_offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!size || (size & 0xFFF) || (wasm_offset & 0xFFF))
        return (uint32_t)-1;
    uint64_t phys = ((uint64_t)phys_hi << 32) | (uint64_t)phys_lo;
    if (!phys)
        return (uint32_t)-1;
    /* Map physical pages into WASM linear memory.
     * With the page-aligned allocator fix in shim.cpp, the linear memory base
     * is 4 KB-aligned, so base + wasm_offset is page-aligned when wasm_offset
     * is a multiple of 4096. */
    uint8_t* lmem = warp_linear_mem_window(ctx, wasm_offset, size);
    if (!lmem)
        return (uint32_t)-1;
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
    if (!lmem)
        return (uint32_t)-1;
    lmem += wasm_offset;

    uint64_t scratch_va = addr_cast(uint64_t, g_phys_scratch);
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
#define WARP_KENV_KEY_MAX 33
#define WARP_KENV_VAL_MAX 129

struct WarpKenvEntry {
    uint8_t in_use;
    char key[WARP_KENV_KEY_MAX];
    char value[WARP_KENV_VAL_MAX];
};

static WarpKenvEntry g_warp_kenv[WARP_KENV_MAX_ENTRIES];

static int warp_kenv_find(const char* key) {
    for (int i = 0; i < WARP_KENV_MAX_ENTRIES; i++) {
        if (g_warp_kenv[i].in_use && __builtin_strcmp(g_warp_kenv[i].key, key) == 0)
            return i;
    }
    return -1;
}

static uint32_t warp_env_get(uint32_t name_off, uint32_t name_len, uint32_t buf_off,
                             uint32_t buf_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || (int32_t)buf_len <= 0)
        return (uint32_t)-1;
    if (name_len >= WARP_KENV_KEY_MAX)
        return (uint32_t)-1;
    const uint8_t* name = warp_mem(ctx, name_off, name_len);
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    if (!name || !buf)
        return (uint32_t)-1;
    char local_name[WARP_KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    int idx = warp_kenv_find(local_name);
    if (idx < 0)
        return (uint32_t)-1;
    uint32_t val_len = 0;
    while (g_warp_kenv[idx].value[val_len])
        val_len++;
    uint32_t write_len = val_len < buf_len - 1u ? val_len : buf_len - 1u;
    __builtin_memcpy(buf, g_warp_kenv[idx].value, write_len);
    buf[write_len] = '\0';
    return write_len;
}

static uint32_t warp_env_set(uint32_t name_off, uint32_t name_len, uint32_t val_off,
                             uint32_t val_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || name_len >= WARP_KENV_KEY_MAX)
        return (uint32_t)-1;
    if ((int32_t)val_len < 0 || val_len >= WARP_KENV_VAL_MAX)
        return (uint32_t)-1;
    const uint8_t* name = warp_mem(ctx, name_off, name_len);
    if (!name)
        return (uint32_t)-1;
    char local_name[WARP_KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    char local_val[WARP_KENV_VAL_MAX];
    local_val[0] = '\0';
    if (val_len > 0) {
        const uint8_t* val = warp_mem(ctx, val_off, val_len);
        if (!val)
            return (uint32_t)-1;
        __builtin_memcpy(local_val, val, val_len);
        local_val[val_len] = '\0';
    }
    int idx = warp_kenv_find(local_name);
    if (idx < 0) {
        for (int i = 0; i < WARP_KENV_MAX_ENTRIES; i++) {
            if (!g_warp_kenv[i].in_use) {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            return (uint32_t)-1;
        g_warp_kenv[idx].in_use = 1;
        __builtin_memcpy(g_warp_kenv[idx].key, local_name, name_len + 1u);
    }
    __builtin_memcpy(g_warp_kenv[idx].value, local_val, val_len + 1u);
    return 0;
}

static uint32_t warp_env_unset(uint32_t name_off, uint32_t name_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || name_len >= WARP_KENV_KEY_MAX)
        return 0;
    const uint8_t* name = warp_mem(ctx, name_off, name_len);
    if (!name)
        return 0;
    char local_name[WARP_KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    int idx = warp_kenv_find(local_name);
    if (idx >= 0)
        g_warp_kenv[idx].in_use = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

static int warp_require_dma_capability(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_DMA_BUFFER, 0);
}
static int warp_require_io_capability(uint32_t context_id, uint16_t port) {
    return policy_authorize(context_id, POLICY_ACTION_IO_PORT, port);
}
static int warp_require_mmio_capability(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_MMIO_MAP, 0);
}
static int warp_require_irq_capability(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_IRQ_CONTROL, 0);
}
static int warp_require_system_control_capability(uint32_t context_id) {
    return policy_require(context_id, POLICY_ACTION_SYSTEM_CONTROL, 0);
}

// ---------------------------------------------------------------------------
// Shmem map tracking — mirrors g_wasm_shmem_maps in wasm3/link.c
// ---------------------------------------------------------------------------

#define WARP_SHMEM_MAP_SLOTS (PROCESS_MAX_COUNT * 32)

struct WarpShmemLinearMap {
    uint32_t pid;
    uint32_t shmem_id;
    uint32_t offset;
    uint32_t size;
    uint8_t valid;
};

static WarpShmemLinearMap g_warp_shmem_maps[WARP_SHMEM_MAP_SLOTS];

static void warp_shmem_map_track(uint32_t pid, uint32_t id, uint32_t offset, uint32_t size) {
    WarpShmemLinearMap* empty = nullptr;
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i) {
        WarpShmemLinearMap* s = &g_warp_shmem_maps[i];
        if (s->valid && s->pid == pid && s->shmem_id == id && s->offset == offset) {
            s->size = size;
            return;
        }
        if (!empty && !s->valid)
            empty = s;
    }
    if (empty) {
        empty->pid = pid;
        empty->shmem_id = id;
        empty->offset = offset;
        empty->size = size;
        empty->valid = 1;
    }
}

static void warp_shmem_map_untrack(uint32_t pid, uint32_t id) {
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i)
        if (g_warp_shmem_maps[i].valid && g_warp_shmem_maps[i].pid == pid &&
            g_warp_shmem_maps[i].shmem_id == id)
            g_warp_shmem_maps[i].valid = 0;
}

static WarpShmemLinearMap* warp_shmem_map_find(uint32_t pid, uint32_t id) {
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i) {
        WarpShmemLinearMap* slot = &g_warp_shmem_maps[i];
        if (slot->valid && slot->pid == pid && slot->shmem_id == id) {
            return slot;
        }
    }
    return nullptr;
}

static uint8_t warp_shmem_map_overlaps(uint32_t pid, uint32_t offset, uint32_t size) {
    uint64_t a0 = offset, a1 = (uint64_t)offset + size;
    for (uint32_t i = 0; i < WARP_SHMEM_MAP_SLOTS; ++i) {
        const WarpShmemLinearMap* s = &g_warp_shmem_maps[i];
        if (!s->valid || s->pid != pid || s->size == 0)
            continue;
        uint64_t b0 = s->offset, b1 = b0 + s->size;
        if (a0 < b1 && b0 < a1)
            return 1;
    }
    return 0;
}

static uint32_t warp_linear_memory_active_size(WarpCallContext* ctx) {
    uint8_t* base = warp_mem(ctx, 0, 0);
    if (!base) {
        return 0;
    }
    return *(uint32_t*)(void*)(base - Basedata::FromEnd::actualLinMemByteSize);
}

static int warp_restore_linear_window(WarpCallContext* ctx, uint32_t offset, uint32_t size) {
    if (!ctx || !ctx->module || size == 0) {
        return -1;
    }
    uint8_t* base = warp_linear_mem_window(ctx, offset, size);
    if (!base) {
        return -1;
    }
    uint64_t virt = addr_cast(uint64_t, base);
    if (virt < KERNEL_HIGHER_HALF_BASE || (virt & 0xFFFULL) != 0) {
        return -1;
    }
    uint64_t pages = ((uint64_t)size + 0xFFFULL) / 0x1000ULL;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t page_virt = virt + i * 0x1000ULL;
        uint64_t page_phys = page_virt - KERNEL_HIGHER_HALF_BASE;
        if (paging_map_4k(page_virt, page_phys,
                          MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC) !=
            0) {
            return -1;
        }
    }
    return 0;
}

#ifdef WASMOS_WASM_RUNTIME_WARP
static int warp_ring3_sync_linmem_user_window(uint8_t* linmem_base) {
    if (!linmem_base) {
        return -1;
    }
    uint64_t current_root = paging_get_current_root_table();
    if (current_root == 0 || current_root == paging_get_root_table()) {
        return 0;
    }
    return warp_mem_ring3_map_linmem(current_root, linmem_base);
}

static int warp_ring3_sync_user_range(WarpCallContext* ctx, uint32_t wasm_off, uint32_t size) {
    if (!ctx || !ctx->module || size == 0) {
        return -1;
    }
    uint64_t current_root = paging_get_current_root_table();
    if (current_root == 0 || current_root == paging_get_root_table()) {
        return 0;
    }

    uint8_t* linmem_base = ctx->module->getLinearMemoryRegion(0, 0);
    uint8_t* range_base = warp_linear_mem_window(ctx, wasm_off, size);
    if (!linmem_base || !range_base) {
        return -1;
    }

    uint64_t user_range_base =
        WARP_R3_LINMEM_BASE + (addr_cast(uint64_t, linmem_base) & 0xFFFULL) + wasm_off;
    uint64_t user_page_base = user_range_base & ~0xFFFULL;
    uint64_t kernel_page_base = (addr_cast(uint64_t, range_base)) & ~0xFFFULL;
    uint64_t page_count = ((user_range_base & 0xFFFULL) + (uint64_t)size + 0xFFFULL) >> 12;

    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t phys_page = warp_mem_alias_phys(kernel_page_base + i * 0x1000ULL) & ~0xFFFULL;
        if (!phys_page) {
            return -1;
        }
        if (paging_map_4k_in_root(current_root, user_page_base + i * 0x1000ULL, phys_page,
                                  MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                      MEM_REGION_FLAG_USER) != 0) {
            return -1;
        }
    }
    return 0;
}

static int warp_ring3_map_user_window(uint8_t* linmem_base, uint32_t wasm_off, uint64_t phys_base,
                                      uint64_t pages) {
    if (!linmem_base || pages == 0) {
        return -1;
    }
    uint64_t current_root = paging_get_current_root_table();
    if (current_root == 0 || current_root == paging_get_root_table()) {
        return 0;
    }
    uint64_t user_va =
        WARP_R3_LINMEM_BASE + (addr_cast(uint64_t, linmem_base) & 0xFFFULL) + wasm_off;
    for (uint64_t i = 0; i < pages; ++i) {
        if (paging_map_4k_in_root(current_root, user_va + i * 0x1000ULL, phys_base + i * 0x1000ULL,
                                  MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                      MEM_REGION_FLAG_USER) != 0) {
            return -1;
        }
    }
    return 0;
}
#endif

// ---------------------------------------------------------------------------
// xfer_buffer_acquire / _borrow / _release / _unborrow (transfer-kind
// convenience over the generic id ABI)
// ---------------------------------------------------------------------------

static uint32_t warp_xfer_buffer_acquire(uint32_t minimum_size, void* ctx_) {
    return warp_buffer_acquire((uint32_t)BUFFER_KIND_TRANSFER, minimum_size, ctx_);
}

/* spawn_info_buffer: return the calling process's spawn-info buffer_id (0 if
 * none). Mirrors wasm3's wasm_spawn_info_buffer_impl. */
static uint32_t warp_spawn_info_buffer(void* ctx_) {
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
    if (!proc)
        return 0u;
    return proc->spawn_info_buffer_id;
}

static uint32_t warp_xfer_buffer_borrow(uint32_t grantee_endpoint, uint32_t buffer_id,
                                        uint32_t flags, void* ctx_) {
    return warp_buffer_borrow((uint32_t)BUFFER_KIND_TRANSFER, grantee_endpoint, buffer_id, flags,
                              ctx_);
}

static uint32_t warp_xfer_buffer_reborrow(uint32_t grantee_endpoint, uint32_t borrow_id,
                                          uint32_t flags, void* ctx_) {
    return warp_buffer_reborrow((uint32_t)BUFFER_KIND_TRANSFER, grantee_endpoint, borrow_id, flags,
                                ctx_);
}

static uint32_t warp_xfer_buffer_release(uint32_t buffer_id, void* ctx_) {
    return warp_buffer_release((uint32_t)BUFFER_KIND_TRANSFER, buffer_id, ctx_);
}

static uint32_t warp_xfer_buffer_unborrow(uint32_t borrow_id, void* ctx_) {
    return warp_buffer_unborrow(borrow_id, ctx_);
}

// ---------------------------------------------------------------------------
// sched_cpu_stats
// ---------------------------------------------------------------------------

static uint32_t warp_sched_cpu_stats(uint32_t cpu_id, uint32_t out_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    typedef struct {
        uint32_t ready_count;
        uint32_t running_pid;
        uint32_t steal_count;
        uint32_t dispatch_count;
        uint32_t last_pid;
    } cpu_stats_t;
    if (cpu_id >= g_cpu_count)
        return (uint32_t)-1;
    uint8_t* raw = warp_mem(ctx, out_off, sizeof(cpu_stats_t));
    if (!raw)
        return (uint32_t)-1;
    cpu_sched_t* cs = &g_cpus[cpu_id].sched;
    uint32_t ready = 0;
    for (int p = 0; p < SCHED_PRIO_MAX; p++)
        ready += cs->thread_count[p];
    cpu_stats_t st;
    st.ready_count = ready;
    st.running_pid = g_cpus[cpu_id].current_process ? g_cpus[cpu_id].current_process->pid : 0;
    st.steal_count = g_cpus[cpu_id].steal_count;
    st.dispatch_count = g_cpus[cpu_id].dispatch_count;
    st.last_pid = g_cpus[cpu_id].last_dispatched_pid;
    __builtin_memcpy(raw, &st, sizeof(st));
    return 0;
}

// ---------------------------------------------------------------------------
// proc_info / proc_info_ex / proc_info_stats
// ---------------------------------------------------------------------------

static uint32_t warp_proc_info(uint32_t index, uint32_t buf_off, uint32_t buf_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)buf_len <= 0)
        return (uint32_t)-1;
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    if (!buf)
        return (uint32_t)-1;
    uint32_t pid = 0;
    const char* name = nullptr;
    if (process_info_at(index, &pid, &name) != 0)
        return (uint32_t)-1;
    uint32_t nlen = 0;
    if (name)
        while (name[nlen] && nlen + 1u < buf_len)
            nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

static uint32_t warp_proc_info_ex(uint32_t index, uint32_t buf_off, uint32_t buf_len,
                                  uint32_t parent_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)buf_len <= 0)
        return (uint32_t)-1;
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    uint8_t* par = warp_mem(ctx, parent_off, sizeof(uint32_t));
    if (!buf || !par)
        return (uint32_t)-1;
    uint32_t pid = 0, parent_pid = 0;
    const char* name = nullptr;
    if (process_info_at_ex(index, &pid, &parent_pid, &name) != 0)
        return (uint32_t)-1;
    __builtin_memcpy(par, &parent_pid, sizeof(parent_pid));
    uint32_t nlen = 0;
    if (name)
        while (name[nlen] && nlen + 1u < buf_len)
            nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

static uint32_t warp_proc_info_stats(uint32_t index, uint32_t buf_off, uint32_t buf_len,
                                     uint32_t parent_off, uint32_t stats_off, void* ctx_) {
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
    } wasm_proc_stats_t;
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)buf_len <= 0)
        return (uint32_t)-1;
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    uint8_t* par = warp_mem(ctx, parent_off, sizeof(uint32_t));
    uint8_t* stp = warp_mem(ctx, stats_off, sizeof(wasm_proc_stats_t));
    if (!buf || !par || !stp)
        return (uint32_t)-1;
    uint32_t pid = 0, parent_pid = 0;
    const char* name = nullptr;
    process_stats_t stats;
    if (process_info_at_stats(index, &pid, &parent_pid, &name, &stats) != 0)
        return (uint32_t)-1;
    __builtin_memcpy(par, &parent_pid, sizeof(parent_pid));
    auto* out = reinterpret_cast<wasm_proc_stats_t*>(stp);
    out->state = stats.state;
    out->block_reason = stats.block_reason;
    __builtin_memcpy(out->runtime_tag, stats.runtime_tag, sizeof(out->runtime_tag));
    out->thread_count = stats.thread_count;
    out->live_thread_count = stats.live_thread_count;
    out->current_tid = stats.current_tid;
    out->context_id = stats.context_id;
    out->cpu_ticks = stats.cpu_ticks;
    out->vm_total_bytes = stats.vm_total_bytes;
    out->thread_kstack_total_bytes = stats.thread_kstack_total_bytes;
    out->heap_committed_bytes = stats.heap_committed_bytes;
    out->rss_est_bytes = stats.rss_est_bytes;
    out->last_cpu = stats.last_cpu;
    uint32_t nlen = 0;
    if (name)
        while (name[nlen] && nlen + 1u < buf_len)
            nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

// ---------------------------------------------------------------------------
// Thread operations
// ---------------------------------------------------------------------------

static uint32_t warp_thread_create(uint32_t entry_off, uint32_t arg0, uint32_t arg1, uint32_t flags,
                                   void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)entry_off <= 0)
        return (uint32_t)-1;
    const uint8_t* name_raw = warp_mem(ctx, entry_off, 1);
    if (!name_raw)
        return (uint32_t)-1;
    /* Scan for NUL within 64 bytes */
    uint32_t mem_size = ctx->module->getLinearMemorySizeInPages() << 16;
    if (entry_off >= mem_size)
        return (uint32_t)-1;
    uint32_t avail = mem_size - entry_off;
    if (avail > 64u)
        avail = 64u;
    uint8_t* nm = warp_mem(ctx, entry_off, avail);
    if (!nm)
        return (uint32_t)-1;
    uint8_t ok = 0;
    for (uint32_t i = 0; i < avail; ++i)
        if (nm[i] == '\0') {
            ok = 1;
            break;
        }
    if (!ok)
        return (uint32_t)-1;
    const char* entry_name = reinterpret_cast<const char*>(nm);
    uint32_t argc = (flags & 0x1u) ? 2u : 0u;
    uint32_t argv[2] = {arg0, arg1};
    uint32_t tid = 0;
    if (wasm_driver_spawn_vm_thread(ctx->pid, entry_name, argc, argv, &tid) != 0)
        return (uint32_t)-1;
    return tid;
}

static uint32_t warp_thread_yield(void* ctx_) {
    (void)ctx_;
    process_yield(PROCESS_RUN_YIELDED);
    return 0;
}

static uint32_t warp_thread_exit(uint32_t status, void* ctx_) {
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
    if (!proc)
        return (uint32_t)-1;
    process_set_exit_status(proc, (int32_t)status);
    process_yield(PROCESS_RUN_THREAD_EXITED);
    return 0;
}

static uint32_t warp_thread_join(uint32_t tid, void* ctx_) {
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
    if (!proc)
        return (uint32_t)-1;
    int32_t exit_status = 0;
    int rc = process_thread_join(proc, tid, &exit_status);
    if (rc > 0) {
        process_yield(PROCESS_RUN_BLOCKED);
        return 0;
    }
    if (rc < 0)
        return (uint32_t)-1;
    return (uint32_t)exit_status;
}

static uint32_t warp_thread_detach(uint32_t tid, void* ctx_) {
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
    if (!proc)
        return (uint32_t)-1;
    return (uint32_t)process_thread_detach(proc, tid);
}

// ---------------------------------------------------------------------------
// Shared memory
// ---------------------------------------------------------------------------

static uint32_t warp_shmem_create(uint32_t pages, uint32_t flags, void* ctx_) {
    (void)ctx_;
    if ((int32_t)pages <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)-1;
    uint32_t id = 0;
    uint64_t phys = 0;
    uint32_t cflags = flags ? flags : (MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE);
    if (mm_shared_create(context_id, (uint64_t)pages, cflags, &id, &phys) != 0)
        return (uint32_t)-1;
    (void)phys;
    return id;
}

static uint32_t warp_klog_register_ring(uint32_t id, void* ctx_) {
    (void)ctx_;
    if ((int32_t)id <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    /* Ownership of the shared region is enforced inside klog_register_ring
     * (mm_shared_get_phys), matching the wasm3 path. */
    return (uint32_t)klog_register_ring(context_id, id);
}

static uint32_t warp_shmem_grant(uint32_t id, uint32_t target_pid, void* ctx_) {
    (void)ctx_;
    if ((int32_t)id <= 0 || (int32_t)target_pid <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)-1;
    process_t* tgt = process_get(target_pid);
    if (!tgt || tgt->context_id == 0)
        return (uint32_t)-1;
    return (uint32_t)mm_shared_grant(context_id, id, tgt->context_id);
}

static uint32_t warp_shmem_revoke(uint32_t id, uint32_t target_pid, void* ctx_) {
    (void)ctx_;
    if ((int32_t)id <= 0 || (int32_t)target_pid <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)-1;
    process_t* tgt = process_get(target_pid);
    if (!tgt || tgt->context_id == 0)
        return (uint32_t)-1;
    return (uint32_t)mm_shared_revoke(context_id, id, tgt->context_id);
}

static uint32_t warp_shmem_map(uint32_t id, uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0 || (size & 0xFFF))
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)-1;
    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0 || shared_pages == 0)
        return (uint32_t)-1;
    if ((uint64_t)size < shared_pages * 0x1000ULL)
        return (uint32_t)-1;
    /* Commit the range via probe() BEFORE paging_map_4k (see warp_shmem_map_auto
     * for the rationale — ensureLinearSize would zero the shmem pages otherwise). */
    ctx->module->getLinearMemoryRegion(wasm_off + size - 1, 1);
    uint8_t* linmem_base = ctx->module->getLinearMemoryRegion(0, 0);
    if (!linmem_base)
        return (uint32_t)-1;
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_sync_linmem_user_window(linmem_base) != 0) {
        return (uint32_t)-1;
    }
#endif
    uint8_t* lmem = linmem_base + wasm_off;
    if (addr_cast(uint64_t, lmem) & 0xFFF)
        return (uint32_t)-1;
    uint64_t virt = addr_cast(uint64_t, lmem);
    for (uint64_t i = 0; i < shared_pages; ++i) {
        paging_map_4k(virt + i * 0x1000ULL, phys_base + i * 0x1000ULL, 3ULL);
    }
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_map_user_window(linmem_base, wasm_off, phys_base, shared_pages) != 0) {
        return (uint32_t)-1;
    }
#endif
    if (mm_shared_retain(context_id, id) != 0)
        return (uint32_t)-1;
    warp_shmem_map_track(ctx->pid, id, wasm_off, size);
    return 0;
}

/* Place a page-aligned window of `window_size` bytes somewhere in the calling
 * app's linear memory, grow/commit linmem to cover it, and remap the window's
 * host pages onto [phys_base, phys_base + map_pages*4KiB).  This is the delicate
 * scan + commit-probe + paging_map_4k core shared by warp_shmem_map_auto
 * (peer-granted shared phys) and warp_region_alloc (driver-owned pinned phys) —
 * one code path so the pinned-linmem-base invariants only live in one place.
 * Caller owns tracking/retain and the physical backing.  Returns the wasm
 * offset (>= 0) on success, or a packed WASMOS_ERR_SHMEM_* code (negative) on failure. */
static int64_t warp_linmem_place_phys(WarpCallContext* ctx, uint64_t phys_base, uint64_t map_pages,
                                      uint32_t window_size) {
    /* Scan linear memory for a free, page-aligned, non-overlapping window.
     * Start from the current active/committed linear-memory size instead of a
     * fixed 2 MiB floor so the first map stays inside memory WARP has already
     * extended. That avoids the Could_not_extend_linear_memory fault seen when
     * gfx_smoke/menu_bar perform their first libui buffer map.
     * TODO(warp-shmem-map-auto): reserve windows against actual heap growth
     * instead of relying on a fixed post-active guard band. */
    /* Use the WARP-configured heap limit rather than the WASM binary's
     * declared page count.  Zig freestanding binaries declare only the
     * minimum memory they need statically (often 1 page = 64 KB) regardless
     * of heap_pages in linker.metadata, so getLinearMemorySizeInPages() gives
     * too small an upper bound for the scan.  Fall back to the configured heap
     * ceiling when it is larger. */
    uint32_t mem_pages = ctx->module->getLinearMemorySizeInPages();
    uint64_t cfg_bytes = warp_heap_committed_bytes(ctx->pid);
    if (cfg_bytes > (uint64_t)mem_pages << 16)
        mem_pages = (uint32_t)((cfg_bytes + 0xFFFFULL) >> 16);
    /* Bound the scan by the RESERVED linmem capacity when the block has moved
     * into its dedicated VA slot: the slot commits pages on demand, so windows
     * placed within it are backed as the commit-probe grows the block (no
     * relocation, base pinned).  0 before the move → committed-size bound. */
    uint64_t reserved = warp_linmem_reserved_bytes(ctx->pid);
    if (reserved > (uint64_t)mem_pages << 16)
        mem_pages = (uint32_t)((reserved + 0xFFFFULL) >> 16);
    uint64_t mem_size = (uint64_t)mem_pages << 16;
    uint64_t scan_min = (uint64_t)warp_linear_memory_active_size(ctx) + 0x10000ULL;
    uint8_t* base = ctx->module->getLinearMemoryRegion(0, 0);
    uint64_t base_mod = base ? (addr_cast(uint64_t, base) & 0xFFFULL) : 0;
    if (scan_min < 0x4000ULL) {
        scan_min = 0x4000ULL;
    }
    uint32_t found_off = 0;
    uint8_t found = 0;
    /* WARP's host linear-memory base is not guaranteed to be page-aligned.
     * Choose guest offsets that make the resulting host virtual address
     * page-aligned, because paging_map_4k remaps host pages, not guest
     * offsets. Keep windows away from both the low live data/stack frontier
     * and the top-of-memory tail, where WARP keeps private runtime state
     * outside the app's explicit globals. */
    if (mem_size < (uint64_t)window_size) {
        return (int64_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    }
    const uint64_t low_guard = 0x200000ULL;
    const uint64_t high_guard = 0x20000ULL;
    if (scan_min < low_guard && mem_size > low_guard + (uint64_t)window_size + high_guard) {
        scan_min = low_guard;
    }
    uint64_t scan_limit = mem_size;
    if (scan_limit > high_guard) {
        scan_limit -= high_guard;
    }
    if (scan_limit < (uint64_t)window_size || scan_min + (uint64_t)window_size > scan_limit) {
        return (int64_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    }
    uint64_t start_off = ((scan_min + base_mod + 0xFFFULL) & ~0xFFFULL) - base_mod;
    if (start_off < scan_min) {
        start_off += 0x1000ULL;
    }
    for (uint64_t off = start_off; off + window_size <= scan_limit; off += 0x1000ULL) {
        uint32_t off32 = (uint32_t)off;
        if (!warp_shmem_map_overlaps(ctx->pid, off32, window_size) && base) {
            uint8_t* p = base + off32;
            if ((addr_cast(uint64_t, p) & 0xFFFULL) == 0) {
                found_off = off32;
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        return (int64_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    }
    /* Commit the target range via probe() BEFORE paging_map_4k.
     * ensureLinearSize() zero-initialises newly committed WASM pages.  If the
     * probe fires AFTER paging_map_4k, the memset writes zeros to the remapped
     * physical pages — corrupting the shared/region buffer. */
    ctx->module->getLinearMemoryRegion(found_off + window_size - 1, 1);
    /* Re-fetch lmem after potential syncBasedataStart from ensureLinearSize. */
    uint8_t* linmem_base = ctx->module->getLinearMemoryRegion(0, 0);
    if (!linmem_base)
        return -1;
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_sync_linmem_user_window(linmem_base) != 0) {
        return -1;
    }
#endif
    uint8_t* lmem = linmem_base + found_off;
    if (addr_cast(uint64_t, lmem) & 0xFFF)
        return -1; /* alignment */

    uint64_t virt = addr_cast(uint64_t, lmem);
    for (uint64_t i = 0; i < map_pages; ++i) {
        paging_map_4k(virt + i * 0x1000ULL, phys_base + i * 0x1000ULL, 3ULL);
    }
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_map_user_window(linmem_base, found_off, phys_base, map_pages) != 0) {
        return -1;
    }
#endif
    return (int64_t)found_off;
}

static uint32_t warp_shmem_map_auto(uint32_t id, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0 || (size & 0xFFF)) {
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ARGS;
    }
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0) {
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    }
    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0 || shared_pages == 0) {
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ID;
    }
    if ((uint64_t)size < shared_pages * 0x1000ULL) {
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_SIZE;
    }
#if WASMOS_TRACE
    klog_printf("[trace-shmem] map_auto pid=%u size=%llx shpg=%llx reserved=%llx "
                "linmem_pages=%u committed=%llx\n",
                (unsigned)ctx->pid, (unsigned long long)size, (unsigned long long)shared_pages,
                (unsigned long long)warp_linmem_reserved_bytes(ctx->pid),
                (unsigned)ctx->module->getLinearMemorySizeInPages(),
                (unsigned long long)warp_heap_committed_bytes(ctx->pid));
#endif
    int64_t placed = warp_linmem_place_phys(ctx, phys_base, shared_pages, size);
    if (placed < 0) {
        return (uint32_t)placed;
    }
    uint32_t found_off = (uint32_t)placed;
    if (mm_shared_retain(context_id, id) != 0) {
        return (uint32_t)-1;
    }
    warp_shmem_map_track(ctx->pid, id, found_off, size);
    return found_off;
}

/* Sanity cap on a single region reservation (4 MiB). The real fit is enforced
 * by warp_linmem_place_phys returning NO_WINDOW when the driver's linmem window
 * is too small. */
#define WARP_REGION_MAX_PAGES 1024u
/* Synthetic tracking id for region windows: high bit set keeps it out of the
 * (small, positive) shmem id space so region windows participate in overlap
 * checks without colliding with real shmem maps. */
#define WARP_REGION_TRACK_ID(off) ((uint32_t)(off) | 0x80000000u)

/* Driver-owned pinned DMA region: allocate a contiguous, page-aligned physical
 * run below 2 GiB, remap it into the calling driver's WASM linear memory (a real
 * page remap, so writes land in the exact physical pages the device DMAs), and
 * pin it for the driver's lifetime.  Gated by CAP_DMA_BUFFER and the driver's
 * approved DMA window.  Returns the wasm linmem offset of the mapped region and
 * writes the u64 physical base to *out_phys_off (a wasm linmem address).  See
 * docs/architecture/12-dma-transfers.md "Driver-Owned DMA Regions".
 * Scope: one-shot pinned reservation, no free/reuse (matches doc 12). */
static uint32_t warp_region_alloc(uint32_t pages, uint32_t cache_policy, uint32_t out_phys_off,
                                  void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (pages == 0 || pages > WARP_REGION_MAX_PAGES) {
        return (uint32_t)WASMOS_ERR_DMA_INVALID;
    }
    /* TODO(region-alloc): write-combining PAT attributes for framebuffer/scanout
     * regions; only write-back (coherent DMA rings) is implemented today. */
    if (cache_policy != WASMOS_REGION_CACHE_WB) {
        return (uint32_t)WASMOS_ERR_DMA_INVALID;
    }
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0) {
        return (uint32_t)WASMOS_ERR_DMA_DENY;
    }
    uint64_t region_bytes = (uint64_t)pages * 0x1000ULL;
    /* Out pointer must hold a u64 and sit inside the caller's linmem. */
    uint8_t* out_ptr = warp_mem(ctx, out_phys_off, sizeof(uint64_t));
    if (!out_ptr) {
        return (uint32_t)WASMOS_ERR_DMA_INVALID;
    }
    /* Below 2 GiB so the legacy virtqueue PFN register and the signed-32-bit
     * device_addr contract hold (same clamp as the borrow DMA path). */
    uint64_t phys_base = pfa_alloc_pages_below(pages, 0x80000000ULL);
    if (!phys_base) {
        return (uint32_t)WASMOS_ERR_DMA_UNAVAILABLE;
    }
    /* Enforce the driver's approved DMA window on allocated regions exactly as on
     * borrow mappings — a general "give me physical memory" primitive without
     * this check would be a DMA-anywhere hole. */
    if (capability_dma_range_allowed(context_id, phys_base, region_bytes) == 0) {
        pfa_free_pages(phys_base, pages);
        return (uint32_t)WASMOS_ERR_DMA_RANGE;
    }
    /* Enforce the driver's declared DMA page budget (dma.buffer manifest flags). */
    if (capability_dma_within_budget(context_id, region_bytes) == 0) {
        pfa_free_pages(phys_base, pages);
        return (uint32_t)WASMOS_ERR_DMA_RANGE;
    }
    int64_t placed = warp_linmem_place_phys(ctx, phys_base, pages, (uint32_t)region_bytes);
    if (placed < 0) {
        pfa_free_pages(phys_base, pages);
        return (uint32_t)WASMOS_ERR_DMA_UNAVAILABLE;
    }
    uint32_t found_off = (uint32_t)placed;
    /* Pin AFTER the remap so the run is never reused/relocated for the driver's
     * lifetime.  Single-threaded WARP kernel context: nothing allocates between
     * alloc and pin. */
    pfa_pin_pages(phys_base, pages);
    warp_shmem_map_track(ctx->pid, WARP_REGION_TRACK_ID(found_off), found_off,
                         (uint32_t)region_bytes);
    __builtin_memcpy(out_ptr, &phys_base, sizeof(uint64_t));
    capability_dma_commit(context_id, region_bytes); /* charge the pinned footprint */
    return found_off;
}

/* Overlay the caller's own 8 KiB block buffer into its linear-memory window so
 * the owner reads/writes block data in place instead of copying through
 * block_buffer_copy/write.  Idempotent.  Unlike warp_region_alloc the pages are
 * NOT pinned: they are owned by the block slot and freed by warp_release_pid on
 * reap, which also untracks this window. */
static uint32_t warp_block_buffer_map(void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!ctx)
        return (uint32_t)-1;
    auto* slot = warp_block_slot(ctx->pid);
    if (!slot)
        return (uint32_t)-1;
    if (!slot->phys) {
        slot->phys = pfa_alloc_pages_below(WARP_BLOCK_BUF_PAGES, 512ULL * 1024 * 1024);
        if (!slot->phys)
            return (uint32_t)-1;
    }
    if (slot->map_off)
        return slot->map_off;

    const uint32_t window = (uint32_t)(WARP_BLOCK_BUF_PAGES * 0x1000ULL);
    int64_t placed = warp_linmem_place_phys(ctx, slot->phys, WARP_BLOCK_BUF_PAGES, window);
    if (placed < 0)
        return (uint32_t)-1;
    uint32_t off = (uint32_t)placed;
    warp_shmem_map_track(ctx->pid, WARP_REGION_TRACK_ID(off), off, window);
    slot->map_off = off;
    return off;
}

/* Synthetic tracking-id namespace for xfer-buffer linmem overlays: bit 30 set
 * keeps them out of the small positive shmem id space AND the region-window
 * namespace (bit 31), so all three participate in overlap checks collision-free. */
#define WARP_XFER_TRACK_ID(id) ((uint32_t)(id) | 0x40000000u)

/* Overlay the caller's OWNED xfer-buffer into its linear memory so the socket
 * rings are driven by pointer (ringbuf.h) rather than copy hostcalls — the
 * zero-copy data plane of docs/architecture/22. Mirrors warp_shmem_map_auto but
 * resolves the phys backing from the xfer-buffer object; owner-only, so no DMA
 * capability is required. Idempotent per buffer_id.
 *
 * The overlay takes a phys refcount (pfa_pin_pages) for the lifetime of the
 * mapping, released in warp_xfer_buffer_unmap. This mirrors the region overlay
 * path and is required for correctness: linmem_slot_decommit frees the phys of
 * every page still mapped in the slot at teardown, so if the process exits or
 * TRAPS before unmapping, decommit would free these object-owned pages and the
 * xfer-buffer owner would then double-free them on release/reap. The pin makes
 * decommit's free a harmless refcount decrement, leaving the single real free to
 * the object's owner. */
static uint32_t warp_xfer_buffer_map(uint32_t buffer_id, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!ctx || (int32_t)buffer_id <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    xfer_buffer_t buf;
    if (xfer_buffer_describe(buffer_id, BUFFER_KIND_TRANSFER, context_id, &buf) != WASMOS_ERR_NONE)
        return (uint32_t)-1;
    xfer_buffer_owner_t owner;
    if (xfer_buffer_get_owned(&buf, context_id, &owner) != WASMOS_ERR_NONE)
        return (uint32_t)-1; /* the overlay is the owner's private in-place view */
    uint64_t phys_base = xfer_buffer_object_phys(&buf);
    if (phys_base == 0 || (phys_base & 0xFFFULL) != 0 || buf.size_bytes == 0)
        return (uint32_t)-1;
    uint32_t track_id = WARP_XFER_TRACK_ID(buffer_id);
    WarpShmemLinearMap* existing = warp_shmem_map_find(ctx->pid, track_id);
    if (existing)
        return existing->offset; /* idempotent */
    uint64_t pages = ((uint64_t)buf.size_bytes + 0xFFFULL) >> 12;
    uint32_t window = (uint32_t)(pages << 12);
    int64_t placed = warp_linmem_place_phys(ctx, phys_base, pages, window);
    if (placed < 0)
        return (uint32_t)placed;
    /* Hold a refcount on the object pages while the overlay is mapped so a slot
     * decommit at teardown (process exit/trap without an unmap) cannot free them
     * out from under the xfer-buffer owner. Released in warp_xfer_buffer_unmap. */
    pfa_pin_pages(phys_base, pages);
    warp_shmem_map_track(ctx->pid, track_id, (uint32_t)placed, window);
    return (uint32_t)placed;
}

/* Tear an xfer-buffer overlay window down. This is a MAPPING teardown: it clears
 * the overlay PTEs and drops the phys refcount that warp_xfer_buffer_map took
 * (pfa_free_pages here is that pin release, NOT the object's owning free — the
 * object's backing is freed by its owner on release/reap). Clearing the PTEs
 * means a later slot decommit (paging_virt_to_phys) sees nothing to free; the pin
 * release balances the map-time pin so the object refcount returns to just the
 * owner's. Returns 0 when there was nothing mapped. */
static uint32_t warp_xfer_buffer_unmap(uint32_t buffer_id, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!ctx || (int32_t)buffer_id <= 0)
        return (uint32_t)-1;
    uint32_t track_id = WARP_XFER_TRACK_ID(buffer_id);
    WarpShmemLinearMap* slot = warp_shmem_map_find(ctx->pid, track_id);
    if (!slot)
        return 0;
    uint64_t pages = ((uint64_t)slot->size + 0xFFFULL) / 0x1000ULL;
    uint8_t* base = warp_linear_mem_window(ctx, slot->offset, slot->size);
    if (base) {
        uint64_t virt = addr_cast(uint64_t, base);
        for (uint64_t i = 0; i < pages; ++i) {
            (void)paging_unmap_4k(virt + i * 0x1000ULL);
        }
    }
#ifdef WASMOS_WASM_RUNTIME_WARP
    uint8_t* linmem_base = ctx->module->getLinearMemoryRegion(0, 0);
    if (linmem_base) {
        (void)warp_ring3_sync_linmem_user_window(linmem_base);
    }
#endif
    /* Release the map-time pin. Re-resolve the object's phys from the still-owned
     * buffer; if it cannot be resolved (already released), skip — the object free
     * path already balanced the refcount. */
    uint32_t context_id = 0;
    xfer_buffer_t buf;
    if (warp_current_context_id(&context_id) == 0 &&
        xfer_buffer_describe(buffer_id, BUFFER_KIND_TRANSFER, context_id, &buf) ==
            WASMOS_ERR_NONE) {
        uint64_t phys_base = xfer_buffer_object_phys(&buf);
        if (phys_base != 0 && (phys_base & 0xFFFULL) == 0) {
            pfa_free_pages(phys_base, pages);
        }
    }
    warp_shmem_map_untrack(ctx->pid, track_id);
    return 0;
}

static uint32_t warp_shmem_unmap(uint32_t id, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    WarpShmemLinearMap* slot = warp_shmem_map_find(process_current_pid(), id);
    if (slot && warp_restore_linear_window(ctx, slot->offset, slot->size) != 0) {
        return (uint32_t)-1;
    }
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (slot) {
        uint8_t* linmem_base = ctx->module->getLinearMemoryRegion(0, 0);
        if (!linmem_base || warp_ring3_sync_linmem_user_window(linmem_base) != 0) {
            return (uint32_t)-1;
        }
    }
#endif
    warp_shmem_map_untrack(process_current_pid(), id);
    return (uint32_t)mm_shared_release(context_id, id);
}

static uint32_t warp_shmem_flush(uint32_t id, uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)-1;
    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0 || shared_pages == 0 ||
        phys_base == 0)
        return (uint32_t)-1;
    if ((uint64_t)size > shared_pages * 0x1000ULL)
        return (uint32_t)-1;
    const uint8_t* src = warp_linear_mem_window(ctx, wasm_off, size);
    if (!src)
        return (uint32_t)-1;
    __builtin_memcpy(ptr_cast(void, (phys_base | KERNEL_HIGHER_HALF_BASE)), src, size);
    return 0;
}

static uint32_t warp_shmem_refresh(uint32_t id, uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)-1;
    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0 || shared_pages == 0 ||
        phys_base == 0)
        return (uint32_t)-1;
    if ((uint64_t)size > shared_pages * 0x1000ULL)
        return (uint32_t)-1;
    /* shmem_refresh writes into a region already committed by shmem_map_auto.
     * Use warp_linear_mem_window (size=0 probe, no ensureLinearSize) instead of
     * getLinearMemoryRegion with a non-zero size, which would trigger
     * ensureLinearSize and potentially extend the linear memory past its current
     * limit causing a page fault at the new boundary. */
    uint8_t* dst = warp_linear_mem_window(ctx, wasm_off, size);
    if (!dst)
        return (uint32_t)-1;
    __builtin_memcpy(dst, ptr_cast(const void, (phys_base | KERNEL_HIGHER_HALF_BASE)), size);
    return 0;
}

// ---------------------------------------------------------------------------
// IRQ routing
// ---------------------------------------------------------------------------

static uint32_t warp_irq_route_ipc(uint32_t irq_line, uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_irq_capability(context_id) != 0)
        return (uint32_t)-1;
    return (uint32_t)irq_register(context_id, irq_line, endpoint);
}

static uint32_t warp_irq_ack(uint32_t irq_line, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)-1;
    return (uint32_t)irq_ack(context_id, irq_line);
}

/* Configure an IRQ line's trigger/polarity (flags: bit0=level, bit1=active-low).
 * Gated by the IRQ capability; pci-bus uses it to mark PCI INTx lines
 * level/active-low. (TODO: split a dedicated irq.configure capability from
 * irq.route for tighter privilege separation — see docs/architecture/09.) */
static uint32_t warp_irq_configure(uint32_t irq_line, uint32_t flags, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_irq_capability(context_id) != 0)
        return (uint32_t)-1;
    return (uint32_t)irq_configure(irq_line, flags);
}

static uint32_t warp_irq_unroute(uint32_t irq_line, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_irq_capability(context_id) != 0)
        return (uint32_t)-1;
    return (uint32_t)irq_unregister(context_id, irq_line);
}

// ---------------------------------------------------------------------------
// Serial / input
// ---------------------------------------------------------------------------

static uint32_t warp_serial_register(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    return (uint32_t)serial_register_remote_driver(endpoint);
}

static uint32_t warp_input_push(uint32_t ch, void* ctx_) {
    (void)ctx_;
    serial_input_push((uint8_t)(ch & 0xFF));
    return 0;
}

static uint32_t warp_input_read(void* ctx_) {
    (void)ctx_;
    uint8_t ch = 0;
    return serial_input_read(&ch) ? (uint32_t)ch : (uint32_t)-1;
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

static uint32_t warp_framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color, void* ctx_) {
    (void)ctx_;
    return (uint32_t)framebuffer_put_pixel(x, y, color);
}

static uint32_t warp_framebuffer_info(uint32_t out_off, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)len < (int32_t)sizeof(framebuffer_info_t))
        return (uint32_t)-1;
    framebuffer_info_t info;
    __builtin_memset(&info, 0, sizeof(info));
    if (framebuffer_get_info(&info) != 0)
        return (uint32_t)-1;
    uint8_t* out = warp_mem(ctx, out_off, sizeof(framebuffer_info_t));
    if (!out)
        return (uint32_t)-1;
    __builtin_memcpy(out, &info, sizeof(info));
    return 0;
}

static uint32_t warp_framebuffer_map(uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)size <= 0 || (size & 0xFFF))
        return (uint32_t)-1;
    framebuffer_info_t info;
    __builtin_memset(&info, 0, sizeof(info));
    if (framebuffer_get_info(&info) != 0)
        return (uint32_t)-1;
    if (size < info.framebuffer_size)
        return (uint32_t)-1;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_mmio_capability(context_id) != 0)
        return (uint32_t)-1;
    uint8_t* lmem = warp_linear_mem_window(ctx, wasm_off, size);
    if (!lmem || (addr_cast(uint64_t, lmem) & 0xFFF))
        return (uint32_t)-1;
    uint64_t virt = addr_cast(uint64_t, lmem);
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

static uint32_t warp_boot_config_size(void* ctx_) {
    (void)ctx_;
    if (!g_warp_boot_info || !g_warp_boot_info->boot_config ||
        g_warp_boot_info->boot_config_size == 0)
        return (uint32_t)-1;
    return (uint32_t)g_warp_boot_info->boot_config_size;
}

static uint32_t warp_boot_config_copy(uint32_t buf_off, uint32_t len, uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!g_warp_boot_info || !g_warp_boot_info->boot_config)
        return (uint32_t)-1;
    uint32_t total = (uint32_t)g_warp_boot_info->boot_config_size;
    if (offset > total || len > total - offset)
        return (uint32_t)-1;
    if (len == 0)
        return 0;
    uint8_t* dst = warp_mem(ctx, buf_off, len);
    if (!dst)
        return (uint32_t)-1;
    const uint8_t* src = static_cast<const uint8_t*>(g_warp_boot_info->boot_config);
    __builtin_memcpy(dst, src + offset, len);
    return 0;
}

// ---------------------------------------------------------------------------
// initfs_find_path
// ---------------------------------------------------------------------------

/* ASCII case-insensitive string equality — matches wasm3's strcasecmp-based
 * initfs path matching so both runtimes resolve the same names. */
static bool warp_path_ieq(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return false;
        if (ca == '\0')
            return true;
    }
}

static uint32_t warp_initfs_find_path(uint32_t path_off, uint32_t path_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)path_len <= 0 || path_len >= 112u)
        return (uint32_t)-1;
    const uint8_t* raw = warp_mem(ctx, path_off, path_len);
    if (!raw)
        return (uint32_t)-1;
    char local_path[112];
    __builtin_memcpy(local_path, raw, path_len);
    local_path[path_len] = '\0';
    uint32_t ri = 0;
    while (local_path[ri] == '/')
        ri++;
    if ((local_path[ri] == 'i' || local_path[ri] == 'I') &&
        (local_path[ri + 1] == 'n' || local_path[ri + 1] == 'N') &&
        (local_path[ri + 2] == 'i' || local_path[ri + 2] == 'I') &&
        (local_path[ri + 3] == 't' || local_path[ri + 3] == 'T') && local_path[ri + 4] == '/')
        ri += 5;
    if (local_path[ri] == '\0')
        return (uint32_t)-1;
    const wasmos_initfs_header_t* hdr = nullptr;
    const uint8_t* base = nullptr;
    if (warp_initfs_header_get(&hdr, &base) != 0)
        return (uint32_t)-1;
    for (uint32_t i = 0; i < hdr->entry_count; ++i) {
        wasmos_initfs_entry_t e;
        if (warp_initfs_entry_at(i, &e) != 0)
            continue;
        if (warp_path_ieq(e.path, &local_path[ri]))
            return i;
        const char* bn = e.path;
        for (uint32_t j = 0; e.path[j]; ++j)
            if (e.path[j] == '/')
                bn = &e.path[j + 1];
        if (warp_path_ieq(bn, &local_path[ri]))
            return i;
    }
    return (uint32_t)-1;
}

// ---------------------------------------------------------------------------
// Early log
// ---------------------------------------------------------------------------

static uint32_t warp_early_log_size(void* ctx_) {
    (void)ctx_;
    return (uint32_t)serial_early_log_size();
}

static uint32_t warp_early_log_copy(uint32_t buf_off, uint32_t len, uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t total = (uint32_t)serial_early_log_size();
    if (offset > total || len > total - offset)
        return (uint32_t)-1;
    if (len == 0)
        return 0;
    uint8_t* dst = warp_mem(ctx, buf_off, len);
    if (!dst)
        return (uint32_t)-1;
    serial_early_log_copy(dst, offset, len);
    return 0;
}

static uint32_t warp_debug_mark(uint32_t /*tag*/, void* ctx_) {
    (void)ctx_;
    return 0;
}
static uint32_t warp_kmap_dump(void* ctx_) {
    (void)ctx_;
    return 0;
}
static uint32_t warp_kmap_dump_all(void* ctx_) {
    (void)ctx_;
    return 0;
}
static uint32_t warp_sched_ready_count(void* ctx_) {
    (void)ctx_;
    return 0;
}
static uint32_t warp_sched_cpu_count(void* ctx_) {
    (void)ctx_;
    return (uint32_t)g_cpu_count;
}
static uint32_t warp_kernel_runtime(void* ctx_) {
    (void)ctx_;
    return 1u; /* WARP */
}

static uint32_t warp_physmem_stats(uint32_t out_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    typedef struct {
        uint64_t total_bytes;
        uint64_t free_bytes;
    } physmem_stats_t;
    uint8_t* raw = warp_mem(ctx, out_off, sizeof(physmem_stats_t));
    if (!raw)
        return (uint32_t)-1;
    physmem_stats_t tmp;
    tmp.total_bytes = pfa_total_bytes();
    tmp.free_bytes = pfa_free_bytes();
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
static void warp_env_abort(uint32_t msg, uint32_t file, uint32_t line, uint32_t column,
                           void* ctx_) {
    (void)msg;
    (void)file;
    (void)line;
    (void)column;
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
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
#include "wasmos_symbols_warp.inc"

vb::Span<vb::NativeSymbol const> warp_wasmos_symbols(void) {
    // STATIC_LINK: bakes function pointers into call stubs at JIT compile time.
    // Smaller basedata → no reallocation pressure during JIT execution.
    static vb::NativeSymbol syms[] = {WASMOS_SYMBOLS(STATIC_LINK)};
    return vb::Span<vb::NativeSymbol const>(syms, sizeof(syms) / sizeof(syms[0]));
}

// Used by initFromCompiledBinary (AOT load path).  DYNAMIC_LINK is required
// because initFromCompiledBinary throws Wrong_type on any STATIC symbol.
vb::Span<vb::NativeSymbol const> warp_wasmos_symbols_for_aot_load(void) {
    static vb::NativeSymbol syms[] = {WASMOS_SYMBOLS(DYNAMIC_LINK)};
    return vb::Span<vb::NativeSymbol const>(syms, sizeof(syms) / sizeof(syms[0]));
}

// Context accessor — called from warp_driver.cpp after WasmModule construction
// to wire up the per-module call context so host functions can access linear
// memory and the calling pid.
static WarpCallContext* ctx_acquire(uint32_t pid) {
    if (pid == 0) {
        return nullptr;
    }
    WarpCallContext* c = static_cast<WarpCallContext*>(hashmap_put(&g_ctx_map, pid));
    if (!c) {
        return nullptr; // out of memory — genuine, not a fixed cap
    }
    /* hashmap_put zeroes new entries and returns existing ones unchanged. */
    c->pid = pid;
    c->boot_info = g_warp_boot_info;
    return c;
}

void warp_bind_module(vb::WasmModule* module, uint32_t pid) {
    WarpCallContext* c = ctx_acquire(pid);
    if (!c) {
        return;
    }
    c->module = module;
    c->pid = pid;
    module->setContext(c);
}

void* warp_context_for_pid(uint32_t pid) {
    return ctx_acquire(pid);
}

void warp_ctx_release_pid(uint32_t pid) {
    (void)hashmap_remove(&g_ctx_map, pid);
}

/* Release ALL per-pid WARP state for an exiting process.  Called once from
 * process_reap() (the single funnel, alongside wasm3_heap_release /
 * native_driver_heap_release).  Without this, the per-pid slot pools below
 * accumulate one dead entry per spawn and eventually wedge new spawns.  This
 * only clears bookkeeping (hashmap entry + slot tags); the heavyweight ring-3
 * teardown stays in wasm_driver_stop(), so this path touches no page tables. */
extern "C" void warp_release_pid(uint32_t pid) {
    if (pid == 0) {
        return;
    }
    (void)hashmap_remove(&g_ctx_map, pid);
    (void)hashmap_remove(&g_ipc_last_map, pid);
    (void)hashmap_remove(&g_fs_peer_map, pid);
    /* Reclaim the per-pid block buffer pages before dropping the slot, else the
     * 2 pages leak on every process exit. */
    if (auto* bslot = static_cast<WarpBlockSlot*>(hashmap_get(&g_block_map, pid))) {
        if (bslot->map_off) {
            warp_shmem_map_untrack(pid, WARP_REGION_TRACK_ID(bslot->map_off));
        }
        if (bslot->phys) {
            pfa_free_pages(bslot->phys, WARP_BLOCK_BUF_PAGES);
        }
    }
    (void)hashmap_remove(&g_block_map, pid);
    /* Per-pid WARP heap config (warp/shim.cpp). */
    warp_heap_release(pid);
}

extern "C" void wasm3_release_pid(uint32_t pid) {
    (void)pid;
}

#ifdef WASMOS_WASM_RUNTIME_WARP
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

vb::Span<vb::NativeSymbol const> warp_wasmos_symbols_ring3(void) {
    static constexpr int g_r3_sym_counter_base = __COUNTER__;
#define R3_LINK(module, name, fnc)                                                                 \
    vb::NativeSymbol {                                                                             \
        vb::NativeSymbol::Linkage::DYNAMIC, module, name,                                          \
            vb::function_traits<vb::remove_noexcept_t<decltype(fnc)>>::getSignature(),             \
            reinterpret_cast<void const*>(WARP_R3_HC_TRAMPOLINE +                                  \
                                          (uint64_t)(__COUNTER__ - g_r3_sym_counter_base - 1) *    \
                                              8ULL)                                                \
    }
    static vb::NativeSymbol syms[] = {WASMOS_SYMBOLS(R3_LINK)};
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

/* Arg-unpacking dispatch generated from abi/hostcalls.yaml
 * (scripts/gen_abi_hostcalls.py); warp_ring3_dispatch decodes the frame. */
#include "wasmos_ring3_dispatch.inc"

extern "C" uint32_t warp_ring3_dispatch(uint32_t hc_id, void* frame_ptr) {
    syscall_frame_t* frame = static_cast<syscall_frame_t*>(frame_ptr);
    /* user_rsp sits just past syscall_frame_t (pushed by the CPU on the
     * ring-3 -> ring-0 INT transition); stack args live past [user_rsp+0]. */
    uint64_t user_rsp =
        *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(frame) + sizeof(syscall_frame_t));
    return warp_ring3_dispatch_table(hc_id, frame->rdi, frame->rsi, frame->rdx, frame->rcx,
                                     frame->r8, frame->r9, user_rsp);
}
#endif /* WASMOS_WASM_RUNTIME_WARP */

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

void warp_link_init(const boot_info_t* boot_info) {
    g_warp_boot_info = boot_info;
    warp_ipc_slots_init();
    /* Pre-sized to comfortably exceed the typical live-process count so the
     * spawn hot path does not trigger a rehash (which would malloc under the
     * preempt-guard drain). It still grows automatically if exceeded. */
    hashmap_init(&g_ctx_map, sizeof(WarpCallContext), 64);
}

#ifdef WASMOS_WASM_RUNTIME_WARP
int warp_sync_linmem_for_pid(uint32_t pid, uint64_t user_root) {
    if (pid == 0 || user_root == 0) {
        return -1;
    }
    WarpCallContext* ctx = ctx_find(pid);
    if (!ctx || !ctx->module) {
        return -1;
    }
    uint8_t* linmem = ctx->module->getLinearMemoryRegion(0, 0);
    if (!linmem) {
        return -1;
    }
    return warp_mem_ring3_map_linmem(user_root, linmem);
}
#endif

} // extern "C"
