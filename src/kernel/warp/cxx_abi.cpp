/* warp/cxx_abi.cpp - Bare-metal C++ exception ABI for the WARP JIT runtime.
 *
 * Exception dispatch strategy:
 *   WARP throws C++ exceptions internally.  The standard Dwarf/SJLJ unwinding
 *   infrastructure is bypassed entirely in favour of a per-thread "throw
 *   checkpoint" (include/warp_exception.h).  Before each WARP call, warp_driver.cpp
 *   arms a warp_setjmp buffer in it.  If __cxa_throw is reached it
 *   calls warp_longjmp to that checkpoint — effectively implementing a
 *   zero-infrastructure catch-all at the driver boundary.
 *
 *   Benefits: no Dwarf tables, no SJLJ register/unregister overhead, no LSDA
 *   parsing.  Works with -fexceptions (Dwarf-based throw) or -fsjlj-exceptions.
 *
 *   Limitation: only one active checkpoint per thread; nested checkpoints are not
 *   supported (not needed — all WARP calls originate from warp_driver.cpp). */

#include <stdint.h>
#include <stddef.h>

#include "../include/kpanic.h"
#include "cxa_guard.h"

extern "C" {
#include "klog.h"
#include "slab.h"
#include "serial.h"
#include "warp_exception.h"
}

#include "src/core/common/VbExceptions.hpp"

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

/* WARP's exception types, by the mangled name in their type_info, with the C++ name to
 * log.  vb::TrapException and vb::RuntimeError derive from std::exception, and the rest
 * from vb::RuntimeError (VbExceptions.hpp), all by single non-virtual inheritance, so a
 * pointer to the thrown object is a pointer to its std::exception base and what() is
 * the virtual the type overrides. */
struct WarpExceptionType {
    const char* mangled;
    const char* name;
};

static const WarpExceptionType k_warp_exception_types[] = {
    {"N2vb13TrapExceptionE", "vb::TrapException"},
    {"N2vb12RuntimeErrorE", "vb::RuntimeError"},
    {"N2vb19ValidationExceptionE", "vb::ValidationException"},
    {"N2vb16LinkingExceptionE", "vb::LinkingException"},
    {"N2vb28FeatureNotSupportedExceptionE", "vb::FeatureNotSupportedException"},
    {"N2vb33ImplementationLimitationExceptionE", "vb::ImplementationLimitationException"},
};

static bool names_equal(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* Log one line naming a thrown exception: "[warp] throw <type>: <message>" for WARP's
 * own exceptions, with the message WARP gives (TrapException::what, RuntimeError::what),
 * and "[warp] throw <mangled type name>" for anything else (std::bad_alloc is
 * St9bad_alloc).  The name is the second word of the Itanium type_info. */
static void warp_log_throw(void* obj, const void* type_info) {
    const char* mangled = type_info ? static_cast<const char* const*>(type_info)[1] : nullptr;
    if (!mangled) {
        klog_write("[warp] throw (no type)\n");
        return;
    }
    for (const WarpExceptionType& t : k_warp_exception_types) {
        if (names_equal(mangled, t.mangled)) {
            klog_printf("[warp] throw %s: %s\n", t.name, static_cast<std::exception*>(obj)->what());
            return;
        }
    }
    klog_printf("[warp] throw %s\n", mangled);
}

/* Deliver a thrown exception.  The exception is logged (warp_log_throw), then its object,
 * type_info and destructor are otherwise ignored: if the calling thread has an armed
 * checkpoint it is disarmed and control longjmps there, otherwise the kernel panics.
 * Consequences a caller must plan for — no type matching, so the nearest armed checkpoint
 * catches everything; no unwinding, so destructors between the throw and the checkpoint DO
 * NOT RUN and anything they would have released (locks, page-table roots, allocations)
 * leaks; and the exception object is never freed. */
extern "C" __attribute__((noreturn)) void __cxa_throw(void* obj, void* type_info,
                                                      void (* /*dtor*/)(void*)) {
    warp_log_throw(obj, type_info);
    warp_exception_checkpoint_t* ckpt = warp_exception_get_checkpoint();
    if (ckpt->active) {
        warp_exception_abandon(ckpt);
        warp_longjmp(ckpt->jbuf);
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
 * __dso_handle exists only to be addressed.  The guard triple is warp/cxa_guard.h. */
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) noexcept {
    return 0;
}
extern "C" void __cxa_finalize(void*) noexcept {}
extern "C" void* __dso_handle = nullptr;

extern "C" int __cxa_guard_acquire(unsigned long long* g) noexcept {
    return warp_cxa_guard_acquire(g);
}
extern "C" void __cxa_guard_release(unsigned long long* g) noexcept {
    warp_cxa_guard_release(g);
}
extern "C" void __cxa_guard_abort(unsigned long long* g) noexcept {
    warp_cxa_guard_abort(g);
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
