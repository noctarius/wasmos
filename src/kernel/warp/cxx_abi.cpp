/* warp/cxx_abi.cpp - Bare-metal C++ exception ABI stubs for the WARP runtime.
 *
 * WARP throws C++ exceptions internally (TrapException, bad_alloc, etc.).
 * All WARP call sites in the kernel are wrapped in try/catch blocks so
 * exceptions should not escape to C code.  If they do, __cxa_throw panics.
 *
 * We compile WARP's own .cpp files with -fsjlj-exceptions so that the
 * setjmp/longjmp exception model is used — this works in a freestanding
 * kernel without libunwind.  The stubs below satisfy the linker for any
 * remaining ABI symbols. */

extern "C" {
#include "klog.h"
#include "slab.h"
}

// ---------------------------------------------------------------------------
// Exception object allocation
// ---------------------------------------------------------------------------

extern "C" void *__cxa_allocate_exception(unsigned long size) noexcept {
    /* +8 for the exception header that __cxa_throw uses internally. */
    return kalloc_small(size + 8);
}

extern "C" void __cxa_free_exception(void *e) noexcept {
    if (e) kfree_small(e);
}

// ---------------------------------------------------------------------------
// Throw — should never be reached because our try/catch boundary catches all
// WARP exceptions before they propagate into C code.
// ---------------------------------------------------------------------------

extern "C" __attribute__((noreturn))
void __cxa_throw(void *, void *, void (*)(void *)) {
    klog_write("[warp] uncaught C++ exception — kernel panic\n");
    for (;;) { __asm__ volatile("hlt"); }
}

// ---------------------------------------------------------------------------
// Catch machinery
// ---------------------------------------------------------------------------

extern "C" void *__cxa_begin_catch(void *e) noexcept { return e; }
extern "C" void  __cxa_end_catch(void)       noexcept {}

extern "C" __attribute__((noreturn)) void __cxa_rethrow(void) {
    klog_write("[warp] __cxa_rethrow — kernel panic\n");
    for (;;) { __asm__ volatile("hlt"); }
}

extern "C" void *__cxa_current_exception_type(void) noexcept { return nullptr; }

// ---------------------------------------------------------------------------
// Static destructor registration — no-op in kernel
// ---------------------------------------------------------------------------

extern "C" int  __cxa_atexit(void (*)(void *), void *, void *) noexcept { return 0; }
extern "C" void __cxa_finalize(void *) noexcept {}
extern "C" void *__dso_handle = nullptr;

// ---------------------------------------------------------------------------
// Static-local init guards (single-threaded — WARP init is serialised by
// warp_runtime_enter so no concurrent guard races are possible).
// ---------------------------------------------------------------------------

extern "C" int  __cxa_guard_acquire(unsigned long long *g) noexcept {
    if (*g & 1ULL) return 0;
    *g = 0x100ULL;
    return 1;
}
extern "C" void __cxa_guard_release(unsigned long long *g) noexcept { *g = 1ULL; }
extern "C" void __cxa_guard_abort  (unsigned long long *g) noexcept { *g = 0ULL; }

// ---------------------------------------------------------------------------
// std::terminate / std::unexpected — both panic
// ---------------------------------------------------------------------------

namespace std {
    __attribute__((noreturn)) void terminate() noexcept {
        klog_write("[warp] std::terminate\n");
        for (;;) { __asm__ volatile("hlt"); }
    }
}

// ---------------------------------------------------------------------------
// Pure virtual call — should never happen
// ---------------------------------------------------------------------------

extern "C" __attribute__((noreturn)) void __cxa_pure_virtual(void) {
    klog_write("[warp] pure virtual call\n");
    for (;;) { __asm__ volatile("hlt"); }
}
