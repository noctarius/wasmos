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
 * The per-pid IPC/FS side tables are owned here and are a separate copy from the
 * equivalent tables in wasm3/link.c; only one runtime is compiled into a kernel.
 *
 * The symbol table is generated from abi/hostcalls.yaml, so it carries every
 * host call whose `runtimes` list includes warp.  `env.strlen` is wasm3-only and
 * has no counterpart here; `wasi_snapshot_preview1.{proc_exit,random_get}` are
 * WARP-only and have no counterpart in wasm3/link.c.
 */

/* The WASMOS_SYMBOLS macro references static host functions by taking their
 * address.  Clang may not track this as an ODR-use and spuriously reports
 * them as unused when the reference is inside a nested macro expansion. */
#pragma clang diagnostic ignored "-Wunused-function"

#include <cstdint>
#include <cstring>
#include <array>

#include "../include/xfer_buffer.h"
#include "link_dma.h"
#include "link_ipc.h"

extern "C" {
#include "boot.h"
#include "warp/shim.h"
#include "block_buffer.h"
#include "hostcall_value.h"
#include "kenv.h"
#include "hostcall_buffer.h"
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
#include "msi.h"
#include "mmio.h"
#include "serial.h"
#include "capability.h"
#include "timer.h"
#include "policy.h"
#include "linmem_slots.h"
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

/* Look up the WARP call context for `pid` without creating one.  Returns nullptr for
 * pid 0 and for a pid whose module has not been bound (or was already released). */
static WarpCallContext* ctx_find(uint32_t pid) {
    if (pid == 0) {
        return nullptr;
    }
    return static_cast<WarpCallContext*>(hashmap_get(&g_ctx_map, pid));
}

/* Page-aligned scratch page used by warp_phys_map for ACPI/physical memory
 * reads.  ACPI physical pages are not necessarily in the kernel direct map
 * (they are EfiACPIReclaimMemory regions), so phys|HIGHER_HALF is not usable
 * directly.  This kernel BSS page (which IS in the page tables) is remapped to
 * the target physical page, the data copied, then the mapping restored. */
static uint8_t g_phys_scratch[4096] __attribute__((aligned(4096)));

/* Kernel-side pointer for the guest linear-memory range [offset, offset+size).
 * `offset` is a wasm32 offset, never a host address; the returned pointer is a KERNEL
 * alias of those bytes, while a ring-3 guest reaches the same bytes at
 * WARP_R3_LINMEM_BASE plus the linear-memory base's own sub-page offset plus `offset`,
 * so this pointer is never what the guest sees.  It is borrowed and only valid until
 * something can move or extend linear memory (a later probe past the current end, or
 * warp_r3_memory_helper); re-fetch the base after any such call rather than caching it.
 * Returns nullptr when the context has no module or the range leaves linear memory. */
static inline uint8_t* warp_mem(WarpCallContext* ctx, uint32_t offset, uint32_t size) {
    /* Use the full getLinearMemoryRegion(offset, size) path with non-zero size.
     * With LINEAR_MEMORY_BOUNDS_CHECKS=1, this triggers probe() →
     * ensureLinearSize() which zero-initialises newly-committed WASM pages.
     * When warp_mem is used to obtain a WRITE destination, the zeroing happens
     * BEFORE the write — so the written data overwrites the zeros safely.
     * Future probe() calls for those offsets then short-circuit (already
     * committed) without zeroing it again. */
    if (!ctx || !ctx->module)
        return nullptr;
    return ctx->module->getLinearMemoryRegion(offset, size);
}

/* Kernel-side pointer for a range that is already committed, bounded by the module's
 * DECLARED linear-memory size (pages << 16) and derived from a zero-size base probe.
 * Unlike warp_mem this cannot trigger ensureLinearSize, so it neither commits nor
 * zero-fills — which is the point at a mapped window, where a commit would overwrite
 * the mapped pages.  Use warp_mem for a range the guest may not have touched yet.
 * Returns nullptr when the module is absent or the range leaves declared memory. */
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

/* Resolve the call context for the host call now running.  The running process's own
 * bound context wins; otherwise `ctx_` is used, either as a registered context or —
 * for the ring-3 path, where the JIT hands over the module — as a vb::WasmModule*
 * whose setContext() value is the context.  Returns nullptr when neither resolves, and
 * every caller must treat that as "no guest memory reachable". */
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

/* Per-pid IPC last-message slots, keyed by pid in a growable hashmap (no fixed
 * process-count bound).  Created on first use for a pid and removed on exit via
 * warp_release_pid.  Value addresses are stable for a key's lifetime. */
static hashmap_t g_ipc_last_map;

static hashmap_t g_fs_peer_map;

static void warp_block_slots_init(void);

/* Create the per-pid IPC last-message, FS-peer and block-DMA tables.  Called once from
 * warp_link_init; the tables grow on demand and entries are dropped by
 * warp_release_pid. */
static void warp_ipc_slots_init(void) {
    hashmap_init(&g_ipc_last_map, sizeof(WarpIpcLastSlot), 64);
    hashmap_init(&g_fs_peer_map, sizeof(WarpFsPeerSlot), 64);
    warp_block_slots_init();
}

WarpIpcLastSlot* warp_ipc_slot_for_pid(uint32_t pid) {
    if (!pid)
        return nullptr;
    auto* slot = static_cast<WarpIpcLastSlot*>(hashmap_put(&g_ipc_last_map, pid));
    if (slot)
        slot->pid = pid;
    return slot;
}

WarpFsPeerSlot* warp_fs_peer_slot_for_pid(uint32_t pid) {
    if (!pid) {
        return nullptr;
    }
    auto* slot = static_cast<WarpFsPeerSlot*>(hashmap_put(&g_fs_peer_map, pid));
    if (slot)
        slot->pid = pid;
    return slot;
}

/* Counterpart of wasm3/link.c's current_process_context(): 0 with *out set to
 * the calling process's context id, or -1 when there is no current process. */
int warp_current_context_id(uint32_t* out) {
    uint32_t pid = process_current_pid();
    process_t* proc = process_get(pid);
    if (!proc || !out)
        return -1;
    *out = proc->context_id;
    return 0;
}

/* NUL-terminated string equality; 0 for a null argument.  Currently unreferenced — the
 * -Wunused-function suppression at the top of this file hides that. */
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

uint8_t warp_dbg_ipc_trace_process(process_t* proc) {
    (void)proc;
    return 0;
}

// ---------------------------------------------------------------------------
// Host call wrappers — console (the IPC wrappers live in warp/link_ipc.cpp)
// ---------------------------------------------------------------------------

static uint32_t warp_console_read(uint32_t buf_offset, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)len <= 0)
        return (uint32_t)WASMOS_INVAL;
    uint8_t* buf = warp_mem(ctx, buf_offset, 1);
    if (!buf)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    uint8_t ch = 0;
    int rc = serial_read_char(&ch);
    if (rc <= 0)
        return (uint32_t)rc;
    *buf = ch;
    return 1;
}

/* hostcalls.yaml `console_write`.  Relays `len` bytes of guest memory at `buf_offset`
 * to the kernel log; the guest buffer is read-only to this call.  Returns 0 on success,
 * 0 for len == 0 (a no-op, not a failure), WASMOS_INVAL for a negative `len`, and
 * WASMOS_ERR_KERNEL_BAD_POINTER when the range leaves the caller's linear memory.
 * Preemption is disabled across the whole relay so one write is not interleaved. */
static uint32_t warp_console_write(uint32_t buf_offset, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)len < 0)
        return (uint32_t)WASMOS_INVAL;
    if (len == 0)
        return 0; /* nothing to write is not a failure */
    uint8_t* buf = warp_mem(ctx, buf_offset, len);
    if (!buf)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    /* klog_write takes a NUL-terminated string but the guest buffer is a counted
     * range, so the payload is relayed in <= 127-byte chunks through a stack
     * buffer that is terminated in place.  The guest buffer is never written. */
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

/* wasi_snapshot_preview1.proc_exit — registered under WARP only; wasm3 links no WASI
 * imports.  Same effect as warp_proc_exit with a void WASM signature. */
static void warp_wasi_proc_exit(uint32_t code, void* ctx_) {
    (void)warp_proc_exit(code, ctx_);
}

/* wasi_snapshot_preview1.random_get — registered under WARP only.  Fills `len` bytes of
 * guest memory at `buf_offset` with ZEROS, so it satisfies a startup probe but is not a
 * source of randomness; guests needing entropy must use the hardware RNG service.
 * Returns 0 on success and for len == 0, WASMOS_ERR_KERNEL_BAD_POINTER for a range
 * outside the caller's linear memory. */
static uint32_t warp_wasi_random_get(uint32_t buf_offset, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (len == 0) {
        return 0;
    }
    uint8_t* buf = warp_mem(ctx, buf_offset, len);
    if (!buf) {
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    }
    /* Minimal WASI compatibility for guest runtimes that probe randomness
     * during startup. Deterministic zero-fill is sufficient for current WASMOS
     * guests, which only require the call not to trap. */
    __builtin_memset(buf, 0, len);
    return 0;
}

/* hostcalls.yaml `proc_notify_ready`: mark the calling process initialised so a waiting
 * spawner is released.  Returns 0 unconditionally, including when there is no current
 * process. */
static uint32_t warp_proc_notify_ready(void* ctx_) {
    (void)ctx_;
    process_t* proc = process_get(process_current_pid());
    if (proc)
        process_notify_ready(proc);
    return 0;
}

/* hostcalls.yaml `sched_yield`: give the CPU up voluntarily.  Blocks the caller only
 * until the scheduler picks it again; returns 0. */
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
        /* Named, not a bare -1: futex_wait's own returns are packed codes, and
         * a guest cannot act on a value it cannot tell apart from a timeout. */
        return (uint32_t)IPC_ERR_INVALID;
    (void)ctx;
    return (uint32_t)futex_wait(addr_off, val, timeout_ms, context_id);
}

/* hostcalls.yaml `futex_wake`.  `addr_off` is a raw guest linear-memory offset — the
 * futex table keys on the (context, offset) pair, so no host pointer is derived and the
 * offset is not dereferenced here.  Returns the number of waiters actually woken (0 if
 * none), or WASMOS_ERR_KERNEL_NO_CALLER when the caller's context cannot be resolved. */
static uint32_t warp_futex_wake(uint32_t addr_off, uint32_t count, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_KERNEL_NO_CALLER;
    (void)ctx;
    return (uint32_t)futex_wake(addr_off, count, context_id);
}

// ---------------------------------------------------------------------------
// IPC select set operations
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FS shared buffer
// ---------------------------------------------------------------------------

static uint32_t warp_xfer_buffer_size(void* ctx_) {
    (void)ctx_;
    return (uint32_t)xfer_buffer_size(BUFFER_KIND_TRANSFER);
}

/* hostcalls.yaml `fs_endpoint`: the endpoint of whichever service currently holds the
 * FS registration.  Returns that endpoint id, or WASMOS_NOENT before any FS service has
 * registered.  The value can change across calls, so it is not cacheable for the life
 * of the process. */
static uint32_t warp_fs_endpoint(void* ctx_) {
    (void)ctx_;
    uint32_t ep = process_manager_fs_endpoint();
    /* no FS service has registered yet */
    return (ep == IPC_ENDPOINT_NONE) ? (uint32_t)WASMOS_NOENT : ep;
}

/* hostcalls.yaml `xfer_buffer_read`: copy `len` bytes out of transfer buffer
 * `buffer_id` starting at `offset` into guest memory at `ptr_off`.  The caller must be
 * the owner or a borrower holding BUFFER_BORROW_READ.  Returns 0 on success and for
 * len == 0, otherwise a negative WASMOS_ERR_XFER_BUFFER_* code.  Under ring 3 the copy
 * lands in the kernel alias first and the destination pages are then re-published into
 * the guest's own root, so the guest sees them without a further sync. */
static uint32_t warp_xfer_buffer_read(uint32_t buffer_id, uint32_t ptr_off, uint32_t len,
                                      uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    /* A zero-length transfer is a no-op success. wasm3 refused it until this
     * was settled; there is nothing to move, so there is nothing to fail. */
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

/* hostcalls.yaml `xfer_buffer_write`: the reverse direction, requiring
 * BUFFER_BORROW_WRITE.  Reads `len` bytes of guest memory at `ptr_off` into the buffer
 * at `offset`.  Returns 0 on success and for len == 0, otherwise a negative
 * WASMOS_ERR_XFER_BUFFER_* code.  No ring-3 republish is needed: the destination is the
 * buffer object, not guest memory. */
static uint32_t warp_xfer_buffer_write(uint32_t buffer_id, uint32_t ptr_off, uint32_t len,
                                       uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    /* A zero-length transfer is a no-op success. wasm3 refused it until this
     * was settled; there is nothing to move, so there is nothing to fail. */
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

/* hostcalls.yaml `buffer_acquire`.  Acquire a buffer of `kind` (only
 * BUFFER_KIND_TRANSFER is accepted) with at least `minimum_size` bytes; the caller
 * becomes its owner and must release it.  `minimum_size` is a floor, not the resulting
 * capacity: the object may be larger, and its actual extent is what xfer_buffer_describe
 * reports.  Returns the new buffer_id, or a negative WASMOS_ERR_XFER_BUFFER_* code. */
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

/* hostcalls.yaml `buffer_release`: the OWNER drops `buffer_id` and its backing.  A
 * borrower cannot release; it gets WASMOS_ERR_XFER_BUFFER_NO_ACCESS from the ownership
 * lookup.  Returns 0 on success, otherwise a negative WASMOS_ERR_XFER_BUFFER_* code. */
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

/* Create the per-pid block-DMA slot table.  Called from warp_ipc_slots_init. */
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

/* hostcalls.yaml `block_buffer_phys`: physical base of this process's private 8 KiB
 * block buffer, allocated on first call and freed by warp_release_pid.  This backend
 * allocates below 512 MiB (the kernel's higher-half window and the 32-bit ATA DMA
 * range); the wasm3 backend uses BLOCK_BUFFER_PHYS_LIMIT (2 GiB), so a guest cannot
 * assume the address is in the low 512 MiB.
 * Returns the address as a u32 — it shares one signed i32 with the error codes, which
 * is why block_buffer_check_phys asserts bit 31 stays clear instead of trusting the
 * allocator's pool.  Errors are WASMOS_ERR_BLOCK_NO_SLOT / NO_BACKING / ABOVE_4G. */
static uint32_t warp_block_buffer_phys(void* ctx_) {
    (void)ctx_;
    uint32_t pid = process_current_pid();
    auto* slot = warp_block_slot(pid);
    if (!slot)
        return (uint32_t)WASMOS_ERR_BLOCK_NO_SLOT;
    if (!slot->phys) {
        /* Must be < 512MB: that's the kernel's higher-half identity mapping
         * window AND within ATA's 32-bit DMA address range. That is already
         * well under the 2 GiB the ABI can express, which block_buffer_check_phys
         * asserts so a later pool change cannot turn an address into an error. */
        slot->phys = pfa_alloc_pages_below(WARP_BLOCK_BUF_PAGES, 512ULL * 1024 * 1024);
        if (!slot->phys)
            return (uint32_t)WASMOS_ERR_BLOCK_NO_BACKING;
    }
    wasmos_error_code_t phys_rc = block_buffer_check_phys(slot->phys);
    if (phys_rc != WASMOS_OK)
        return (uint32_t)phys_rc;
    return (uint32_t)slot->phys;
}

/* hostcalls.yaml `block_buffer_copy`: copy `len` bytes out of the block buffer named by
 * physical address `phys`, from `offset`, into guest memory at `ptr_off`.  `phys` is
 * looked up across ALL processes' slots, so a driver can drain a buffer whose owner is
 * a different process.  Returns 0 on success, WASMOS_ERR_BLOCK_NO_SLOT for an unknown
 * `phys`, WASMOS_ERR_BLOCK_RANGE when [offset, offset+len) leaves the 8 KiB buffer, or
 * WASMOS_ERR_KERNEL_BAD_POINTER for a guest range outside linear memory. */
static uint32_t warp_block_buffer_copy(uint32_t phys, uint32_t ptr_off, uint32_t len,
                                       uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    auto* slot = warp_block_slot_by_phys((uint64_t)phys);
    if (!slot)
        return (uint32_t)WASMOS_ERR_BLOCK_NO_SLOT;
    /* 64-bit, via the shared check: `offset + len` in 32 bits wraps, and a
     * wrapped sum passed the old bound while naming a byte outside the buffer. */
    wasmos_error_code_t range_rc =
        block_buffer_check_range(offset, len, (uint64_t)WARP_BLOCK_BUF_PAGES * 4096ULL);
    if (range_rc != WASMOS_OK)
        return (uint32_t)range_rc;
    uint8_t* wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    uint8_t* buf = reinterpret_cast<uint8_t*>(slot->phys | 0xFFFFFFFF80000000ULL);
    __builtin_memcpy(wasm_ptr, buf + offset, len);
    return 0;
}

/* hostcalls.yaml `block_buffer_write`: the reverse direction of warp_block_buffer_copy,
 * with the same lookup, range rules and return codes. */
static uint32_t warp_block_buffer_write(uint32_t phys, uint32_t ptr_off, uint32_t len,
                                        uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    auto* slot = warp_block_slot_by_phys((uint64_t)phys);
    if (!slot)
        return (uint32_t)WASMOS_ERR_BLOCK_NO_SLOT;
    /* 64-bit, via the shared check: `offset + len` in 32 bits wraps, and a
     * wrapped sum passed the old bound while naming a byte outside the buffer. */
    wasmos_error_code_t range_rc =
        block_buffer_check_range(offset, len, (uint64_t)WARP_BLOCK_BUF_PAGES * 4096ULL);
    if (range_rc != WASMOS_OK)
        return (uint32_t)range_rc;
    uint8_t* wasm_ptr = warp_mem(ctx, ptr_off, len);
    if (!wasm_ptr)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    uint8_t* buf = reinterpret_cast<uint8_t*>(slot->phys | 0xFFFFFFFF80000000ULL);
    __builtin_memcpy(buf + offset, wasm_ptr, len);
    return 0;
}

// ---------------------------------------------------------------------------
// I/O port access
// ---------------------------------------------------------------------------

/* Region-addressed I/O. The driver supplies (region, offset), never an absolute
 * port, so it cannot express an access outside the window its spawn profile
 * granted -- the kernel owns the base. `region` indexes those windows in
 * declaration order. Failures propagate the specific WASMOS_ERR_IO_* reason
 * rather than collapsing to one value: "no such region" and "offset past the
 * end" are different bugs. */
static int warp_io_region_port(uint32_t region, uint32_t offset, uint32_t width,
                               uint16_t* out_port) {
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return WASMOS_ERR_IO_NOT_AUTHORIZED;
    return capability_io_region_port(context_id, region, offset, width, out_port);
}

/* Reads return the datum through linear memory, not as the result: a 32-bit
 * port read can legitimately be 0xFFFFFFFF and must stay distinguishable from a
 * failure code. */
static uint32_t warp_io_region_read(uint32_t region, uint32_t offset, uint32_t out_off, void* ctx_,
                                    uint32_t width) {
    auto* ctx = warp_call_ctx(ctx_);
    uint8_t* raw = warp_mem(ctx, out_off, sizeof(uint32_t));
    uint16_t port = 0;
    uint32_t value = 0;
    int rc = 0;
    if (!raw)
        return (uint32_t)WASMOS_ERR_IO_OUT_OF_WINDOW;
    rc = warp_io_region_port(region, offset, width, &port);
    if (rc != 0)
        return (uint32_t)rc;
    value = (width == 1u) ? (uint32_t)inb(port) : (width == 2u) ? (uint32_t)inw(port) : inl(port);
    __builtin_memcpy(raw, &value, sizeof(value));
    return 0;
}

/* hostcalls.yaml `io_region_in8/16/32`: read from the caller's granted I/O window
 * `region` at `offset`, storing the datum at guest offset `out_off` (1, 2 and 4 bytes
 * respectively, all written through a 4-byte scratch).  Returns 0 on success, otherwise
 * a negative WASMOS_ERR_IO_* code. */
static uint32_t warp_io_region_in8(uint32_t region, uint32_t offset, uint32_t out_off, void* ctx_) {
    return warp_io_region_read(region, offset, out_off, ctx_, 1u);
}
static uint32_t warp_io_region_in16(uint32_t region, uint32_t offset, uint32_t out_off,
                                    void* ctx_) {
    return warp_io_region_read(region, offset, out_off, ctx_, 2u);
}
static uint32_t warp_io_region_in32(uint32_t region, uint32_t offset, uint32_t out_off,
                                    void* ctx_) {
    return warp_io_region_read(region, offset, out_off, ctx_, 4u);
}
/* hostcalls.yaml `io_region_out8/16/32`: write the low 8, 16 or 32 bits of `value` to
 * the caller's granted I/O window `region` at `offset`.  Returns 0 on success,
 * otherwise a negative WASMOS_ERR_IO_* code.  Excess high bits of `value` are masked
 * off rather than refused. */
static uint32_t warp_io_region_out8(uint32_t region, uint32_t offset, uint32_t value, void* ctx_) {
    (void)ctx_;
    uint16_t port = 0;
    int rc = warp_io_region_port(region, offset, 1u, &port);
    if (rc != 0)
        return (uint32_t)rc;
    outb(port, (uint8_t)(value & 0xFFu));
    return 0;
}
static uint32_t warp_io_region_out16(uint32_t region, uint32_t offset, uint32_t value, void* ctx_) {
    (void)ctx_;
    uint16_t port = 0;
    int rc = warp_io_region_port(region, offset, 2u, &port);
    if (rc != 0)
        return (uint32_t)rc;
    outw(port, (uint16_t)(value & 0xFFFFu));
    return 0;
}
static uint32_t warp_io_region_out32(uint32_t region, uint32_t offset, uint32_t value, void* ctx_) {
    (void)ctx_;
    uint16_t port = 0;
    int rc = warp_io_region_port(region, offset, 4u, &port);
    if (rc != 0)
        return (uint32_t)rc;
    outl(port, value);
    return 0;
}

/* See the wasm3 shims: the whole port-read family reports its value through
 * `out` and its outcome through the return. */
static uint32_t warp_io_in8(uint32_t port, uint32_t out_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    /* Split from the capability check so a guest gets the same code here
     * as from wasm3: an out-of-range port is BAD_PORT, not "not authorized". */
    if (port > 0xFFFF) {
        return (uint32_t)WASMOS_ERR_IO_BAD_PORT;
    }
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    uint8_t* out = reinterpret_cast<uint8_t*>(warp_mem(ctx, out_off, sizeof(uint8_t)));
    if (!out) {
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    }
    *out = inb((uint16_t)port);
    return 0;
}
static uint32_t warp_io_in16(uint32_t port, uint32_t out_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    /* Split from the capability check so a guest gets the same code here
     * as from wasm3: an out-of-range port is BAD_PORT, not "not authorized". */
    if (port > 0xFFFF) {
        return (uint32_t)WASMOS_ERR_IO_BAD_PORT;
    }
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    uint16_t* out = reinterpret_cast<uint16_t*>(warp_mem(ctx, out_off, sizeof(uint16_t)));
    if (!out) {
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    }
    *out = inw((uint16_t)port);
    return 0;
}
static uint32_t warp_io_in32(uint32_t port, uint32_t out_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t context_id = 0;
    /* Split from the capability check so a guest gets the same code here
     * as from wasm3: an out-of-range port is BAD_PORT, not "not authorized". */
    if (port > 0xFFFF) {
        return (uint32_t)WASMOS_ERR_IO_BAD_PORT;
    }
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    uint32_t* out = reinterpret_cast<uint32_t*>(warp_mem(ctx, out_off, sizeof(uint32_t)));
    if (!out) {
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    }
    *out = (uint32_t)inl((uint16_t)port);
    return 0;
}

static uint32_t warp_io_out8(uint32_t port, uint32_t val, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    /* Split from the capability check so a guest gets the same code here
     * as from wasm3: an out-of-range port is BAD_PORT, not "not authorized". */
    if (port > 0xFFFF) {
        return (uint32_t)WASMOS_ERR_IO_BAD_PORT;
    }
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    outb((uint16_t)port, (uint8_t)val);
    return 0;
}
static uint32_t warp_io_out16(uint32_t port, uint32_t val, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    /* Split from the capability check so a guest gets the same code here
     * as from wasm3: an out-of-range port is BAD_PORT, not "not authorized". */
    if (port > 0xFFFF) {
        return (uint32_t)WASMOS_ERR_IO_BAD_PORT;
    }
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    outw((uint16_t)port, (uint16_t)val);
    return 0;
}
static uint32_t warp_io_out32(uint32_t port, uint32_t val, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    /* Split from the capability check so a guest gets the same code here
     * as from wasm3: an out-of-range port is BAD_PORT, not "not authorized". */
    if (port > 0xFFFF) {
        return (uint32_t)WASMOS_ERR_IO_BAD_PORT;
    }
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, (uint16_t)port) != 0) {
        return (uint32_t)WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    outl((uint16_t)port, (uint32_t)val);
    return 0;
}
/* hostcalls.yaml `io_wait`: a short bus delay (a dummy access to port 0x80).  Gated on
 * the io.port capability for port 0x80, so a driver that was not granted it gets
 * WASMOS_ERR_IO_NOT_AUTHORIZED; returns 0 otherwise. */
static uint32_t warp_io_wait(void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 ||
        warp_require_io_capability(context_id, 0x80u) != 0) {
        return (uint32_t)WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    io_wait();
    return 0;
}

// ---------------------------------------------------------------------------
// ACPI / boot info
// ---------------------------------------------------------------------------

static const boot_info_t* g_warp_boot_info = nullptr;

/* hostcalls.yaml `acpi_rsdp_info`: copy the ACPI RSDP blob handed over at boot into
 * guest memory at `out_off` and store its byte length at `out_len_off`.  The blob is
 * copied whole or not at all — a blob longer than `max_len` is WASMOS_ERR_KERNEL_TOO_LARGE,
 * never a truncated copy.  Returns 0 on success, WASMOS_INVAL for a non-positive
 * `max_len`, WASMOS_NOENT when no RSDP was handed over, or
 * WASMOS_ERR_KERNEL_BAD_POINTER for an output range outside linear memory. */
static uint32_t warp_acpi_rsdp_info(uint32_t out_off, uint32_t out_len_off, uint32_t max_len,
                                    void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)max_len <= 0)
        return (uint32_t)WASMOS_INVAL;
    if (!g_warp_boot_info || !g_warp_boot_info->rsdp || !g_warp_boot_info->rsdp_length)
        return (uint32_t)WASMOS_NOENT; /* no ACPI RSDP was handed over at boot */
    uint32_t len = g_warp_boot_info->rsdp_length;
    if (len > max_len)
        return (uint32_t)WASMOS_ERR_KERNEL_TOO_LARGE;
    /* warp_mem uses getLinearMemoryRegion(offset, size), which triggers probe()
     * → ensureLinearSize() BEFORE the write — so zeroing happens first. */
    uint8_t* out = warp_mem(ctx, out_off, len);
    uint32_t* out_len = reinterpret_cast<uint32_t*>(warp_mem(ctx, out_len_off, sizeof(uint32_t)));
    if (!out || !out_len)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    __builtin_memcpy(out, g_warp_boot_info->rsdp, len);
    *out_len = len;
    return 0;
}

/* hostcalls.yaml `boot_module_name`: copy the name of boot module `index` into guest
 * memory at `out_off`, NUL-terminated and truncated to `out_len - 1` bytes.  Returns the
 * module's TRUE name length, which may exceed what was written — that is how a caller
 * detects truncation — or WASMOS_INVAL, WASMOS_NOENT for no such module, or
 * WASMOS_ERR_KERNEL_BAD_POINTER. */
static uint32_t warp_boot_module_name(uint32_t index, uint32_t out_off, uint32_t out_len,
                                      void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)index < 0 || (int32_t)out_len <= 0)
        return (uint32_t)WASMOS_INVAL;
    if (!g_warp_boot_info)
        return (uint32_t)WASMOS_NOENT;
    if (index >= g_warp_boot_info->module_count)
        return (uint32_t)WASMOS_NOENT; /* no module at that index */
    const boot_module_t* mod = static_cast<const boot_module_t*>(g_warp_boot_info->modules) + index;
    uint32_t true_len = 0;
    uint32_t copy_len = 0;
    wasmos_error_code_t clamp_rc = hostcall_name_clamp(
        mod->name, (uint32_t)__builtin_strlen(mod->name) + 1u, out_len, &true_len, &copy_len);
    if (clamp_rc != WASMOS_OK)
        return (uint32_t)clamp_rc;
    uint8_t* out = warp_mem(ctx, out_off, out_len);
    if (!out)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    __builtin_memcpy(out, mod->name, copy_len);
    out[copy_len] = '\0';
    /* The TRUE length, matching wasm3, so truncation stays detectable. */
    return true_len;
}

/* hostcalls.yaml `sync_user_read`: touch `len` bytes of guest memory at `ptr_off` with
 * volatile reads so the range is committed and its mapping resolved.  Nothing is
 * written and no data is returned.  Returns 0 on success and for len == 0, or
 * WASMOS_ERR_KERNEL_BAD_POINTER when the range leaves linear memory. */
static uint32_t warp_sync_user_read(uint32_t ptr_off, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!len)
        return 0;
    uint8_t* p = warp_mem(ctx, ptr_off, len);
    if (!p)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
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
    if (warp_current_context_id(&context_id) != 0) {
        return (uint32_t)WASMOS_ERR_KERNEL_NO_CALLER;
    }
    if (warp_require_system_control_capability(context_id) != 0) {
        return (uint32_t)WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    }
    kernel_system_shutdown(WASMOS_SHUTDOWN_REASON_HALT);
}
/* hostcalls.yaml `system_reboot`: counterpart of warp_system_halt, same capability
 * gate and the same "returns only on refusal" contract. */
static uint32_t warp_system_reboot(void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) {
        return (uint32_t)WASMOS_ERR_KERNEL_NO_CALLER;
    }
    if (warp_require_system_control_capability(context_id) != 0) {
        return (uint32_t)WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    }
    kernel_system_shutdown(WASMOS_SHUTDOWN_REASON_REBOOT);
}

// ---------------------------------------------------------------------------
// Scheduler extras
// ---------------------------------------------------------------------------

static uint32_t warp_sched_ticks(void* ctx_) {
    (void)ctx_;
    /* See the wasm3 side: positive and wrapping at 2^31, because a negative
     * return is how this ABI spells "error". */
    return (uint32_t)hostcall_value_counter(timer_ticks());
}
/* hostcalls.yaml `proc_count`: number of processes currently alive.  A snapshot, valid
 * only at the moment of the call. */
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

/* Copy initfs directory entry `index` into *out, re-validating that the entry's
 * [offset, offset+size) stays inside the image.  Returns 0 on success, -1 for a null
 * `out`, a malformed image, an index past the end, or an entry whose extent escapes. */
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

/* hostcalls.yaml `initfs_entry_count`: number of entries in the boot initfs image.
 * Returns the count, WASMOS_ERR_FS_NO_IMAGE when no valid image was handed over, or the
 * hostcall_value_check code if the count cannot be expressed in the positive i32 range
 * this ABI reserves for values. */
static uint32_t warp_initfs_entry_count(void* ctx_) {
    (void)ctx_;
    const wasmos_initfs_header_t* hdr = nullptr;
    const uint8_t* base = nullptr;
    if (warp_initfs_header_get(&hdr, &base) != 0)
        return (uint32_t)WASMOS_ERR_FS_NO_IMAGE;
    wasmos_error_code_t rc = hostcall_value_check(hdr->entry_count);
    if (rc != WASMOS_OK)
        return (uint32_t)rc;
    return (uint32_t)hdr->entry_count;
}

/* hostcalls.yaml `initfs_entry_name`: copy entry `index`'s path into guest memory at
 * `out_off`, NUL-terminated and truncated to `out_len - 1` bytes.  Returns the TRUE
 * path length so truncation stays detectable, or WASMOS_INVAL /
 * WASMOS_ERR_FS_NOT_FOUND / WASMOS_ERR_KERNEL_BAD_POINTER. */
static uint32_t warp_initfs_entry_name(uint32_t index, uint32_t out_off, uint32_t out_len,
                                       void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)index < 0 || (int32_t)out_len <= 0)
        return (uint32_t)WASMOS_INVAL;
    wasmos_initfs_entry_t e;
    if (warp_initfs_entry_at(index, &e) != 0)
        return (uint32_t)WASMOS_ERR_FS_NOT_FOUND;
    uint32_t true_len = 0;
    uint32_t copy_len = 0;
    wasmos_error_code_t rc =
        hostcall_name_clamp(e.path, (uint32_t)sizeof(e.path), out_len, &true_len, &copy_len);
    if (rc != WASMOS_OK)
        return (uint32_t)rc;
    uint8_t* out = warp_mem(ctx, out_off, out_len);
    if (!out)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    __builtin_memcpy(out, e.path, copy_len);
    out[copy_len] = '\0';
    /* The TRUE length, matching wasm3: fs_init skips an entry whose reported
     * length does not fit its buffer, and that test cannot fire if the number
     * reported is the one that was made to fit. */
    return true_len;
}

/* hostcalls.yaml `initfs_entry_size`: payload length in bytes of entry `index`.
 * Returns the size, WASMOS_ERR_FS_NOT_FOUND for a bad index or malformed image, or the
 * hostcall_value_check code when the size does not fit the positive i32 value range. */
static uint32_t warp_initfs_entry_size(uint32_t index, void* ctx_) {
    (void)ctx_;
    wasmos_initfs_entry_t e;
    if ((int32_t)index < 0 || warp_initfs_entry_at(index, &e) != 0)
        return (uint32_t)WASMOS_ERR_FS_NOT_FOUND;
    wasmos_error_code_t rc = hostcall_value_check(e.size);
    if (rc != WASMOS_OK)
        return (uint32_t)rc;
    return (uint32_t)e.size;
}

/* hostcalls.yaml `initfs_entry_copy`: copy up to `len` bytes of entry `index` starting
 * at `offset` into guest memory at `out_off`.  Returns the number of bytes actually
 * copied, which is 0 at or past the entry's end and short for a trailing chunk — a
 * request longer than the remainder is clamped, not refused.  Errors are WASMOS_INVAL,
 * WASMOS_ERR_FS_NOT_FOUND and WASMOS_ERR_KERNEL_BAD_POINTER. */
static uint32_t warp_initfs_entry_copy(uint32_t index, uint32_t out_off, uint32_t len,
                                       uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)index < 0 || (int32_t)len <= 0 || (int32_t)offset < 0)
        return (uint32_t)WASMOS_INVAL;
    wasmos_initfs_entry_t e;
    if (warp_initfs_entry_at(index, &e) != 0)
        return (uint32_t)WASMOS_ERR_FS_NOT_FOUND;
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
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    const uint8_t* src = static_cast<const uint8_t*>(g_warp_boot_info->initfs) + e.offset + offset;
    __builtin_memcpy(out, src, copy_len);
    return copy_len; /* bytes copied, matches wasm3 which returns (int32_t)copy_len */
}

// ---------------------------------------------------------------------------
// Physical memory mapping into WASM linear memory
// ---------------------------------------------------------------------------

static uint32_t warp_phys_map(uint32_t phys_lo, uint32_t phys_hi, uint32_t size,
                              uint32_t wasm_offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if (!size)
        return (uint32_t)WASMOS_INVAL;
    if ((size & 0xFFF) || (wasm_offset & 0xFFF))
        return (uint32_t)WASMOS_ERR_KERNEL_UNALIGNED;
    uint64_t phys = ((uint64_t)phys_hi << 32) | (uint64_t)phys_lo;
    if (!phys)
        return (uint32_t)WASMOS_INVAL;
    uint8_t* lmem = warp_linear_mem_window(ctx, wasm_offset, size);
    if (!lmem)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    /* Copy, not remap.  WARP's host linear-memory base sits at a sub-page offset
     * inside its allocation (AllocHeader + basedata precede it), so base +
     * wasm_offset is not page-aligned even for a 4 KiB-aligned wasm_offset, and
     * paging_map_4k needs aligned VAs — a misaligned remap would read from the
     * wrong physical offset and clobber WARP's basedata metadata.
     *
     * The source is not read through the direct map either: the only caller
     * (acpi_bus) reads EfiACPIReclaimMemory pages, which need not be inside the
     * kernel's 512 MiB higher-half window.  Each source page is therefore mapped
     * onto the page-aligned kernel scratch page, copied out, and the scratch page
     * restored to its own frame.  g_phys_scratch is shared, and this sequence is
     * unsynchronised: it is safe only under the WARP single-CPU invariant
     * (warp/shim.cpp).
     *
     * The destination range is committed FIRST.  probe() calls
     * ActiveMemoryManager::ensureLinearSize(), which zero-fills everything
     * between the old and the new usable size; a probe that fired after the copy
     * would erase the data.  Probing the last byte commits the whole range, and
     * later probes inside it short-circuit (offset < usableLinMemBytes_). */
    ctx->module->getLinearMemoryRegion(wasm_offset + size - 1, 1);
    /* Re-fetch lmem after the probe (ensureCapacityForLinearSize inside
     * ensureLinearSize may have called syncBasedataStart, changing the base). */
    lmem = ctx->module->getLinearMemoryRegion(0, 0);
    if (!lmem)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
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
// Environment variables. The store itself is shared with the other runtime
// (src/kernel/kenv.c); what stays here is the guest-memory plumbing.
// ---------------------------------------------------------------------------

static uint32_t warp_env_get(uint32_t name_off, uint32_t name_len, uint32_t buf_off,
                             uint32_t buf_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || (int32_t)buf_len <= 0)
        return (uint32_t)WASMOS_INVAL;
    if (name_len >= KENV_KEY_MAX)
        return (uint32_t)WASMOS_ERR_ENV_TOO_LONG;
    const uint8_t* name = warp_mem(ctx, name_off, name_len);
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    if (!name || !buf)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    char local_name[KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';

    char local_val[KENV_VAL_MAX];
    uint32_t out_size = buf_len < KENV_VAL_MAX ? buf_len : KENV_VAL_MAX;
    uint32_t write_len = 0;
    wasmos_error_code_t rc = kenv_get(local_name, local_val, out_size, &write_len);
    if (rc != WASMOS_OK)
        return (uint32_t)rc;
    __builtin_memcpy(buf, local_val, write_len + 1u); /* kenv_get placed the NUL */
    return write_len;
}

/* hostcalls.yaml `env_set`: set the kernel environment variable named by the
 * `name_len` guest bytes at `name_off` to the `val_len` bytes at `val_off`.  Neither
 * range needs a NUL — the lengths are authoritative and the copies are terminated in
 * kernel scratch.  A `val_len` of 0 sets the empty string.  Names/values at or above
 * KENV_KEY_MAX / KENV_VAL_MAX are refused with WASMOS_ERR_ENV_TOO_LONG rather than
 * truncated; other returns are 0, WASMOS_INVAL, WASMOS_ERR_KERNEL_BAD_POINTER and
 * kenv_set's own code. */
static uint32_t warp_env_set(uint32_t name_off, uint32_t name_len, uint32_t val_off,
                             uint32_t val_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0 || (int32_t)val_len < 0)
        return (uint32_t)WASMOS_INVAL;
    if (name_len >= KENV_KEY_MAX || val_len >= KENV_VAL_MAX)
        return (uint32_t)WASMOS_ERR_ENV_TOO_LONG;
    const uint8_t* name = warp_mem(ctx, name_off, name_len);
    if (!name)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    char local_name[KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    char local_val[KENV_VAL_MAX];
    local_val[0] = '\0';
    if (val_len > 0) {
        const uint8_t* val = warp_mem(ctx, val_off, val_len);
        if (!val)
            return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
        __builtin_memcpy(local_val, val, val_len);
        local_val[val_len] = '\0';
    }
    return (uint32_t)kenv_set(local_name, local_val);
}

/* hostcalls.yaml `env_unset`: remove the variable named by the `name_len` guest bytes
 * at `name_off`.  Returns kenv_unset's code, or WASMOS_INVAL /
 * WASMOS_ERR_ENV_TOO_LONG / WASMOS_ERR_KERNEL_BAD_POINTER on argument failures. */
static uint32_t warp_env_unset(uint32_t name_off, uint32_t name_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)name_len <= 0)
        return (uint32_t)WASMOS_INVAL;
    if (name_len >= KENV_KEY_MAX)
        return (uint32_t)WASMOS_ERR_ENV_TOO_LONG;
    const uint8_t* name = warp_mem(ctx, name_off, name_len);
    if (!name)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    char local_name[KENV_KEY_MAX];
    __builtin_memcpy(local_name, name, name_len);
    local_name[name_len] = '\0';
    return (uint32_t)kenv_unset(local_name);
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

static int warp_require_dma_capability(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_DMA_BUFFER, 0);
}
/* Capability gates.  Each returns 0 when `context_id` may perform the action and
 * non-zero when it may not; callers translate that into the NOT_AUTHORIZED code of
 * their own domain.  policy_authorize consults the context's granted capabilities;
 * policy_require additionally refuses a context that has no policy at all, which is why
 * system control uses it. */
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

/* Registry of the linear-memory windows a process has had something mapped into, used
 * only to keep a later placement from overlapping an earlier one.  Three id namespaces
 * share it: small positive ids are real shmem ids, bit 31 marks a region/block window
 * and bit 30 an xfer-buffer overlay.  The table is a fixed PROCESS_MAX_COUNT*32 array;
 * when it is full a new window is simply not recorded, so it can no longer be seen by
 * the overlap check.  Unsynchronised — safe only under the WARP single-CPU invariant.
 *
 * warp_shmem_map_track: record or resize the (pid, id, offset) window.
 * warp_shmem_map_untrack: drop every window this pid holds under `id`.
 * warp_shmem_map_find: the first window matching (pid, id), or nullptr.
 * warp_shmem_map_overlaps: whether [offset, offset+size) intersects any window of pid. */
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

/* Bytes of linear memory WARP currently treats as live, read from the basedata field
 * that precedes the linear-memory base.  This is the committed frontier, not the
 * declared maximum, and it is the floor the window scan starts above.  Returns 0 when
 * the base cannot be resolved. */
static uint32_t warp_linear_memory_active_size(WarpCallContext* ctx) {
    uint8_t* base = warp_mem(ctx, 0, 0);
    if (!base) {
        return 0;
    }
    return *(uint32_t*)(void*)(base - Basedata::FromEnd::actualLinMemByteSize);
}

/* Undo a window remap: put ordinary backing back under [offset, offset+size) of the
 * caller's linear memory so the guest does not keep reading the unmapped object.  For a
 * dedicated-VA slot the original frames were released when the window was mapped and
 * cannot be re-derived, so FRESH ZEROED pages are committed instead — the previous
 * contents of that range are not preserved.  For direct-mapped linmem the frame is
 * re-derived from the VA and remapped, preserving contents.  Returns 0 on success, -1
 * for a zero size, an unresolvable or misaligned window, or a mapping failure. */
static int warp_restore_linear_window(WarpCallContext* ctx, uint32_t offset, uint32_t size) {
    if (!ctx || !ctx->module || size == 0) {
        return -1;
    }
    uint8_t* base = warp_linear_mem_window(ctx, offset, size);
    if (!base) {
        return -1;
    }
    uint64_t virt = addr_cast(uint64_t, base);
    /* A linmem VA-slot window is NOT in the direct map: its pages are scattered,
     * so virt - KERNEL_HIGHER_HALF_BASE is not the backing frame.  The slot
     * window remap replaced the slot's own mapping, so there is nothing to
     * restore -- re-deriving a phys here would remap foreign frames.  phys is
     * routed through warp_mem_alias_phys for the direct-mapped case. */
    uint64_t pages_n = ((uint64_t)size + 0xFFFULL) / 0x1000ULL;
    if (linmem_slot_contains(virt)) {
        /* A slot-backed window's original frames are scattered and were released
         * when the window was mapped (see warp_linmem_place_phys), so they cannot
         * be re-derived from the VA.  Re-commit fresh zeroed frames over the
         * range instead: the window lives in guarded, app-unused linmem, and a
         * fresh commit is exactly what the slot hands out initially.
         * TODO(linmem-window): preserving the original contents would need the
         * displaced frames recorded per map; WARP_SHMEM_MAP_SLOTS is
         * PROCESS_MAX_COUNT*32, so a per-slot frame array is too costly. */
        return linmem_slot_commit(virt, 0, pages_n);
    }
    if (virt < KERNEL_HIGHER_HALF_BASE || (virt & 0xFFFULL) != 0) {
        return -1;
    }
    for (uint64_t i = 0; i < pages_n; ++i) {
        uint64_t page_virt = virt + i * 0x1000ULL;
        uint64_t page_phys = warp_mem_alias_phys(page_virt);
        if (!page_phys) {
            return -1;
        }
        if (paging_map_4k(page_virt,
                          page_phys,
                          MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC) !=
            0) {
            return -1;
        }
    }
    return 0;
}

#ifdef WASMOS_WASM_RUNTIME_WARP
/* Re-publish the whole linear-memory allocation into the user root that is active
 * right now, so a ring-3 guest sees the kernel-side layout after it changed.  A no-op
 * returning 0 when the current root is the kernel root (a ring-0 call), which is what
 * makes it safe to call unconditionally.  Returns -1 on a null base or a mapping
 * failure. */
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

/* Narrower counterpart of warp_ring3_sync_linmem_user_window: re-publish only the
 * pages spanned by guest range [wasm_off, wasm_off+size) into the active user root, at
 * WARP_R3_LINMEM_BASE plus the base's sub-page offset plus wasm_off.  Used after the
 * kernel writes into guest memory through its own alias, so the ring-3 view is not
 * stale.  Returns 0 (including the ring-0 no-op case) or -1. */
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
        if (paging_map_4k_in_root(current_root,
                                  user_page_base + i * 0x1000ULL,
                                  phys_page,
                                  MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                      MEM_REGION_FLAG_USER) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Publish `pages` frames starting at `phys_base` into the active user root at the guest
 * address corresponding to `wasm_off`, i.e. mirror a kernel-side window remap into
 * ring 3.  `linmem_base` supplies only the sub-page offset of the linear-memory base.
 * Returns 0 (including the ring-0 no-op case) or -1 on a mapping failure. */
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
        if (paging_map_4k_in_root(current_root,
                                  user_va + i * 0x1000ULL,
                                  phys_base + i * 0x1000ULL,
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

/* hostcalls.yaml `xfer_buffer_borrow` / `_reborrow` / `_release` / `_unborrow`: the
 * transfer-kind spellings of the generic buffer calls above, with `kind` fixed to
 * BUFFER_KIND_TRANSFER.  Arguments, rights and return codes are those of the generic
 * form; borrow grants from the owner, reborrow sub-grants from a borrower, release
 * drops ownership and unborrow is the LENDER withdrawing a grant it made. */
static uint32_t warp_xfer_buffer_borrow(uint32_t grantee_endpoint, uint32_t buffer_id,
                                        uint32_t flags, void* ctx_) {
    return warp_buffer_borrow(
        (uint32_t)BUFFER_KIND_TRANSFER, grantee_endpoint, buffer_id, flags, ctx_);
}

static uint32_t warp_xfer_buffer_reborrow(uint32_t grantee_endpoint, uint32_t borrow_id,
                                          uint32_t flags, void* ctx_) {
    return warp_buffer_reborrow(
        (uint32_t)BUFFER_KIND_TRANSFER, grantee_endpoint, borrow_id, flags, ctx_);
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
        return (uint32_t)WASMOS_INVAL;
    uint8_t* raw = warp_mem(ctx, out_off, sizeof(cpu_stats_t));
    if (!raw)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
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
        return (uint32_t)WASMOS_INVAL;
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    if (!buf)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    uint32_t pid = 0;
    const char* name = nullptr;
    if (process_info_at(index, &pid, &name) != 0)
        return (uint32_t)WASMOS_NOENT; /* no process at that index */
    uint32_t nlen = 0;
    if (name)
        while (name[nlen] && nlen + 1u < buf_len)
            nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

/* hostcalls.yaml `proc_info_ex`: warp_proc_info plus the parent pid, written as a u32
 * at guest offset `parent_off`.  Same enumeration and truncation rules; returns the pid
 * at `index`, or WASMOS_INVAL / WASMOS_NOENT / WASMOS_ERR_KERNEL_BAD_POINTER. */
static uint32_t warp_proc_info_ex(uint32_t index, uint32_t buf_off, uint32_t buf_len,
                                  uint32_t parent_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)buf_len <= 0)
        return (uint32_t)WASMOS_INVAL;
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    uint8_t* par = warp_mem(ctx, parent_off, sizeof(uint32_t));
    if (!buf || !par)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    uint32_t pid = 0, parent_pid = 0;
    const char* name = nullptr;
    if (process_info_at_ex(index, &pid, &parent_pid, &name) != 0)
        return (uint32_t)WASMOS_NOENT; /* no process at that index */
    __builtin_memcpy(par, &parent_pid, sizeof(parent_pid));
    uint32_t nlen = 0;
    if (name)
        while (name[nlen] && nlen + 1u < buf_len)
            nlen++;
    __builtin_memcpy(buf, name ? name : "", nlen);
    buf[nlen] = '\0';
    return pid;
}

/* hostcalls.yaml `proc_info_stats`: warp_proc_info_ex plus a fixed-layout statistics
 * record written at guest offset `stats_off`.  The struct declared here mirrors the
 * guest-side layout field for field; changing either side without the other silently
 * misreads the record.  Returns the pid at `index`, or WASMOS_INVAL / WASMOS_NOENT /
 * WASMOS_ERR_KERNEL_BAD_POINTER. */
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
        return (uint32_t)WASMOS_INVAL;
    uint8_t* buf = warp_mem(ctx, buf_off, buf_len);
    uint8_t* par = warp_mem(ctx, parent_off, sizeof(uint32_t));
    uint8_t* stp = warp_mem(ctx, stats_off, sizeof(wasm_proc_stats_t));
    if (!buf || !par || !stp)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    uint32_t pid = 0, parent_pid = 0;
    const char* name = nullptr;
    process_stats_t stats;
    if (process_info_at_stats(index, &pid, &parent_pid, &name, &stats) != 0)
        return (uint32_t)WASMOS_NOENT; /* no process at that index */
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

static uint32_t warp_thread_yield(void* ctx_) {
    (void)ctx_;
    process_yield(PROCESS_RUN_YIELDED);
    return 0;
}

// ---------------------------------------------------------------------------
// Shared memory
// ---------------------------------------------------------------------------

static uint32_t warp_shmem_create(uint32_t pages, uint32_t flags, void* ctx_) {
    (void)ctx_;
    if ((int32_t)pages <= 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ARGS;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    uint32_t id = 0;
    uint64_t phys = 0;
    uint32_t cflags = flags ? flags : (MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE);
    if (mm_shared_create(context_id, (uint64_t)pages, cflags, &id, &phys) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_MAP;
    (void)phys;
    return id;
}

/* hostcalls.yaml `klog_register_ring`: adopt the caller's OWNED transfer buffer `id` as
 * the kernel log ring, after which klog output is also published into that ringbuf.
 * `id` is a BUFFER_KIND_TRANSFER buffer_id, not a shared-memory id; ownership, physical
 * backing, page alignment and ringbuf validity are all checked inside
 * klog_register_ring, which takes no retain — the caller must keep the buffer alive for
 * as long as the kernel logs into it.  `notify_endpoint` receives the VT_IPC_KLOG_NOTIFY
 * doorbell and must belong to the caller; a non-positive value registers the ring without
 * one.  Returns klog_register_ring's result (0, or a bare -1 on any of those checks), or
 * WASMOS_INVAL / WASMOS_ERR_KERNEL_NO_CALLER. */
static uint32_t warp_klog_register_ring(uint32_t id, uint32_t notify_endpoint, void* ctx_) {
    (void)ctx_;
    if ((int32_t)id <= 0)
        return (uint32_t)WASMOS_INVAL;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_KERNEL_NO_CALLER;
    /* Ownership of the transfer buffer and of the doorbell endpoint is enforced
     * inside klog_register_ring, matching the wasm3 path. */
    uint32_t notify = ((int32_t)notify_endpoint > 0) ? notify_endpoint : 0u;
    return (uint32_t)klog_register_ring(context_id, id, notify);
}

/* hostcalls.yaml `shmem_grant` / `shmem_revoke`: give the process `target_pid` access
 * to the caller's shared region `id`, or take it away.  Both require the caller to hold
 * the DMA-buffer capability and to own the region.  The target is named by pid and
 * resolved to its context here, so a pid with no context is refused.  Returns
 * mm_shared_grant / mm_shared_revoke's code, or WASMOS_ERR_SHMEM_BAD_ID /
 * WASMOS_ERR_SHMEM_NO_CAP. */
static uint32_t warp_shmem_grant(uint32_t id, uint32_t target_pid, void* ctx_) {
    (void)ctx_;
    if ((int32_t)id <= 0 || (int32_t)target_pid <= 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ID;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    process_t* tgt = process_get(target_pid);
    if (!tgt || tgt->context_id == 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    return (uint32_t)mm_shared_grant(context_id, id, tgt->context_id);
}

static uint32_t warp_shmem_revoke(uint32_t id, uint32_t target_pid, void* ctx_) {
    (void)ctx_;
    if ((int32_t)id <= 0 || (int32_t)target_pid <= 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ID;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    process_t* tgt = process_get(target_pid);
    if (!tgt || tgt->context_id == 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    return (uint32_t)mm_shared_revoke(context_id, id, tgt->context_id);
}

/* hostcalls.yaml `shmem_map`: map shared region `id` at a guest offset the CALLER
 * chooses.  `wasm_off` must land on a page boundary once the linear-memory base's own
 * sub-page offset is added — WARP's base is not page-aligned, so a 4 KiB-aligned
 * `wasm_off` is not sufficient and a misfit is WASMOS_ERR_SHMEM_NO_WINDOW.  `size` must
 * be page-aligned and at least the region's size.  The range is committed by probe
 * BEFORE the remap, because a later commit would zero-fill the freshly mapped frames.
 * Returns 0 on success, otherwise a negative WASMOS_ERR_SHMEM_* code.  Prefer
 * warp_shmem_map_auto, which picks a placement that satisfies these constraints. */
static uint32_t warp_shmem_map(uint32_t id, uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0 || (size & 0xFFF))
        return (uint32_t)WASMOS_ERR_SHMEM_UNALIGNED;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0 || shared_pages == 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ID;
    if ((uint64_t)size < shared_pages * 0x1000ULL)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_SIZE;
    /* Commit the range via probe() BEFORE paging_map_4k (see warp_shmem_map_auto
     * for the rationale — ensureLinearSize would zero the shmem pages otherwise). */
    ctx->module->getLinearMemoryRegion(wasm_off + size - 1, 1);
    uint8_t* linmem_base = ctx->module->getLinearMemoryRegion(0, 0);
    if (!linmem_base)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_WINDOW;
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_sync_linmem_user_window(linmem_base) != 0) {
        return (uint32_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    }
#endif
    uint8_t* lmem = linmem_base + wasm_off;
    if (addr_cast(uint64_t, lmem) & 0xFFF)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    uint64_t virt = addr_cast(uint64_t, lmem);
    for (uint64_t i = 0; i < shared_pages; ++i) {
        paging_map_4k(virt + i * 0x1000ULL, phys_base + i * 0x1000ULL, 3ULL);
    }
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_map_user_window(linmem_base, wasm_off, phys_base, shared_pages) != 0) {
        return (uint32_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    }
#endif
    if (mm_shared_retain(context_id, id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_MAP;
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
        uint64_t page_virt = virt + i * 0x1000ULL;
        /* Slot-backed linmem: the frame this VA currently maps belongs to the
         * slot and is about to be replaced by the shared frame.  Release it here
         * or every map/unmap cycle leaks a page; the unmap path re-commits a
         * fresh one.  Direct-mapped linmem keeps its frame (the VA *is* the
         * frame), so nothing to release there. */
        if (linmem_slot_contains(page_virt)) {
            uint64_t old_phys = paging_virt_to_phys(page_virt);
            if (old_phys) {
                pfa_free_pages(old_phys & ~0xFFFULL, 1);
            }
        }
        paging_map_4k(page_virt, phys_base + i * 0x1000ULL, 3ULL);
    }
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (warp_ring3_map_user_window(linmem_base, found_off, phys_base, map_pages) != 0) {
        return -1;
    }
#endif
    return (int64_t)found_off;
}

/* hostcalls.yaml `shmem_map_auto`: map shared region `id` into a guest window the
 * KERNEL places, avoiding the alignment trap of warp_shmem_map.  `size` must be
 * page-aligned and at least the region's size.  Requires the DMA-buffer capability.
 * Returns the chosen guest offset (a value, not a status), or a negative
 * WASMOS_ERR_SHMEM_* code — WASMOS_ERR_SHMEM_NO_WINDOW when no free, aligned,
 * non-overlapping window of that size exists inside the app's reserved linear memory. */
static uint32_t warp_shmem_map_auto(uint32_t id, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0 || (int32_t)size <= 0) {
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ARGS;
    }
    /* Split from the argument check so a guest gets the same code here as from
     * wasm3: a misaligned size is UNALIGNED, not "bad arguments". */
    if ((size & 0xFFF) != 0) {
        return (uint32_t)WASMOS_ERR_SHMEM_UNALIGNED;
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
                (unsigned)ctx->pid,
                (unsigned long long)size,
                (unsigned long long)shared_pages,
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
        return (uint32_t)WASMOS_ERR_SHMEM_MAP;
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
    warp_shmem_map_track(
        ctx->pid, WARP_REGION_TRACK_ID(found_off), found_off, (uint32_t)region_bytes);
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
        return (uint32_t)WASMOS_ERR_KERNEL_NO_CALLER;
    auto* slot = warp_block_slot(ctx->pid);
    if (!slot)
        return (uint32_t)WASMOS_ERR_BLOCK_NO_SLOT;
    if (!slot->phys) {
        slot->phys = pfa_alloc_pages_below(WARP_BLOCK_BUF_PAGES, 512ULL * 1024 * 1024);
        if (!slot->phys)
            return (uint32_t)WASMOS_ERR_BLOCK_NO_BACKING;
    }
    if (slot->map_off)
        return slot->map_off;

    const uint32_t window = (uint32_t)(WARP_BLOCK_BUF_PAGES * 0x1000ULL);
    int64_t placed = warp_linmem_place_phys(ctx, slot->phys, WARP_BLOCK_BUF_PAGES, window);
    if (placed < 0)
        return (uint32_t)WASMOS_ERR_KERNEL_NO_WINDOW;
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
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    xfer_buffer_t buf;
    if (xfer_buffer_describe(buffer_id, BUFFER_KIND_TRANSFER, context_id, &buf) != WASMOS_ERR_NONE)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    xfer_buffer_owner_t owner;
    if (xfer_buffer_get_owned(&buf, context_id, &owner) != WASMOS_ERR_NONE)
        /* the overlay is the owner's private in-place view */
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    uint64_t phys_base = xfer_buffer_object_phys(&buf);
    if (phys_base == 0 || (phys_base & 0xFFFULL) != 0 || buf.size_bytes == 0)
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_NO_BACKING;
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
        return (uint32_t)WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
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

/* hostcalls.yaml `shmem_unmap`: drop the caller's reference to shared region `id` and
 * restore ordinary backing under whatever window it occupied.  For a dedicated-VA slot
 * the restored pages are fresh and ZEROED, so the window's previous contents do not
 * survive the unmap.  Unmapping an id that was never mapped still releases the
 * reference.  Returns mm_shared_release's code, or WASMOS_ERR_SHMEM_BAD_ARGS /
 * NO_CAP / NO_WINDOW. */
static uint32_t warp_shmem_unmap(uint32_t id, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)id <= 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ARGS;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    WarpShmemLinearMap* slot = warp_shmem_map_find(process_current_pid(), id);
    if (slot && warp_restore_linear_window(ctx, slot->offset, slot->size) != 0) {
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ARGS;
    }
#ifdef WASMOS_WASM_RUNTIME_WARP
    if (slot) {
        uint8_t* linmem_base = ctx->module->getLinearMemoryRegion(0, 0);
        if (!linmem_base || warp_ring3_sync_linmem_user_window(linmem_base) != 0) {
            return (uint32_t)WASMOS_ERR_SHMEM_NO_WINDOW;
        }
    }
#endif
    warp_shmem_map_untrack(process_current_pid(), id);
    return (uint32_t)mm_shared_release(context_id, id);
}

/* hostcalls.yaml `shmem_flush`: copy `size` bytes from guest memory at `wasm_off` into
 * the front of shared region `id`.  A copy, not a mapping operation — for a region that
 * is already mapped into linear memory this would copy the window onto itself.  `size`
 * may not exceed the region.  Returns 0 on success, otherwise a negative
 * WASMOS_ERR_SHMEM_* code. */
static uint32_t warp_shmem_flush(uint32_t id, uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    /* wasm_off arrives as u32, so a negative offset becomes a huge one. Reject
     * it as a bad argument here, as wasm3 does, rather than letting it fall
     * through to the window check and report NO_WINDOW. */
    if ((int32_t)id <= 0 || (int32_t)size <= 0 || (int32_t)wasm_off < 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ARGS;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0 || shared_pages == 0 ||
        phys_base == 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ID;
    if ((uint64_t)size > shared_pages * 0x1000ULL)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_SIZE;
    const uint8_t* src = warp_linear_mem_window(ctx, wasm_off, size);
    if (!src)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    __builtin_memcpy(ptr_cast(void, (phys_base | KERNEL_HIGHER_HALF_BASE)), src, size);
    return 0;
}

/* hostcalls.yaml `shmem_refresh`: the reverse of warp_shmem_flush — copy `size` bytes
 * from the front of shared region `id` into guest memory at `wasm_off`.  The
 * destination must already be committed; this call deliberately does not extend linear
 * memory (see the comment on the window lookup below).  Returns 0 on success, otherwise
 * a negative WASMOS_ERR_SHMEM_* code. */
static uint32_t warp_shmem_refresh(uint32_t id, uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    /* wasm_off arrives as u32, so a negative offset becomes a huge one. Reject
     * it as a bad argument here, as wasm3 does, rather than letting it fall
     * through to the window check and report NO_WINDOW. */
    if ((int32_t)id <= 0 || (int32_t)size <= 0 || (int32_t)wasm_off < 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ARGS;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_dma_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_CAP;
    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(context_id, id, &phys_base, &shared_pages) != 0 || shared_pages == 0 ||
        phys_base == 0)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_ID;
    if ((uint64_t)size > shared_pages * 0x1000ULL)
        return (uint32_t)WASMOS_ERR_SHMEM_BAD_SIZE;
    /* shmem_refresh writes into a region already committed by shmem_map_auto.
     * Use warp_linear_mem_window (size=0 probe, no ensureLinearSize) instead of
     * getLinearMemoryRegion with a non-zero size, which would trigger
     * ensureLinearSize and potentially extend the linear memory past its current
     * limit causing a page fault at the new boundary. */
    uint8_t* dst = warp_linear_mem_window(ctx, wasm_off, size);
    if (!dst)
        return (uint32_t)WASMOS_ERR_SHMEM_NO_WINDOW;
    __builtin_memcpy(dst, ptr_cast(const void, (phys_base | KERNEL_HIGHER_HALF_BASE)), size);
    return 0;
}

// ---------------------------------------------------------------------------
// IRQ routing
// ---------------------------------------------------------------------------

/* The ABI passes the line as i32, so a negative one arrives here above
 * INT32_MAX. Rejecting it first, as wasm3 does, is what keeps a guest from
 * getting "not authorized" for what is really a malformed line -- and stops
 * 0xFFFFFFFF reaching irq_register/irq_ack as a line number. */
static bool warp_irq_line_valid(uint32_t irq_line) {
    return irq_line <= 0x7FFFFFFFu;
}

/* hostcalls.yaml `irq_route_ipc`: deliver interrupts on `irq_line` as IPC messages to
 * `endpoint`.  Requires the IRQ capability.  Returns irq_register's code, or
 * WASMOS_ERR_IRQ_BAD_LINE / BAD_ENDPOINT / NOT_AUTHORIZED.  Each delivered interrupt
 * must be acknowledged with warp_irq_ack before the line is unmasked again. */
static uint32_t warp_irq_route_ipc(uint32_t irq_line, uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (!warp_irq_line_valid(irq_line))
        return (uint32_t)WASMOS_ERR_IRQ_BAD_LINE;
    if (endpoint > 0x7FFFFFFFu)
        return (uint32_t)WASMOS_ERR_IRQ_BAD_ENDPOINT;
    if (warp_current_context_id(&context_id) != 0 || warp_require_irq_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_IRQ_NOT_AUTHORIZED;
    return (uint32_t)irq_register(context_id, irq_line, endpoint);
}

/* hostcalls.yaml `irq_ack`: acknowledge the interrupt the caller was notified of.  Only
 * a context registered as a sharer of `irq_line` may ack, which the IRQ layer enforces
 * (WASMOS_ERR_IRQ_NOT_A_SHARER otherwise); the IRQ capability is deliberately NOT
 * re-checked, so a driver that routed a line can keep acking it.  A line shared by
 * several drivers is only unmasked once every sharer has acked, and a duplicate or late
 * ack is a benign 0 rather than an error.  Also returns WASMOS_ERR_IRQ_BAD_LINE, or
 * WASMOS_ERR_IRQ_NOT_AUTHORIZED when the caller has no context. */
static uint32_t warp_irq_ack(uint32_t irq_line, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (!warp_irq_line_valid(irq_line))
        return (uint32_t)WASMOS_ERR_IRQ_BAD_LINE;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_IRQ_NOT_AUTHORIZED;
    return (uint32_t)irq_ack(context_id, irq_line);
}

/* Configure an IRQ line's trigger/polarity (flags: bit0=level, bit1=active-low).
 * Gated by the IRQ capability; pci-bus uses it to mark PCI INTx lines
 * level/active-low. (TODO: split a dedicated irq.configure capability from
 * irq.route for tighter privilege separation — see docs/architecture/09.) */
static uint32_t warp_irq_configure(uint32_t irq_line, uint32_t flags, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_KERNEL_NO_CALLER;
    if (warp_require_irq_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    return (uint32_t)irq_configure(irq_line, flags);
}

/* hostcalls.yaml `irq_unroute`: stop delivering `irq_line` to the caller.  Requires the
 * IRQ capability.  Returns irq_unregister's code, or WASMOS_ERR_IRQ_BAD_LINE /
 * NOT_AUTHORIZED. */
static uint32_t warp_irq_unroute(uint32_t irq_line, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (!warp_irq_line_valid(irq_line))
        return (uint32_t)WASMOS_ERR_IRQ_BAD_LINE;
    if (warp_current_context_id(&context_id) != 0 || warp_require_irq_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_IRQ_NOT_AUTHORIZED;
    return (uint32_t)irq_unregister(context_id, irq_line);
}

/* Allocate an MSI vector bound to one of the caller's endpoints. The kernel owns
 * the vector namespace; the caller passes the returned address/data pair to the
 * bus driver that programs the device (pci-bus, PCI_IPC_MSI_BIND). */
static uint32_t warp_msi_alloc(uint32_t endpoint, uint32_t out_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    typedef struct {
        uint32_t address_lo;
        uint32_t address_hi;
        uint32_t data;
        uint32_t vector;
    } msi_desc_t;
    uint8_t* raw = warp_mem(ctx, out_off, sizeof(msi_desc_t));
    if (!raw)
        return (uint32_t)WASMOS_ERR_MSI_BAD_ENDPOINT;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_irq_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_MSI_NOT_AUTHORIZED;
    msi_desc_t tmp;
    int rc =
        msi_alloc(context_id, endpoint, &tmp.address_lo, &tmp.address_hi, &tmp.data, &tmp.vector);
    if (rc != 0)
        return (uint32_t)rc;
    __builtin_memcpy(raw, &tmp, sizeof(tmp));
    return 0;
}

/* hostcalls.yaml `msi_free`: return an MSI vector allocated by warp_msi_alloc.  The
 * caller must hold the IRQ capability and own the vector, which msi_free checks.
 * Returns msi_free's code or WASMOS_ERR_MSI_NOT_AUTHORIZED.  The device must already
 * have been programmed to stop using the vector; this only frees the kernel binding. */
static uint32_t warp_msi_free(uint32_t vector, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_irq_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_MSI_NOT_AUTHORIZED;
    return (uint32_t)msi_free(context_id, vector);
}

/* Poke a memory-mapped device register (pci-bus placing an MSI-X table entry).
 * mmio_write32_phys refuses anything overlapping system RAM, so the mmio.map
 * capability buys MMIO access, not arbitrary physical memory access. */
static uint32_t warp_mmio_write32(uint32_t phys_lo, uint32_t phys_hi, uint32_t value, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0 || warp_require_mmio_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_MSI_NOT_AUTHORIZED;
    uint64_t phys = ((uint64_t)phys_hi << 32) | (uint64_t)phys_lo;
    return (uint32_t)mmio_write32_phys(phys, value);
}

// ---------------------------------------------------------------------------
// Serial / input
// ---------------------------------------------------------------------------

static uint32_t warp_serial_register(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    return (uint32_t)serial_register_remote_driver(endpoint);
}

/* hostcalls.yaml `input_push`: append the low byte of `ch` to the kernel's serial input
 * queue, as a keyboard driver injecting a decoded character.  Higher bits are discarded.
 * Returns 0; a full queue is not reported. */
static uint32_t warp_input_push(uint32_t ch, void* ctx_) {
    (void)ctx_;
    serial_input_push((uint8_t)(ch & 0xFF));
    return 0;
}

/* hostcalls.yaml `input_read`: take one byte from the kernel's serial input queue.
 * Does not block.  Returns the byte (0..255), or WASMOS_AGAIN when nothing is queued —
 * an empty queue is a retry condition, not a failure. */
static uint32_t warp_input_read(void* ctx_) {
    (void)ctx_;
    uint8_t ch = 0;
    /* nothing queued is not a failure */
    return serial_input_read(&ch) ? (uint32_t)ch : (uint32_t)WASMOS_AGAIN;
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

static uint32_t warp_framebuffer_pixel(uint32_t x, uint32_t y, uint32_t color, void* ctx_) {
    (void)ctx_;
    return (uint32_t)framebuffer_put_pixel(x, y, color);
}

/* hostcalls.yaml `framebuffer_info`: write the framebuffer geometry record to guest
 * offset `out_off`.  `len` is the caller's buffer size and must be at least
 * sizeof(framebuffer_info_t), otherwise WASMOS_ERR_FRAMEBUFFER_TOO_SMALL; exactly
 * sizeof(framebuffer_info_t) bytes are written regardless of a larger `len`.  Returns 0
 * on success, or WASMOS_INVAL / framebuffer_get_info's code /
 * WASMOS_ERR_KERNEL_BAD_POINTER. */
static uint32_t warp_framebuffer_info(uint32_t out_off, uint32_t len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)len <= 0)
        return (uint32_t)WASMOS_INVAL;
    if ((int32_t)len < (int32_t)sizeof(framebuffer_info_t))
        return (uint32_t)WASMOS_ERR_FRAMEBUFFER_TOO_SMALL;
    framebuffer_info_t info;
    __builtin_memset(&info, 0, sizeof(info));
    wasmos_error_code_t info_rc = framebuffer_get_info(&info);
    if (info_rc != WASMOS_OK)
        return (uint32_t)info_rc;
    uint8_t* out = warp_mem(ctx, out_off, sizeof(framebuffer_info_t));
    if (!out)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    __builtin_memcpy(out, &info, sizeof(info));
    return 0;
}

/* hostcalls.yaml `framebuffer_map`: remap the guest window at `wasm_off` onto the
 * framebuffer's physical pages, so stores into linear memory go straight to the display.
 * `size` must be page-aligned and cover the whole framebuffer, and the window's host
 * address must land on a page boundary — the caller picks `wasm_off`, so a misfit is
 * WASMOS_ERR_KERNEL_UNALIGNED rather than a relocation.  Requires the MMIO-map
 * capability.  Returns 0 on success, otherwise a negative code.  The remap replaces the
 * previous backing of that window; nothing restores it. */
static uint32_t warp_framebuffer_map(uint32_t wasm_off, uint32_t size, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)size <= 0)
        return (uint32_t)WASMOS_INVAL;
    if (size & 0xFFF)
        return (uint32_t)WASMOS_ERR_KERNEL_UNALIGNED;
    framebuffer_info_t info;
    __builtin_memset(&info, 0, sizeof(info));
    wasmos_error_code_t info_rc = framebuffer_get_info(&info);
    if (info_rc != WASMOS_OK)
        return (uint32_t)info_rc;
    if (size < info.framebuffer_size)
        return (uint32_t)WASMOS_ERR_FRAMEBUFFER_TOO_SMALL;
    /* Split, so "no caller" and "not permitted to map MMIO" stay distinct. */
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)WASMOS_ERR_KERNEL_NO_CALLER;
    if (warp_require_mmio_capability(context_id) != 0)
        return (uint32_t)WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    uint8_t* lmem = warp_linear_mem_window(ctx, wasm_off, size);
    if (!lmem)
        return (uint32_t)WASMOS_ERR_KERNEL_NO_WINDOW;
    if (addr_cast(uint64_t, lmem) & 0xFFF)
        return (uint32_t)WASMOS_ERR_KERNEL_UNALIGNED;
    uint64_t virt = addr_cast(uint64_t, lmem);
    uint64_t phys = info.framebuffer_base;
    uint64_t pages = (uint64_t)size / 0x1000ULL;
    for (uint64_t i = 0; i < pages; ++i) {
        paging_unmap_4k(virt + i * 0x1000ULL);
        if (paging_map_4k(virt + i * 0x1000ULL, phys + i * 0x1000ULL, 3ULL) < 0)
            return (uint32_t)WASMOS_ERR_KERNEL_MAP_FAILED;
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
        return (uint32_t)WASMOS_NOENT;
    return (uint32_t)g_warp_boot_info->boot_config_size;
}

/* hostcalls.yaml `boot_config_copy`: copy exactly `len` bytes of the boot config blob
 * starting at `offset` into guest memory at `buf_off`.  Unlike initfs_entry_copy this
 * refuses a range that runs past the end (WASMOS_INVAL) instead of clamping it, and
 * returns 0 on success rather than a byte count.  WASMOS_NOENT when no boot config was
 * handed over, WASMOS_ERR_KERNEL_BAD_POINTER for a guest range outside linear memory. */
static uint32_t warp_boot_config_copy(uint32_t buf_off, uint32_t len, uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    /* The size check belongs here, as in wasm3: without it a zero-length boot
     * config reports success-and-nothing for offset 0 / len 0 instead of
     * "there is no boot config". */
    if (!g_warp_boot_info || !g_warp_boot_info->boot_config || !g_warp_boot_info->boot_config_size)
        return (uint32_t)WASMOS_NOENT;
    uint32_t total = (uint32_t)g_warp_boot_info->boot_config_size;
    if (offset > total || len > total - offset)
        return (uint32_t)WASMOS_INVAL;
    if (len == 0)
        return 0;
    uint8_t* dst = warp_mem(ctx, buf_off, len);
    if (!dst)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
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

/* hostcalls.yaml `initfs_find_path`: resolve the `path_len` guest bytes at `path_off`
 * to an initfs entry index.  Matching is ASCII case-insensitive; leading '/' and an
 * "init/" prefix are stripped, and an entry matches on either its full stored path or
 * its basename, so "/init/foo.wasm", "foo.wasm" and "dir/foo.wasm" can all resolve.
 * Returns the entry index, or WASMOS_INVAL, WASMOS_ERR_FS_PATH_TOO_LONG for 112 bytes
 * or more, WASMOS_ERR_FS_NO_IMAGE, WASMOS_ERR_FS_NOT_FOUND,
 * WASMOS_ERR_KERNEL_BAD_POINTER. */
static uint32_t warp_initfs_find_path(uint32_t path_off, uint32_t path_len, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    if ((int32_t)path_len <= 0)
        return (uint32_t)WASMOS_INVAL;
    if (path_len >= 112u)
        return (uint32_t)WASMOS_ERR_FS_PATH_TOO_LONG;
    const uint8_t* raw = warp_mem(ctx, path_off, path_len);
    if (!raw)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
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
        return (uint32_t)WASMOS_INVAL;
    const wasmos_initfs_header_t* hdr = nullptr;
    const uint8_t* base = nullptr;
    if (warp_initfs_header_get(&hdr, &base) != 0)
        return (uint32_t)WASMOS_ERR_FS_NO_IMAGE;
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
    return (uint32_t)WASMOS_ERR_FS_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Early log
// ---------------------------------------------------------------------------

static uint32_t warp_early_log_size(void* ctx_) {
    (void)ctx_;
    return (uint32_t)serial_early_log_size();
}

/* hostcalls.yaml `early_log_copy`: copy exactly `len` bytes of the pre-service serial
 * log starting at `offset` into guest memory at `buf_off`.  A range past the end is
 * refused with WASMOS_INVAL, not clamped.  Returns 0 on success and for len == 0, or
 * WASMOS_ERR_KERNEL_BAD_POINTER.  The log can still be appended to, so a size read
 * earlier may already be stale. */
static uint32_t warp_early_log_copy(uint32_t buf_off, uint32_t len, uint32_t offset, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    uint32_t total = (uint32_t)serial_early_log_size();
    if (offset > total || len > total - offset)
        return (uint32_t)WASMOS_INVAL;
    if (len == 0)
        return 0;
    uint8_t* dst = warp_mem(ctx, buf_off, len);
    if (!dst)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    serial_early_log_copy(dst, offset, len);
    return 0;
}

/* Diagnostics that the wasm3 backend implements and this one does not: a mark is
 * dropped and a kmap dump prints nothing.  They report success so a guest that
 * instruments itself runs identically under both runtimes. */
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
/* Stub, as recorded in abi/hostcalls.yaml (id 49): reports an empty ready queue
 * rather than the live count wasm3 returns, so a guest cannot use this to size
 * the runqueue under WARP.
 * TODO(warp-sched-stats): return process_ready_count() as the wasm3 side does. */
static uint32_t warp_sched_ready_count(void* ctx_) {
    (void)ctx_;
    return 0;
}
/* hostcalls.yaml `sched_cpu_count`: number of CPUs the kernel brought up. */
static uint32_t warp_sched_cpu_count(void* ctx_) {
    (void)ctx_;
    return (uint32_t)g_cpu_count;
}
/* hostcalls.yaml `kernel_runtime`: which WASM backend the guest is running under.
 * 1 = WARP; the wasm3 build of this call answers differently, and it is the supported
 * way for a guest to tell the two apart at runtime. */
static uint32_t warp_kernel_runtime(void* ctx_) {
    (void)ctx_;
    return 1u; /* WARP */
}

/* hostcalls.yaml `physmem_stats`: write {total_bytes, free_bytes} as two u64s at guest
 * offset `out_off`.  Returns 0 on success or WASMOS_ERR_KERNEL_BAD_POINTER.  A
 * snapshot: free_bytes can change before the guest reads it. */
static uint32_t warp_physmem_stats(uint32_t out_off, void* ctx_) {
    auto* ctx = warp_call_ctx(ctx_);
    typedef struct {
        uint64_t total_bytes;
        uint64_t free_bytes;
    } physmem_stats_t;
    uint8_t* raw = warp_mem(ctx, out_off, sizeof(physmem_stats_t));
    if (!raw)
        return (uint32_t)WASMOS_ERR_KERNEL_BAD_POINTER;
    physmem_stats_t tmp;
    tmp.total_bytes = pfa_total_bytes();
    tmp.free_bytes = pfa_free_bytes();
    __builtin_memcpy(raw, &tmp, sizeof(tmp));
    return 0;
}

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

/* WASMOS_SYMBOLS(LINK) is generated from abi/hostcalls.yaml
 * (scripts/gen_abi_hostcalls.py) and expands the whole host-call list through
 * the caller-supplied LINK macro, so no table can drift out of sync with the
 * IDL.  Table position is the ring-3 host-call id.
 *
 * The kernel instantiates it exactly once, through R3_LINK below, producing
 * Linkage::DYNAMIC entries.  A STATIC entry would bake kernel function pointers
 * into the compiled code, which ring-3 guests cannot call and which
 * initFromCompiledBinary — the tail of every module init path — rejects with
 * Wrong_type. */
#include "wasmos_symbols_warp.inc"

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

/* Contracts for warp_bind_module / warp_context_for_pid / warp_ctx_release_pid are on
 * their declarations in warp/link.h. */
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

/* process_reap() calls both backends' release hooks unconditionally; in a WARP kernel
 * no wasm3 per-pid state exists, so this one does nothing. */
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
    return warp_ring3_dispatch_table(
        hc_id, frame->rdi, frame->rsi, frame->rdx, frame->rcx, frame->r8, frame->r9, user_rsp);
}
#endif /* WASMOS_WASM_RUNTIME_WARP */

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

/* Contract on the declaration in warp/link.h. */
void warp_link_init(const boot_info_t* boot_info) {
    g_warp_boot_info = boot_info;
    warp_ipc_slots_init();
    /* Pre-sized to comfortably exceed the typical live-process count so the
     * spawn hot path does not trigger a rehash (which would malloc under the
     * preempt-guard drain). It still grows automatically if exceeded. */
    hashmap_init(&g_ctx_map, sizeof(WarpCallContext), 64);
}

#ifdef WASMOS_WASM_RUNTIME_WARP
/* Re-publish `pid`'s whole linear-memory allocation into `user_root`.  Called from the
 * context-switch path so a ring-3 WARP process resuming on any CPU sees a linear memory
 * that grew or was remapped while it was descheduled.  Unlike
 * warp_ring3_sync_linmem_user_window the root is named explicitly rather than taken
 * from the current CR3, because the target address space is not yet active.  Returns 0
 * on success, -1 for a zero pid or root, an unbound pid, or a mapping failure. */
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
