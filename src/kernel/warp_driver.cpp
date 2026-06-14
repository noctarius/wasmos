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
#include "process.h"
#include "thread.h"
#include "spinlock.h"
#include "ipc.h"
#include "string.h"
}

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/Span.hpp"
#include "src/core/common/ILogger.hpp"
#include "warp/shim.h"
#include "warp/link.h"

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
        if (msg.data() && msg.size() > 0) klog_write(msg.data());
        return *this;
    }
    KernelLogger &operator<<(uint32_t const) override { return *this; }
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

// ---------------------------------------------------------------------------
// Call helper — invokes an exported function with 0-4 i32 args
// ---------------------------------------------------------------------------

static int
call_export_mod(vb::WasmModule *mod, const char *name,
                uint32_t argc, const uint32_t *argv)
{
    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
    ckpt->active = 1;
    if (__builtin_setjmp(ckpt->jbuf)) {
        klog_write("[warp-driver] exception during export call\n");
        ckpt->active = 0;
        return -1;
    }
    switch (argc) {
    case 0: mod->callExportedFunctionWithName<1>(nullptr, name); break;
    case 1: mod->callExportedFunctionWithName<1>(nullptr, name, argv[0]); break;
    case 2: mod->callExportedFunctionWithName<1>(nullptr, name, argv[0], argv[1]); break;
    case 3: mod->callExportedFunctionWithName<1>(nullptr, name, argv[0], argv[1], argv[2]); break;
    default:mod->callExportedFunctionWithName<1>(nullptr, name, argv[0], argv[1], argv[2], argv[3]); break;
    }
    ckpt->active = 0;
    return 0;
}

__attribute__((unused))
static int
call_export(wasm_driver_t *driver, const char *name,
            uint32_t argc, const uint32_t *argv)
{
    return call_export_mod(module_of(driver), name, argc, argv);
}

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
    spinlock_init(&driver->lock);

    process_t *owner = process_find_by_context(owner_context_id);
    driver->owner_pid = owner ? owner->pid : process_current_pid();

    warp_heap_configure(driver->owner_pid, manifest->heap_size,
                        2ULL * 1024ULL * 1024ULL * 1024ULL);

    uint32_t prev = warp_runtime_enter(driver->owner_pid);

    /* Allocate and compile the WASM module; catch any WARP exceptions. */
    vb::WasmModule *mod = nullptr;
    {
        WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
        ckpt->active = 1;
        if (__builtin_setjmp(ckpt->jbuf)) {
            klog_write("[warp-driver] module compilation failed\n");
            ckpt->active = 0;
            delete mod;
            warp_runtime_leave(prev);
            return -1;
        }
        mod = new vb::WasmModule(g_logger);
        vb::Span<uint8_t const> bc(manifest->module_bytes, manifest->module_size);
        mod->initFromBytecode(bc, warp_wasmos_symbols(), true);
        ckpt->active = 0;
    }

    warp_bind_module(mod, driver->owner_pid);

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
    warp_runtime_leave(prev);
    driver->active = 0;
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
    spinlock_lock(&driver->lock);
    uint32_t prev = warp_runtime_enter(driver->owner_pid);
    /* start() runs the WASM start section; exceptions are caught. */
    {
        WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
        ckpt->active = 1;
        if (__builtin_setjmp(ckpt->jbuf) == 0)
            module_of(driver)->start(nullptr);
        ckpt->active = 0;
    }
    int rc = call_export_mod(module_of(driver),
                         driver->manifest.entry_export,
                         driver->manifest.entry_argc,
                         driver->manifest.entry_argv);
    warp_runtime_leave(prev);
    spinlock_unlock(&driver->lock);
    return rc;
}

int
wasm_driver_call(wasm_driver_t *driver, const char *name,
                 uint32_t argc, uint32_t *argv)
{
    if (!driver || !driver->active || !driver->wasm_module) return -1;
    spinlock_lock(&driver->lock);
    uint32_t prev = warp_runtime_enter(driver->owner_pid);
    int rc = call_export_mod(module_of(driver), name, argc, argv);
    warp_runtime_leave(prev);
    spinlock_unlock(&driver->lock);
    return rc;
}

int
wasm_driver_call_unlocked(wasm_driver_t *driver, const char *name,
                          uint32_t argc, uint32_t *argv)
{
    if (!driver || !driver->active || !driver->wasm_module) return -1;
    uint32_t prev = warp_runtime_enter(driver->owner_pid);
    int rc = call_export_mod(module_of(driver), name, argc, argv);
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

    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
    ckpt->active = 1;
    if (__builtin_setjmp(ckpt->jbuf) == 0) {
        mod = new vb::WasmModule(g_logger);
        vb::Span<uint8_t const> bc(slot->module_bytes, slot->module_size);
        mod->initFromBytecode(bc, warp_wasmos_symbols(), true);
        warp_bind_module(mod, slot->owner_pid);
        ckpt->active = 0;
        rc = call_export_mod(mod, slot->export_name, slot->argc, slot->argv);
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
