/* warp/cxx_abi.cpp - Bare-metal C++ exception ABI for the WARP JIT runtime.
 *
 * Exception dispatch strategy:
 *   WARP throws C++ exceptions internally.  We bypass the standard Dwarf/SJLJ
 *   unwinding infrastructure entirely and use a per-CPU "throw checkpoint"
 *   instead.  Before each WARP call, warp_driver.cpp saves a __builtin_setjmp
 *   buffer via warp_exception_checkpoint_set().  If __cxa_throw is reached it
 *   calls __builtin_longjmp to that checkpoint — effectively implementing a
 *   zero-infrastructure catch-all at the driver boundary.
 *
 *   Benefits: no Dwarf tables, no SJLJ register/unregister overhead, no LSDA
 *   parsing.  Works with -fexceptions (Dwarf-based throw) or -fsjlj-exceptions.
 *
 *   Limitation: only one active checkpoint per CPU; nested checkpoints are not
 *   supported (not needed — all WARP calls originate from warp_driver.cpp). */

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "klog.h"
#include "slab.h"
#include "serial.h"
#include "arch/x86_64/smp.h"
}

// ---------------------------------------------------------------------------
// Per-CPU exception checkpoint
// ---------------------------------------------------------------------------

struct WarpExceptionCheckpoint {
    void   *jbuf[5];   /* __builtin_setjmp buffer */
    int     active;    /* 1 if a checkpoint is set */
};

static WarpExceptionCheckpoint g_warp_ckpt[64]; /* indexed by cpu_id */

extern "C" WarpExceptionCheckpoint *warp_exception_get_checkpoint(void)
{
    uint32_t id = cpu_local()->cpu_id;
    return &g_warp_ckpt[id < 64u ? id : 0u];
}

// ---------------------------------------------------------------------------
// Exception object allocation
// ---------------------------------------------------------------------------

extern "C" void *__cxa_allocate_exception(unsigned long size) noexcept {
    return kalloc_small(size + 32); /* +32 for optional header alignment */
}

extern "C" void __cxa_free_exception(void *e) noexcept {
    if (e) kfree_small(e);
}

// ---------------------------------------------------------------------------
// Throw — longjmp to the nearest driver checkpoint if one is set
// ---------------------------------------------------------------------------

extern "C" __attribute__((noreturn))
void __cxa_throw(void *obj, void *type_info, void (* /*dtor*/)(void *))
{
    (void)obj; (void)type_info;
    WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
    if (ckpt->active) {
        ckpt->active = 0;
        __builtin_longjmp(ckpt->jbuf, 1);
    }
    klog_write("[warp] uncaught C++ exception — kernel panic\n");
    for (;;) { __asm__ volatile("hlt"); }
}

// ---------------------------------------------------------------------------
// Catch machinery — stubs (we bypass the C++ catch mechanism)
// ---------------------------------------------------------------------------

extern "C" void *__cxa_begin_catch(void *e) noexcept { return e; }
extern "C" void  __cxa_end_catch(void)       noexcept {}

extern "C" __attribute__((noreturn)) void __cxa_rethrow(void) {
    klog_write("[warp] __cxa_rethrow — kernel panic\n");
    for (;;) { __asm__ volatile("hlt"); }
}

extern "C" void *__cxa_current_exception_type(void) noexcept { return nullptr; }

// ---------------------------------------------------------------------------
// Dwarf / SJLJ unwind stubs — needed at link time even though we bypass them
// ---------------------------------------------------------------------------

typedef int   _Unwind_Reason_Code;
typedef void *_Unwind_Exception;

extern "C" {
_Unwind_Reason_Code __gxx_personality_v0(...)  { return 3; }
__attribute__((noreturn)) void _Unwind_Resume(_Unwind_Exception) {
    for (;;) { __asm__ volatile("hlt"); }
}
}

// ---------------------------------------------------------------------------
// Static destructor / guard stubs
// ---------------------------------------------------------------------------

extern "C" int  __cxa_atexit(void (*)(void *), void *, void *) noexcept { return 0; }
extern "C" void __cxa_finalize(void *) noexcept {}
extern "C" void *__dso_handle = nullptr;

extern "C" int  __cxa_guard_acquire(unsigned long long *g) noexcept {
    if (*g & 1ULL) return 0;
    *g = 0x100ULL;
    return 1;
}
extern "C" void __cxa_guard_release(unsigned long long *g) noexcept { *g = 1ULL; }
extern "C" void __cxa_guard_abort  (unsigned long long *g) noexcept { *g = 0ULL; }

namespace std {
    __attribute__((noreturn)) void terminate() noexcept {
        klog_write("[warp] std::terminate\n");
        for (;;) { __asm__ volatile("hlt"); }
    }
}

extern "C" __attribute__((noreturn)) void __cxa_pure_virtual(void) {
    klog_write("[warp] pure virtual call\n");
    for (;;) { __asm__ volatile("hlt"); }
}
