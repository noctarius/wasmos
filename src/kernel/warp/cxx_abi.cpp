/* warp/cxx_abi.cpp - Bare-metal C++ exception ABI for the WARP JIT runtime.
 *
 * Exception dispatch strategy:
 *   WARP throws C++ exceptions internally.  The standard Dwarf/SJLJ unwinding
 *   infrastructure is bypassed entirely in favour of a per-CPU "throw
 *   checkpoint".  Before each WARP call, warp_driver.cpp saves a __builtin_setjmp
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

#include "../include/kpanic.h"

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
    void* jbuf[5]; /* __builtin_setjmp buffer */
    int active;    /* 1 if a checkpoint is set */
};

/* One checkpoint per CPU, capped at 64.  A cpu_id at or above the cap folds onto slot
 * 0, so such a CPU would share slot 0's checkpoint; the kernel does not bring up that
 * many CPUs.  Zero-initialised, so `active` is 0 until a driver arms one. */
static WarpExceptionCheckpoint g_warp_ckpt[64]; /* indexed by cpu_id */

extern "C" WarpExceptionCheckpoint* warp_exception_get_checkpoint(void) {
    uint32_t id = cpu_local()->cpu_id;
    return &g_warp_ckpt[id < 64u ? id : 0u];
}

// ---------------------------------------------------------------------------
// Exception object allocation
// ---------------------------------------------------------------------------

/* Exception object allocation.  The block comes from the kernel slab, so an exception
 * object cannot exceed the slab's largest class.  Returns nullptr on exhaustion, which
 * the compiler-generated throw sequence does not check — allocation failure inside a
 * throw is not recoverable here.  __cxa_free_exception tolerates a null pointer. */
extern "C" void* __cxa_allocate_exception(unsigned long size) noexcept {
    return kalloc_small(size + 32); /* +32 for optional header alignment */
}

extern "C" void __cxa_free_exception(void* e) noexcept {
    if (e)
        kfree_small(e);
}

// ---------------------------------------------------------------------------
// Throw — longjmp to the nearest driver checkpoint if one is set
// ---------------------------------------------------------------------------

/* Deliver a thrown exception.  Ignores the exception object, its type_info and its
 * destructor entirely: if this CPU has an armed checkpoint it is disarmed and control
 * longjmps there, otherwise the kernel panics.  Consequences a caller must plan for —
 * no type matching, so the nearest armed checkpoint catches everything; no unwinding,
 * so destructors between the throw and the checkpoint DO NOT RUN and anything they
 * would have released (locks, page-table roots, allocations) leaks; and the exception
 * object is never freed. */
extern "C" __attribute__((noreturn)) void __cxa_throw(void* obj, void* type_info,
                                                      void (* /*dtor*/)(void*)) {
    WarpExceptionCheckpoint* ckpt = warp_exception_get_checkpoint();
    if (ckpt->active) {
        ckpt->active = 0;
        __builtin_longjmp(ckpt->jbuf, 1);
    }
    klog_write("[warp] uncaught C++ exception — kernel panic\n");
    kpanic("uncaught_cpp_exception", 0ULL, 0ULL);
}

// ---------------------------------------------------------------------------
// Catch machinery — stubs; the C++ catch mechanism is bypassed
// ---------------------------------------------------------------------------

/* Catch-machinery stubs, present for link completeness only: the checkpoint scheme
 * bypasses the real mechanism, so a compiler-generated landing pad is never reached.
 * __cxa_begin_catch is the identity, __cxa_end_catch does nothing (in particular it
 * does not free the exception object), __cxa_current_exception_type always reports
 * "no exception in flight", and __cxa_rethrow panics rather than re-delivering. */
extern "C" void* __cxa_begin_catch(void* e) noexcept {
    return e;
}
extern "C" void __cxa_end_catch(void) noexcept {}

extern "C" __attribute__((noreturn)) void __cxa_rethrow(void) {
    klog_write("[warp] __cxa_rethrow — kernel panic\n");
    kpanic("cxa_rethrow_panic", 0ULL, 0ULL);
}

extern "C" void* __cxa_current_exception_type(void) noexcept {
    return nullptr;
}

// ---------------------------------------------------------------------------
// Dwarf / SJLJ unwind stubs — required at link time even though unused
// ---------------------------------------------------------------------------

typedef int _Unwind_Reason_Code;
typedef void* _Unwind_Exception;

extern "C" {
/* Dwarf unwinder symbols the linker demands even though no unwinding happens.
 * __gxx_personality_v0 returns 3 (_URC_FATAL_PHASE1_ERROR), i.e. it never claims a
 * frame; _Unwind_Resume panics.  Reaching either means a throw escaped the checkpoint
 * scheme. */
_Unwind_Reason_Code __gxx_personality_v0(...) {
    return 3;
}
__attribute__((noreturn)) void _Unwind_Resume(_Unwind_Exception) {
    kpanic("uncaught_unwind_resume", 0ULL, 0ULL);
}
}

// ---------------------------------------------------------------------------
// Static destructor / guard stubs
// ---------------------------------------------------------------------------

/* Static-object lifetime and guard stubs.  __cxa_atexit accepts and DISCARDS the
 * destructor, reporting success — static destructors never run, which is correct for a
 * kernel that does not shut its runtime down.  __cxa_finalize does nothing and
 * __dso_handle exists only to be addressed.
 *
 * The guard triple implements a non-atomic, non-blocking first-use check: acquire
 * returns 1 to the first caller and 0 once the low bit is set, release sets it, abort
 * clears the whole word.  There is no waiting, so two CPUs initialising the same
 * function-local static concurrently can both run the initialiser — sound only under
 * the WARP single-CPU invariant (warp/shim.cpp). */
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) noexcept {
    return 0;
}
extern "C" void __cxa_finalize(void*) noexcept {}
extern "C" void* __dso_handle = nullptr;

extern "C" int __cxa_guard_acquire(unsigned long long* g) noexcept {
    if (*g & 1ULL)
        return 0;
    *g = 0x100ULL;
    return 1;
}
extern "C" void __cxa_guard_release(unsigned long long* g) noexcept {
    *g = 1ULL;
}
extern "C" void __cxa_guard_abort(unsigned long long* g) noexcept {
    *g = 0ULL;
}

namespace std {
/* Terminal handlers: both panic the kernel rather than aborting a process.
 * std::terminate is reached on an unhandled-exception path the stubs above did not
 * intercept; __cxa_pure_virtual on a virtual call through a partially constructed or
 * destroyed object. */
__attribute__((noreturn)) void terminate() noexcept {
    klog_write("[warp] std::terminate\n");
    kpanic("std_terminate_panic", 0ULL, 0ULL);
}
} // namespace std

extern "C" __attribute__((noreturn)) void __cxa_pure_virtual(void) {
    klog_write("[warp] pure virtual call\n");
    kpanic("pure_virtual_call_panic", 0ULL, 0ULL);
}
