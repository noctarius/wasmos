/* warp_driver.cpp - WARP JIT-backed WASM module loader and driver runner.
 *
 * Mirrors wasm_driver.c but uses vb::WasmModule (single-pass JIT compiler).
 * The public API is identical so callers (kernel.c, wasm_chardev.c,
 * process_manager.cpp) are unchanged.
 *
 * Exception strategy: instead of try/catch (which requires a working C++
 * unwinder), we use the per-CPU warp_exception_checkpoint from cxx_abi.cpp.
 * The WARP_CALL macro sets a __builtin_setjmp checkpoint before each WARP API
 * call; __cxa_throw longjmps directly back to that checkpoint so the exception
 * never propagates through kernel call frames. */

#include <cstdint>
#include <new>

extern "C" {
#include "wasm_driver.h"
#include "wasm_chardev.h"
#include "klog.h"
#include "serial.h"
#include "process.h"
#include "thread.h"
#include "spinlock.h"
#include "ipc.h"
#include "string.h"
#include "paging.h"
#ifdef WASMOS_WARP_RING3
#include "warp_ring3.h"
#include "arch/x86_64/smp.h"
#endif
}

#define private public
#include "src/WasmModule/WasmModule.hpp"
#undef private
#include "src/core/runtime/MemoryHelper.hpp"
#include "src/core/common/Span.hpp"
#include "src/core/common/ILogger.hpp"
#include "warp/shim.h"
#include "warp/link.h"

#ifdef WASMOS_WARP_RING3
extern "C" {
/* from mem_utils_kernel.cpp */
int      warp_mem_ring3_map_jit(uint64_t user_root,
                                uint8_t const *jit_ptr, size_t jit_size);
int      warp_mem_ring3_map_linmem(uint64_t user_root,
                                   uint8_t const *linmem_ptr);
uint64_t warp_mem_linmem_basedata_length(uint8_t const *linmem_ptr);
}
/* basedataoffsets.hpp gives us BD::FromEnd::jobMemoryDataPtrPtr at compile time
 * without modifying any WARP source. */
#include "src/core/common/basedataoffsets.hpp"
namespace BD = Basedata;

/* Forward declarations for static ring-3 helpers defined later in this file. */
static void warp_r3_patch_basedata(uint8_t *linmem_kernel_ptr, uint64_t basedataLength,
                                   uint64_t stack_phys, uint64_t r3_linmem_base,
                                   uint64_t jit_kernel_base, uint64_t jit_size,
                                   uint64_t custom_ctx, uint64_t runtime_ptr);
static int  warp_r3_call_export(vb::WasmModule *mod, const char *name,
                                uint32_t argc, const uint32_t *argv,
                                uint64_t user_root, uint64_t stack_phys,
                                uint64_t r3_linmem_base);
#endif

// ---------------------------------------------------------------------------
// Exception checkpoint helpers (defined in cxx_abi.cpp)
// ---------------------------------------------------------------------------

struct WarpExceptionCheckpoint {
    void *jbuf[5];
    int   active;
};
extern "C" WarpExceptionCheckpoint *warp_exception_get_checkpoint(void);

/* WARP_CALL(retval_on_exception, expression):
 *   - Sets a per-CPU setjmp checkpoint.
 *   - Evaluates expression.
 *   - If __cxa_throw fires, execution resumes after the macro with the given
 *     return value. */
#define WARP_CALL(err_ret, expr)                                \
    do {                                                        \
        WarpExceptionCheckpoint *_ckpt =                       \
            warp_exception_get_checkpoint();                    \
        _ckpt->active = 1;                                      \
        if (__builtin_setjmp(_ckpt->jbuf)) {                    \
            klog_write("[warp-driver] WARP exception caught\n");\
            _ckpt->active = 0;                                  \
            return (err_ret);                                   \
        }                                                       \
        (expr);                                                 \
        _ckpt->active = 0;                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* Stack fence for WARP's ACTIVE_STACK_OVERFLOW_CHECK signed jle.
 * Kernel RSP is ~0xFFFFFFFF8... (signed ~-2GB).
 * 0xFFFFFFFF00000000 (signed ~-4GB) is always < kernel RSP, so jle never fires. */
static const uint8_t *const k_stack_fence =
    reinterpret_cast<const uint8_t *>(0xFFFFFFFF00000000ULL);

static inline vb::WasmModule *
module_of(wasm_driver_t *d) noexcept
{
    return static_cast<vb::WasmModule *>(d->wasm_module);
}

/* ILogger routing WARP diagnostics through klog_write. */
namespace {
class KernelLogger final : public vb::ILogger {
public:
    KernelLogger &operator<<(char const *const msg) override {
        if (msg) klog_write(msg);
        return *this;
    }
    KernelLogger &operator<<(const vb::Span<char const> &msg) override {
        if (msg.data() && msg.size() > 0) {
            /* Span may not be null-terminated; copy to a stack buffer. */
            char buf[128];
            size_t n = msg.size() < 127 ? msg.size() : 127;
            __builtin_memcpy(buf, msg.data(), n);
            buf[n] = '\0';
            klog_write(buf);
        }
        return *this;
    }
    KernelLogger &operator<<(uint32_t const v) override {
        serial_write_hex64(v); return *this;
    }
};
static KernelLogger g_logger;
} // namespace

// ---------------------------------------------------------------------------
// Thread-slot table (mirrors wasm_driver.c)
// ---------------------------------------------------------------------------

#define WASM_DRIVER_THREAD_SLOTS 64u

typedef struct {
    uint8_t in_use;
    uint32_t owner_pid;
    const uint8_t *module_bytes;
    uint32_t module_size;
    uint32_t stack_size;
    uint32_t heap_size;
    char export_name[64];
    uint32_t argc;
    uint32_t argv[4];
} warp_driver_thread_slot_t;

static warp_driver_thread_slot_t g_thread_slots[WASM_DRIVER_THREAD_SLOTS];
static spinlock_t g_thread_slots_lock;

static warp_driver_thread_slot_t *
thread_slot_alloc(void)
{
    spinlock_lock(&g_thread_slots_lock);
    for (uint32_t i = 0; i < WASM_DRIVER_THREAD_SLOTS; ++i) {
        if (!g_thread_slots[i].in_use) {
            g_thread_slots[i].in_use = 1;
            spinlock_unlock(&g_thread_slots_lock);
            return &g_thread_slots[i];
        }
    }
    spinlock_unlock(&g_thread_slots_lock);
    return nullptr;
}

static void
thread_slot_free(warp_driver_thread_slot_t *slot)
{
    if (!slot) return;
    spinlock_lock(&g_thread_slots_lock);
    slot->in_use = 0;
    spinlock_unlock(&g_thread_slots_lock);
}

static int
warp_driver_start_module(vb::WasmModule *mod, uint64_t user_root)
{
    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
    ckpt->active = 1;
    if (__builtin_setjmp(ckpt->jbuf) != 0) {
        klog_write("[warp-driver] exception during start\n");
        ckpt->active = 0;
        return -1;
    }
#ifdef WASMOS_WARP_RING3
    /* Switch to the WARP user CR3 so ring-0 JIT code can reach trampoline VAs
     * (DYNAMIC_LINK) when start() calls any imported functions. */
    uint64_t old_cr3 = 0;
    if (user_root) {
        old_cr3 = paging_get_current_root_table();
        paging_switch_root(user_root);
    }
#else
    (void)user_root;
#endif
    mod->start(k_stack_fence);
#ifdef WASMOS_WARP_RING3
    if (old_cr3) paging_switch_root(old_cr3);
#endif
    ckpt->active = 0;
    return 0;
}

static int
warp_driver_ensure_started(wasm_driver_t *driver, uint64_t user_root, uint64_t stack_phys)
{
    if (!driver || !driver->active || !driver->wasm_module) {
        return -1;
    }
    if (driver->started) {
        return 0;
    }
    if (warp_driver_start_module(module_of(driver), user_root) != 0) {
        return -1;
    }
    driver->started = 1;
#ifdef WASMOS_WARP_RING3
    if (user_root) {
        vb::WasmModule *mod = module_of(driver);
        vb::Span<uint8_t const> compiled = mod->getCompiledBinary();
        /* Prime the first WASM page so ring-3 wrappers do not immediately trap
         * on the initial 0x1000/0x1004 stacktrace bookkeeping probes. */
        (void)mod->getLinearMemoryRegion(0xFFFFu, 1u);
        uint8_t *linmem = mod->getLinearMemoryRegion(0, 0);
        driver->r3_linmem_base =
            WARP_R3_LINMEM_BASE + (reinterpret_cast<uint64_t>(linmem) & 0xFFFULL);
        if (warp_mem_ring3_map_linmem(user_root, linmem) != 0) {
            return -1;
        }
        uint64_t bd_len = warp_mem_linmem_basedata_length(linmem);
        warp_r3_patch_basedata(linmem, bd_len, stack_phys, driver->r3_linmem_base,
                               reinterpret_cast<uint64_t>(compiled.data()),
                               static_cast<uint64_t>(compiled.size()),
                               reinterpret_cast<uint64_t>(mod->getContext()),
                               reinterpret_cast<uint64_t>(&mod->runtime_));
    }
#else
    (void)stack_phys;
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// Call helper — invokes an exported function with 0-4 i32 args
// ---------------------------------------------------------------------------

static int
call_export_mod(vb::WasmModule *mod, const char *name,
                uint32_t argc, const uint32_t *argv,
                uint64_t user_root = 0, uint64_t stack_phys = 0)
{
#ifdef WASMOS_WARP_RING3
    if (user_root)
        return warp_r3_call_export(mod, name, argc, argv, user_root, stack_phys,
                                   WARP_R3_LINMEM_BASE + (reinterpret_cast<uint64_t>(
                                       mod->getLinearMemoryRegion(0, 0)) & 0xFFFULL));
#else
    (void)user_root; (void)stack_phys;
#endif
    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
    ckpt->active = 1;
    if (__builtin_setjmp(ckpt->jbuf)) {
        klog_write("[warp-driver] exception during export call\n");
        ckpt->active = 0;
        return -1;
    }
    switch (argc) {
    case 0: mod->callExportedFunctionWithName<1>(k_stack_fence, name); break;
    case 1: mod->callExportedFunctionWithName<1>(k_stack_fence, name, argv[0]); break;
    case 2: mod->callExportedFunctionWithName<1>(k_stack_fence, name, argv[0], argv[1]); break;
    case 3: mod->callExportedFunctionWithName<1>(k_stack_fence, name, argv[0], argv[1], argv[2]); break;
    default:mod->callExportedFunctionWithName<1>(k_stack_fence, name, argv[0], argv[1], argv[2], argv[3]); break;
    }
    ckpt->active = 0;
    return 0;
}

__attribute__((unused))
static int
call_export(wasm_driver_t *driver, const char *name,
            uint32_t argc, const uint32_t *argv)
{
#ifdef WASMOS_WARP_RING3
    return call_export_mod(module_of(driver), name, argc, argv,
                           driver->r3_user_root, driver->r3_stack_phys);
#else
    return call_export_mod(module_of(driver), name, argc, argv);
#endif
}

#ifdef WASMOS_WARP_RING3
// ---------------------------------------------------------------------------
// Ring-3 execution helpers
// ---------------------------------------------------------------------------

/* USER_CS / USER_SS selectors as used in process.c */
static constexpr uint64_t kUserCS = 0x1Bu;
static constexpr uint64_t kUserSS = 0x23u;
/* Kernel higher-half base for physical → virtual alias translation */
static constexpr uint64_t kHalfBase = 0xFFFFFFFF80000000ULL;
/* roundUpToPow2: rounds v up to the next multiple of 2^n. Used for
 * FunctionInfo binary layout parsing (same formula as WARP's util.hpp). */
static inline uint32_t r3_rup2(uint32_t v, uint32_t n) {
    return (v + n - 1u) & ~(n - 1u);
}

/* Patch the basedata so ring-3 JIT code can reload linMem correctly after
 * any hostcall (LINEAR_MEMORY_BOUNDS_CHECKS=1 caches/restores linMem base
 * around every import call via a double-indirect pointer).
 *
 * We place a "proxy" pointer in the ring-3 stack at WARP_R3_STACK_BASE+0:
 *   proxy = WARP_R3_LINMEM_BASE - basedataLength  (user VA of alloc start)
 * Then patch basedata[jobMemoryDataPtrPtr] = WARP_R3_STACK_BASE (user VA).
 *
 * In ring-3, the JIT wrapper does:
 *   mov r, [RSI - BD::FromEnd::jobMemoryDataPtrPtr]  → loads WARP_R3_STACK_BASE
 *   mov r, [r]                                        → loads alloc-start user VA
 *   add r, basedataLength                             → gets WARP_R3_LINMEM_BASE
 */
static void
warp_r3_patch_basedata(uint8_t *linmem_kernel_ptr, uint64_t basedataLength,
                       uint64_t stack_phys, uint64_t r3_linmem_base,
                       uint64_t jit_kernel_base, uint64_t jit_size,
                       uint64_t custom_ctx, uint64_t runtime_ptr)
{
    if (!linmem_kernel_ptr || basedataLength == 0) return;

    auto patch_jit_va = [jit_kernel_base, jit_size](uint64_t *slot) {
        if (!slot || !*slot) return;
        uint64_t v = *slot;
        if (v < jit_kernel_base || v >= jit_kernel_base + jit_size) return;
        *slot = WARP_R3_JIT_BASE + (v - jit_kernel_base);
    };

    /* Write alloc-start user VA into the proxy slot (ring-3 stack bottom). */
    uint64_t *proxy_kernel =
        reinterpret_cast<uint64_t *>(stack_phys | kHalfBase);
    *proxy_kernel = r3_linmem_base - basedataLength;

    /* Patch the basedata jobMemoryDataPtrPtr field to point to the proxy. */
    uint64_t *bd_field = reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::jobMemoryDataPtrPtr));
    *bd_field = static_cast<uint64_t>(WARP_R3_STACK_BASE);

    /* Patch the memory helper pointer to a ring-3 syscall trampoline.
     * The kernel currently handles only in-bounds requests here; growth that
     * would require remapping remains a deferred ring-3 gap. */
    uint64_t *mh_field = reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::memoryHelperPtr));
    *mh_field = static_cast<uint64_t>(WARP_R3_MEMHELPER_TRAMPOLINE);

    /* ACTIVE_STACK_OVERFLOW_CHECK compares RSP against basedata.stackFence and
     * traps when RSP falls below it. The kernel start() path seeds this with a
     * higher-half fence, which is invalid once exports execute on the user
     * ring-3 stack. */
    uint64_t *sf_field = reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::stackFence));
    *sf_field = static_cast<uint64_t>(WARP_R3_STACK_BASE + 64ULL);

    uint64_t *nsf_field = reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::nativeStackFence));
    *nsf_field = static_cast<uint64_t>(WARP_R3_STACK_BASE + 64ULL + 4096ULL);

    uint64_t *ctx_field = reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::customCtxOffset));
    *ctx_field = custom_ctx;

    uint64_t *runtime_field = reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::runtimePtrOffset));
    *runtime_field = runtime_ptr;

    /* AOT and some JIT paths consult compiled-binary metadata through basedata.
     * Translate any embedded kernel compiled-binary aliases to the user JIT VA. */
    patch_jit_va(reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::tableAddressOffset)));
    patch_jit_va(reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::binaryModuleStartAddressOffset)));
    patch_jit_va(reinterpret_cast<uint64_t *>(
        linmem_kernel_ptr - static_cast<ptrdiff_t>(BD::FromEnd::linkStatusAddressOffset)));
}

/* IRET to ring-3: loads rdi/rsi/rdx/rcx from memory then builds and executes
 * a 5-dword IRET frame.  Uses memory constraints for the register values so
 * the compiler cannot place the 5 IRET-frame values in rdi/rsi/rdx/rcx.
 * Never returns; caller must follow with __builtin_unreachable(). */
static __attribute__((noinline)) void
r3_do_iretq(uint64_t rip, uint64_t rsp, uint64_t rflags,
             uint64_t rdi_v, uint64_t rsi_v,
             uint64_t rdx_v, uint64_t rcx_v)
{
    uint64_t user_cs = kUserCS;
    uint64_t user_ss = kUserSS;
    __asm__ volatile(
        "movq %[rdi_v], %%rdi\n\t"
        "movq %[rsi_v], %%rsi\n\t"
        "movq %[rdx_v], %%rdx\n\t"
        "movq %[rcx_v], %%rcx\n\t"
        "pushq %[ss]\n\t"
        "pushq %[rsp_v]\n\t"
        "pushq %[rf]\n\t"
        "pushq %[cs]\n\t"
        "pushq %[rip_v]\n\t"
        "iretq"
        :
        : [rdi_v] "m"(rdi_v), [rsi_v] "m"(rsi_v),
          [rdx_v] "m"(rdx_v), [rcx_v] "m"(rcx_v),
          [ss]    "r"(user_ss),
          [rsp_v] "r"(rsp),
          [rf]    "r"(rflags),
          [cs]    "r"(user_cs),
          [rip_v] "r"(rip)
        : "memory", "rdi", "rsi", "rdx", "rcx"
    );
    __builtin_unreachable();
}

/* Resolve the JIT wrapper entry point for exported function 'name' and return
 * its ring-3 user VA, or 0 on failure. */
static uint64_t
warp_r3_resolve_export(vb::WasmModule *mod, const char *name)
{
    vb::Span<char const> name_span(name, __builtin_strlen(name));
    vb::Span<char const> sig;
    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
    ckpt->active = 1;
    if (__builtin_setjmp(ckpt->jbuf)) {
        ckpt->active = 0;
        return 0;
    }
    sig = mod->getFunctionSignatureByName(name_span);
    ckpt->active = 0;

    if (!sig.data() || sig.size() == 0) return 0;

    /* FunctionInfo binary layout (from end of compiled binary, backwards):
     *   [binaryEnd - binaryOffset]:
     *     SignatureLength (uint32_t) → stepPtr -= 4
     *     Signature[roundUp2(SigLen)] (bytes) → stepPtr -= roundUp2(SigLen)
     *     WrapperSize (uint32_t) → stepPtr -= 4
     *     Wrapper[roundUp2(WrapperSize)] (bytes) ← fncPtr = stepPtr */
    char const *sig_data = sig.data();
    uint32_t wrapper_size =
        *reinterpret_cast<uint32_t const *>(sig_data - 4);
    uint8_t const *kernel_fn =
        reinterpret_cast<uint8_t const *>(sig_data) - 4 -
        static_cast<ptrdiff_t>(r3_rup2(wrapper_size, 2u));

    vb::Span<uint8_t const> compiled = mod->getCompiledBinary();
    if (kernel_fn < compiled.data() ||
        kernel_fn >= compiled.data() + compiled.size()) {
        return 0;
    }
    uint64_t offset = static_cast<uint64_t>(kernel_fn - compiled.data());
    return WARP_R3_JIT_BASE + offset;
}

/* Invoke WASM export 'name' via ring-3 IRET, blocking the kernel thread until
 * the WARP_RETURN syscall fires.  Returns 0 on success, -1 on error. */
static int
warp_r3_call_export(vb::WasmModule *mod, const char *name,
                    uint32_t argc, const uint32_t *argv,
                    uint64_t user_root, uint64_t stack_phys,
                    uint64_t r3_linmem_base)
{
    uint64_t r3_fn_va = warp_r3_resolve_export(mod, name);
    if (r3_fn_va == 0) {
        klog_write("[warp-r3] failed to resolve export\n");
        return -1;
    }

    /* Set up ring-3 call frame on the user stack (via kernel alias).
     *
     * Layout (from WARP_R3_STACK_BASE upward, using kernel alias for writes):
     *   [+0]  proxy_ptr: alloc-start user VA (for bounds-check restore)
     *   [+8]  serArgs[0..3]: up to 4 × uint64_t serialized WASM args
     *   [+40] trapCode: uint64_t, 0 = no trap
     *   [+48] results: uint64_t (space for one i32 return value)
     *   [+56] entryTarget: uint64_t, loaded by the debug entry trampoline
     *   [+64] entryRsp: uint64_t, captures user RSP right after IRET
     * Return address (WARP_R3_RET_TRAMPOLINE) at [STACK_TOP - 8]; RSP = that. */
    uint8_t *kstack = reinterpret_cast<uint8_t *>(stack_phys | kHalfBase);

    /* proxy_ptr is already set by warp_r3_patch_basedata. */

    /* serArgs at kstack+8, user VA = WARP_R3_STACK_BASE + 8 */
    uint64_t *ser_args = reinterpret_cast<uint64_t *>(kstack + 8);
    for (uint32_t i = 0; i < 4; ++i)
        ser_args[i] = (i < argc) ? static_cast<uint64_t>(argv[i]) : 0ULL;

    /* trapCode, results, and entry trampoline scratch */
    uint64_t *trap_code_k = reinterpret_cast<uint64_t *>(kstack + 40);
    uint64_t *results_k   = reinterpret_cast<uint64_t *>(kstack + 48);
    uint64_t *entry_target_k = reinterpret_cast<uint64_t *>(kstack + 56);
    *trap_code_k = 0;
    *results_k   = 0;
    *entry_target_k = r3_fn_va;

    /* Return address for the JIT wrapper */
    *reinterpret_cast<uint64_t *>(kstack + WARP_R3_STACK_SIZE - 8) =
        WARP_R3_RET_TRAMPOLINE;

    /* Ring-3 register values:
     *   RDI = serArgs user VA
     *   RSI = per-module linMem user VA (preserves low-page offset)
     *   RDX = trapCode user VA
     *   RCX = results user VA
     *   RSP = return address slot (STACK_TOP - 8) */
    uint64_t rdi_v  = WARP_R3_STACK_BASE + 8u;
    uint64_t rsi_v  = r3_linmem_base;
    uint64_t rdx_v  = WARP_R3_STACK_BASE + 40u;
    uint64_t rcx_v  = WARP_R3_STACK_BASE + 48u;
    uint64_t r3_rsp = WARP_R3_STACK_TOP - 8u;

    /* RFLAGS: IF=1 (enable interrupts for INT 0x80 hostcalls), DF=0. */
    uint64_t rflags = 0x202ULL;

    /* Switch to WARP user CR3; kernel higher-half is still accessible
     * because paging_create_address_space shares PML4[511].
     * The call state (old_cr3 / active / jbuf) lives in the calling thread, not
     * cpu-local: a ring-3 call can block in a hostcall and resume on another
     * CPU, so WARP_RETURN must longjmp to this thread's checkpoint regardless of
     * which CPU it fires on. */
    thread_t *t = cpu_local()->current_thread;
    /* Capture the calling process's root table (each ring-3 WARP process has its
     * own; it is not the global kernel root) so WARP_RETURN can restore it. */
    t->warp_r3_old_cr3 = paging_get_current_root_table();
    paging_switch_root(user_root);

    /* Checkpoint: longjmp from WARP_RETURN syscall handler lands here. */
    t->warp_r3_active = 1;
    if (__builtin_setjmp(t->warp_r3_jbuf) != 0) {
        /* WARP_RETURN fired — restore kernel CR3 and return. */
        paging_switch_root(t->warp_r3_old_cr3);
        uint64_t tc = *reinterpret_cast<uint64_t *>(kstack + 40);
        return (tc == 0) ? 0 : -1;
    }

    /* IRET to the debug entry trampoline, which captures user RSP and jumps to
     * the real JIT wrapper stored in entryTarget. Never returns here; resumes above. */
    r3_do_iretq(WARP_R3_ENTRY_TRAMPOLINE, r3_rsp, rflags, rdi_v, rsi_v, rdx_v, rcx_v);
    __builtin_unreachable();
}

extern "C" uint64_t
warp_r3_memory_helper(uint64_t min_linmem_len,
                      uint32_t basedata_len,
                      uint64_t original_linmem_user_va)
{
    if (basedata_len == 0 || original_linmem_user_va < USER_VA_MIN) {
        return 0;
    }

    uint8_t *linmem_user = reinterpret_cast<uint8_t *>(original_linmem_user_va);
    uint32_t actual_linmem = *reinterpret_cast<uint32_t *>(
        linmem_user - static_cast<ptrdiff_t>(BD::FromEnd::actualLinMemByteSize));
    if (min_linmem_len <= actual_linmem) {
        return original_linmem_user_va - static_cast<uint64_t>(basedata_len);
    }

    uint8_t *new_memory_base =
        vb::MemoryHelper::extensionRequest(min_linmem_len, basedata_len, linmem_user);
    if (!new_memory_base) {
        return 0;
    }
    if (reinterpret_cast<uintptr_t>(new_memory_base) == ~static_cast<uintptr_t>(0)) {
        return ~0ULL;
    }

    uint8_t *new_linmem_kernel = new_memory_base + basedata_len;
    uint64_t user_root = paging_get_current_root_table();
    if (warp_mem_ring3_map_linmem(user_root, new_linmem_kernel) != 0) {
        return 0;
    }

    uint64_t new_linmem_user_va =
        WARP_R3_LINMEM_BASE + (reinterpret_cast<uint64_t>(new_linmem_kernel) & 0xFFFULL);
    uint64_t new_memory_base_user_va = new_linmem_user_va - static_cast<uint64_t>(basedata_len);
    *reinterpret_cast<uint64_t *>(WARP_R3_STACK_BASE) = new_memory_base_user_va;
    return new_memory_base_user_va;
}
#endif /* WASMOS_WARP_RING3 */

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

extern "C" {

void
wasm_driver_init(void)
{
    spinlock_init(&g_thread_slots_lock);
}

int
wasm_driver_start(wasm_driver_t *driver,
                  const wasm_driver_manifest_t *manifest,
                  uint32_t owner_context_id)
{
    if (!driver || !manifest || !manifest->module_bytes || !manifest->module_size
        || !owner_context_id) {
        return -1;
    }

    driver->manifest         = *manifest;
    driver->wasm_module      = nullptr;
    driver->owner_context_id = owner_context_id;
    driver->active           = 0;
    driver->started          = 0;
    spinlock_init(&driver->lock);
#ifdef WASMOS_WARP_RING3
    driver->r3_user_root  = 0;
    driver->r3_stack_phys = 0;
    driver->r3_linmem_base = 0;
#endif

    process_t *owner = process_find_by_context(owner_context_id);
    driver->owner_pid = owner ? owner->pid : process_current_pid();

    klog_write("[warp-driver] start: "); klog_write(manifest->name ? manifest->name : "?"); klog_write("\n");

    warp_heap_configure(driver->owner_pid, manifest->heap_size,
                        2ULL * 1024ULL * 1024ULL * 1024ULL);

    uint32_t prev = warp_runtime_enter(driver->owner_pid);

    /* Allocate and compile/load the WASM module; catch any WARP exceptions. */
    vb::WasmModule *mod = nullptr;
    void *warp_ctx = warp_context_for_pid(driver->owner_pid);
    if (!warp_ctx) {
        warp_runtime_leave(prev);
        return -1;
    }
    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();

    /* AOT path: try loading a pre-compiled binary if one was embedded in the
     * .wap.  If it fails (WARP exception or mismatched symbol table) we fall
     * back to JIT below — the module object is deleted and rebuilt fresh. */
    int use_jit = 1;
    if (manifest->compiled_bytes != nullptr && manifest->compiled_size > 0) {
        ckpt->active = 1;
        if (__builtin_setjmp(ckpt->jbuf)) {
            /* AOT load failed — discard the half-initialised module and let
             * the JIT path below handle it. */
            ckpt->active = 0;
            klog_write("[warp-driver] AOT load failed, falling back to JIT\n");
            delete mod;
            mod = nullptr;
        } else {
            klog_write("[warp-driver] using AOT binary\n");
            mod = new vb::WasmModule(UINT64_MAX, g_logger, false, warp_ctx, 10U);
            vb::Span<uint8_t const> compiled(manifest->compiled_bytes, manifest->compiled_size);
            vb::Span<uint8_t const> empty_debug(nullptr, 0);
            /* initFromCompiledBinary requires DYNAMIC_LINK symbols. */
#ifdef WASMOS_WARP_RING3
            mod->initFromCompiledBinary(compiled, warp_wasmos_symbols_ring3(), empty_debug);
#else
            mod->initFromCompiledBinary(compiled, warp_wasmos_symbols_for_aot_load(), empty_debug);
#endif
            ckpt->active = 0;
            use_jit = 0;
        }
    }

    /* JIT path: used when no AOT binary was present or AOT loading failed. */
    if (use_jit) {
        ckpt->active = 1;
        if (__builtin_setjmp(ckpt->jbuf)) {
            klog_write("[warp-driver] module compilation failed\n");
            /* Re-arm the checkpoint before calling the WasmModule destructor.
             * If the destructor throws (e.g. while freeing partially-compiled
             * JIT code), the exception is caught here rather than reaching the
             * top-level handler and causing a kernel panic. */
            ckpt->active = 1;
            if (__builtin_setjmp(ckpt->jbuf) == 0) {
                delete mod;
            }
            ckpt->active = 0;
            mod = nullptr;
            warp_runtime_leave(prev);
            return -1;
        }
        mod = new vb::WasmModule(UINT64_MAX, g_logger, false, warp_ctx, 10U);
        vb::Span<uint8_t const> bc(manifest->module_bytes, manifest->module_size);
#ifdef WASMOS_WARP_RING3
        mod->initFromBytecode(bc, warp_wasmos_symbols_ring3(), true);
#else
        mod->initFromBytecode(bc, warp_wasmos_symbols(), true);
#endif
        ckpt->active = 0;
    }

    warp_bind_module(mod, driver->owner_pid);

#ifdef WASMOS_WARP_RING3
    /* Set up per-module ring-3 user address space and dual-map JIT+linmem. */
    {
        uint64_t user_root = 0;
        uint64_t stack_phys = 0;
        if (warp_r3_setup(&user_root, &stack_phys) != 0) {
            klog_write("[warp-r3] setup failed\n");
            delete mod;
            warp_runtime_leave(prev);
            return -1;
        }
        vb::Span<uint8_t const> jit = mod->getCompiledBinary();
        uint8_t *linmem = mod->getLinearMemoryRegion(0, 0);
        int jit_rc = warp_mem_ring3_map_jit(user_root, jit.data(), jit.size());
        int lm_rc  = warp_mem_ring3_map_linmem(user_root, linmem);
        if (jit_rc != 0 || lm_rc != 0) {
            klog_write("[warp-r3] dual-map failed\n");
            warp_r3_teardown(user_root, stack_phys);
            delete mod;
            warp_runtime_leave(prev);
            return -1;
        }
        driver->r3_user_root  = user_root;
        driver->r3_stack_phys = stack_phys;
        driver->r3_linmem_base =
            WARP_R3_LINMEM_BASE + (reinterpret_cast<uint64_t>(linmem) & 0xFFFULL);
    }
#endif

    if (ipc_endpoint_create(owner_context_id, &driver->endpoint) != IPC_OK) {
        klog_write("[warp-driver] endpoint alloc failed\n");
        delete mod;
        warp_runtime_leave(prev);
        return -1;
    }

    driver->wasm_module = mod;
    driver->active      = 1;
    warp_runtime_leave(prev);
    return 0;
}

void
wasm_driver_stop(wasm_driver_t *driver)
{
    if (!driver || !driver->active) return;
    uint32_t prev = warp_runtime_enter(driver->owner_pid);
    delete module_of(driver);
    driver->wasm_module = nullptr;
    warp_heap_release(driver->owner_pid);
    warp_ctx_release_pid(driver->owner_pid);
    warp_runtime_leave(prev);
    driver->active = 0;
    driver->started = 0;
#ifdef WASMOS_WARP_RING3
    if (driver->r3_user_root) {
        warp_r3_teardown(driver->r3_user_root, driver->r3_stack_phys);
        driver->r3_user_root  = 0;
        driver->r3_stack_phys = 0;
        driver->r3_linmem_base = 0;
    }
#endif
}

int
wasm_driver_endpoint(const wasm_driver_t *driver, uint32_t *out)
{
    if (!driver || !out) return -1;
    *out = driver->endpoint;
    return 0;
}

int
wasm_driver_call_entry(wasm_driver_t *driver)
{
    if (!driver || !driver->active || !driver->wasm_module) return -1;
#ifdef WASMOS_WARP_RING3
    /* Use the noirq variant (no preempt_disable) so that:
     *  - the timer can still fire and advance process_tick during the
     *    ring-0 ensure_started / mod->start() phase, and
     *  - preempt_disable_count stays 0, which lets the watchdog
     *    recognise the ring-0 startup as a transient non-stall condition.
     * Matches the wasm3_lock precedent (see spinlock.c). */
    spinlock_lock_noirq(&driver->lock);
    uint64_t r3_root  = driver->r3_user_root;
    uint64_t r3_stack = driver->r3_stack_phys;
#else
    spinlock_lock(&driver->lock);
    uint64_t r3_root  = 0;
    uint64_t r3_stack = 0;
#endif
    uint32_t prev = warp_runtime_enter(driver->owner_pid);
    if (warp_driver_ensure_started(driver, r3_root, r3_stack) != 0) {
        warp_runtime_leave(prev);
#ifdef WASMOS_WARP_RING3
        spinlock_unlock_noirq(&driver->lock);
#else
        spinlock_unlock(&driver->lock);
#endif
        return -1;
    }
#ifdef WASMOS_WARP_RING3
    if (r3_root) {
        /* Release lock and runtime binding before IRET to ring-3.  Clear any
         * need_resched that accumulated during ring-0 ensure_started so the
         * ring-3 process starts with a fresh scheduling window. */
        warp_runtime_leave(prev);
        spinlock_unlock_noirq(&driver->lock);
        process_clear_resched();
        return call_export_mod(module_of(driver),
                           driver->manifest.entry_export,
                           driver->manifest.entry_argc,
                           driver->manifest.entry_argv,
                           r3_root, r3_stack);
    }
    int rc = call_export_mod(module_of(driver),
                         driver->manifest.entry_export,
                         driver->manifest.entry_argc,
                         driver->manifest.entry_argv,
                         r3_root, r3_stack);
    warp_runtime_leave(prev);
    spinlock_unlock_noirq(&driver->lock);
    return rc;
#else
    int rc = call_export_mod(module_of(driver),
                         driver->manifest.entry_export,
                         driver->manifest.entry_argc,
                         driver->manifest.entry_argv,
                         r3_root, r3_stack);
    warp_runtime_leave(prev);
    spinlock_unlock(&driver->lock);
    return rc;
#endif
}

int
wasm_driver_call(wasm_driver_t *driver, const char *name,
                 uint32_t argc, uint32_t *argv)
{
    if (!driver || !driver->active || !driver->wasm_module) return -1;
#ifdef WASMOS_WARP_RING3
    spinlock_lock_noirq(&driver->lock);
    uint64_t r3_root  = driver->r3_user_root;
    uint64_t r3_stack = driver->r3_stack_phys;
#else
    spinlock_lock(&driver->lock);
    uint64_t r3_root  = 0;
    uint64_t r3_stack = 0;
#endif
    uint32_t prev = warp_runtime_enter(driver->owner_pid);
    if (warp_driver_ensure_started(driver, r3_root, r3_stack) != 0) {
        warp_runtime_leave(prev);
#ifdef WASMOS_WARP_RING3
        spinlock_unlock_noirq(&driver->lock);
#else
        spinlock_unlock(&driver->lock);
#endif
        return -1;
    }
#ifdef WASMOS_WARP_RING3
    if (r3_root) {
        warp_runtime_leave(prev);
        spinlock_unlock_noirq(&driver->lock);
        process_clear_resched();
        return call_export_mod(module_of(driver), name, argc, argv, r3_root, r3_stack);
    }
    int rc = call_export_mod(module_of(driver), name, argc, argv, r3_root, r3_stack);
    warp_runtime_leave(prev);
    spinlock_unlock_noirq(&driver->lock);
    return rc;
#else
    int rc = call_export_mod(module_of(driver), name, argc, argv, r3_root, r3_stack);
    warp_runtime_leave(prev);
    spinlock_unlock(&driver->lock);
    return rc;
#endif
}

int
wasm_driver_call_unlocked(wasm_driver_t *driver, const char *name,
                          uint32_t argc, uint32_t *argv)
{
    if (!driver || !driver->active || !driver->wasm_module) return -1;
    uint32_t prev = warp_runtime_enter(driver->owner_pid);
#ifdef WASMOS_WARP_RING3
    uint64_t r3_root  = driver->r3_user_root;
    uint64_t r3_stack = driver->r3_stack_phys;
    if (r3_root) {
        /* pm_app_entry holds preempt_disable() (PREEMPT_GUARD) for ring-0
         * re-entrancy protection, but ring-3 WARP execution must be
         * timer-preemptible.  Drain the guard so:
         *  - ensure_started / mod->start() do not trigger watchdog stalls,
         *  - the ring-3 IRET runs with preempt_disable_count=0, and
         *  - the timer can preempt ring-3 code normally.
         * pm_app_entry's paired preempt_enable() is a safe no-op at zero. */
        while (preempt_disable_depth() > 0) preempt_enable();
        process_clear_resched();
    }
#else
    uint64_t r3_root  = 0;
    uint64_t r3_stack = 0;
#endif
    if (warp_driver_ensure_started(driver, r3_root, r3_stack) != 0) {
        warp_runtime_leave(prev);
        return -1;
    }
    int rc = call_export_mod(module_of(driver), name, argc, argv, r3_root, r3_stack);
    warp_runtime_leave(prev);
    return rc;
}

static process_run_result_t
warp_vm_thread_entry(process_t *process, uint32_t tid, void *arg)
{
    warp_driver_thread_slot_t *slot = static_cast<warp_driver_thread_slot_t *>(arg);
    (void)tid;
    if (!process || !slot || !slot->in_use) return PROCESS_RUN_EXITED;

    warp_heap_configure(slot->owner_pid, slot->heap_size,
                        2ULL * 1024ULL * 1024ULL * 1024ULL);
    uint32_t prev = warp_runtime_enter(slot->owner_pid);

    vb::WasmModule *mod = nullptr;
    int rc = -1;
    void *warp_ctx = warp_context_for_pid(slot->owner_pid);
    if (!warp_ctx) {
        warp_runtime_leave(prev);
        process_set_exit_status(process, -1);
        thread_slot_free(slot);
        process_yield(PROCESS_RUN_THREAD_EXITED);
        __builtin_unreachable();
    }

    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
    ckpt->active = 1;
    if (__builtin_setjmp(ckpt->jbuf) == 0) {
        mod = new vb::WasmModule(UINT64_MAX, g_logger, false, warp_ctx, 10U);
        vb::Span<uint8_t const> bc(slot->module_bytes, slot->module_size);
        mod->initFromBytecode(bc, warp_wasmos_symbols(), true);
        warp_bind_module(mod, slot->owner_pid);
        ckpt->active = 0;
        if (warp_driver_start_module(mod, 0) != 0) {
            rc = -1;
        } else {
            /* Thread slots have no per-slot ring-3 setup; pass 0 to run kernel-mode. */
            rc = call_export_mod(mod, slot->export_name, slot->argc, slot->argv, 0, 0);
        }
    } else {
        klog_write("[warp-driver] vm thread: module init failed\n");
    }
    ckpt->active = 0;

    delete mod;
    warp_runtime_leave(prev);
    process_set_exit_status(process, rc);
    thread_slot_free(slot);
    process_yield(PROCESS_RUN_THREAD_EXITED);
    __builtin_unreachable();
}

int
wasm_driver_spawn_vm_thread(uint32_t owner_pid, const char *export_name,
                             uint32_t argc, const uint32_t *argv,
                             uint32_t *out_tid)
{
    warp_driver_thread_slot_t *slot = thread_slot_alloc();
    if (!slot) return -1;

    slot->owner_pid    = owner_pid;
    slot->module_bytes = nullptr; /* TODO: locate bytes from owner_pid */
    slot->module_size  = 0;
    slot->stack_size   = 64u * 1024u;
    slot->heap_size    = 2u * 1024u * 1024u;
    slot->argc         = argc <= 4 ? argc : 4;
    for (uint32_t i = 0; i < slot->argc; ++i) slot->argv[i] = argv[i];
    __builtin_strncpy(slot->export_name, export_name,
                      sizeof(slot->export_name) - 1);
    slot->export_name[sizeof(slot->export_name) - 1] = '\0';

    uint32_t tid = 0;
    if (process_thread_spawn_worker_internal(owner_pid, "warp-vm-thread",
                                             warp_vm_thread_entry, slot,
                                             &tid) != 0) {
        thread_slot_free(slot);
        return -1;
    }
    if (out_tid) *out_tid = tid;
    return 0;
}

} // extern "C"
