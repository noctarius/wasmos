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
 * TODO: implement the remaining ~80 imports listed at the bottom of this file.
 */

#include <cstdint>
#include <cstring>
#include <array>

extern "C" {
#include "boot.h"
#include "ipc.h"
#include "process.h"
#include "thread.h"
#include "futex.h"
#include "klog.h"
#include "futex.h"
}

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/NativeSymbol.hpp"
#include "src/core/common/Span.hpp"
#include "src/core/common/function_traits.hpp"
#include "link.h"

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

static inline uint8_t *
warp_mem(WarpCallContext *ctx, uint32_t offset, uint32_t size)
{
    if (!ctx || !ctx->module) return nullptr;
    return ctx->module->getLinearMemoryRegion(offset, size);
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

static void
warp_ipc_slots_init(void)
{
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_ipc_last[i].pid   = 0;
        g_ipc_last[i].valid = 0;
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

// ---------------------------------------------------------------------------
// Host call wrappers — IPC
// ---------------------------------------------------------------------------

static uint32_t
warp_ipc_create_endpoint(void *ctx_)
{
    auto *ctx = static_cast<WarpCallContext *>(ctx_);
    uint32_t context_id = 0, endpoint = IPC_ENDPOINT_NONE;
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
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    WarpIpcLastSlot *slot = warp_ipc_slot_for_pid(context_id);
    if (!slot) return (uint32_t)-1;
    int rc = ipc_recv_blocking_for(context_id, endpoint, &slot->message);
    if (rc != IPC_OK) return (uint32_t)-1;
    slot->pid   = context_id;
    slot->valid = 1;
    return endpoint; /* wasm3 returns the ready endpoint; we return the one we waited on */
}

static uint32_t
warp_ipc_drain(uint32_t endpoint, void *ctx_)
{
    (void)ctx_;
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    WarpIpcLastSlot *slot = warp_ipc_slot_for_pid(context_id);
    if (!slot) return (uint32_t)-1;
    int rc = ipc_recv_for(context_id, endpoint, &slot->message);
    if (rc == IPC_EMPTY) return 0;
    if (rc != IPC_OK)   return (uint32_t)-1;
    slot->pid   = context_id;
    slot->valid = 1;
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
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return 0;
    WarpIpcLastSlot *slot = warp_ipc_slot_for_pid(context_id);
    if (!slot || !slot->valid) return 0;
    switch (field) {
        case 0: return slot->message.type;
        case 1: return slot->message.request_id;
        case 2: return slot->message.arg0;
        case 3: return slot->message.arg1;
        case 4: return slot->message.arg2;
        case 5: return slot->message.arg3;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Host call wrappers — console
// ---------------------------------------------------------------------------

static uint32_t
warp_console_write(uint32_t buf_offset, uint32_t len, void *ctx_)
{
    auto *ctx = static_cast<WarpCallContext *>(ctx_);
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
    auto *ctx = static_cast<WarpCallContext *>(ctx_);
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    (void)ctx;
    return (uint32_t)futex_wait(addr_off, val, timeout_ms, context_id);
}

static uint32_t
warp_futex_wake(uint32_t addr_off, uint32_t count, void *ctx_)
{
    auto *ctx = static_cast<WarpCallContext *>(ctx_);
    uint32_t context_id = 0;
    if (warp_current_context_id(&context_id) != 0) return (uint32_t)-1;
    (void)ctx;
    return (uint32_t)futex_wake(addr_off, count, context_id);
}

// ---------------------------------------------------------------------------
// NativeSymbol tables
// ---------------------------------------------------------------------------

static auto g_wasmos_symbols = make_array(
    STATIC_LINK("wasmos", "ipc_create_endpoint", warp_ipc_create_endpoint),
    STATIC_LINK("wasmos", "ipc_endpoint_owner",  warp_ipc_endpoint_owner),
    STATIC_LINK("wasmos", "ipc_send",             warp_ipc_send),
    STATIC_LINK("wasmos", "ipc_select_one",       warp_ipc_select_one),
    STATIC_LINK("wasmos", "ipc_recv",             warp_ipc_select_one),  // legacy alias
    STATIC_LINK("wasmos", "ipc_drain",            warp_ipc_drain),
    STATIC_LINK("wasmos", "ipc_try_recv",         warp_ipc_drain),       // legacy alias
    STATIC_LINK("wasmos", "ipc_notify",           warp_ipc_notify),
    STATIC_LINK("wasmos", "ipc_last_field",       warp_ipc_last_field),
    STATIC_LINK("wasmos", "console_write",        warp_console_write),
    STATIC_LINK("wasmos", "proc_exit",            warp_proc_exit),
    STATIC_LINK("wasmos", "proc_notify_ready",    warp_proc_notify_ready),
    STATIC_LINK("wasmos", "sched_yield",          warp_sched_yield),
    STATIC_LINK("wasmos", "sched_current_pid",    warp_sched_current_pid),
    STATIC_LINK("wasmos", "thread_gettid",        warp_thread_gettid),
    STATIC_LINK("wasmos", "futex_wait",           warp_futex_wait),
    STATIC_LINK("wasmos", "futex_wake",           warp_futex_wake)
    // TODO: ipc_select_{create,add,wait,destroy}
    // TODO: fs_buffer_{borrow,release,size,copy,write}, fs_endpoint
    // TODO: buffer_{borrow,release}, dma_{map,sync,unmap}_borrow
    // TODO: proc_{count,info,info_ex,info_stats}
    // TODO: sched_{ticks,ready_count,cpu_count,cpu_stats}
    // TODO: thread_{create,yield,exit,join,detach}
    // TODO: shmem_{create,grant,revoke,map,map_auto,flush,refresh,unmap}
    // TODO: irq_{route_ipc,ack,unroute}, serial_register
    // TODO: input_{push,read}, framebuffer_{info,map,pixel}, phys_map
    // TODO: block_buffer_{phys,copy,write}
    // TODO: boot_{module_name,config_size,config_copy}
    // TODO: initfs_{entry_count,entry_name,entry_size,entry_copy,find_path}
    // TODO: early_log_{size,copy}, env_{get,set,unset}
    // TODO: io_{in8,in16,in32,out8,out16,out32,wait}
    // TODO: system_{halt,reboot}, acpi_rsdp_info
    // TODO: kmap_dump, kmap_dump_all, debug_mark, sync_user_read, console_read
);

// Symbol accessor — called from the future warp_driver.cpp when compiling a
// module via module.initFromBytecode(bytecode, warp_wasmos_symbols(), true).
vb::Span<vb::NativeSymbol const>
warp_wasmos_symbols(void)
{
    return vb::Span<vb::NativeSymbol const>(
        g_wasmos_symbols.data(), g_wasmos_symbols.size());
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

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

void
warp_link_init(const boot_info_t *boot_info)
{
    warp_ipc_slots_init();
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_ctx_table[i] = WarpCallContext{nullptr, i, boot_info};
    }
}

} // extern "C"
