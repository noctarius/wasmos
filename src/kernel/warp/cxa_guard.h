#ifndef WASMOS_WARP_CXA_GUARD_H
#define WASMOS_WARP_CXA_GUARD_H

/* Function-local static initialization guards for the WARP backend's C++ runtime
 * (Itanium C++ ABI §3.3.2): warp/cxx_abi.cpp's __cxa_guard_acquire, _release and
 * _abort are these functions.  The guard is the 64-bit object the compiler
 * reserves beside each guarded static; the compiler's inline fast path tests its
 * first byte with an acquire load and skips the call once that byte is non-zero.
 *
 * The guard word holds one of three values:
 *   0                        not initialized
 *   WARP_CXA_GUARD_BUSY      one caller is running the initializer
 *   WARP_CXA_GUARD_DONE      initialized; the first byte is non-zero
 * Several CPUs can be inside WARP at once (warp_runtime_enter in warp/shim.cpp), so
 * every transition is atomic.  acquire returns 1 to exactly one caller, which
 * must then call release or abort; every other caller waits while the guard is
 * BUSY and returns 0 once it is DONE, so it never sees a half-built object.  The
 * wait spins with preemption enabled: an initializer that blocks costs the
 * waiters CPU time, not progress.  abort returns the guard to 0, and the next
 * acquire runs the initializer again.  An initializer that reaches its own
 * guard deadlocks, as the ABI permits.
 *
 * A WARP throw longjmps to the exception checkpoint without unwinding, so an
 * initializer that throws never reaches abort.  The holder's checkpoint therefore
 * records every guard it holds BUSY (warp_exception_note_guard), and throw
 * delivery returns those guards to 0 (warp_exception_abandon). */

#include "arch/arch_cpu.h"
#include "warp_exception.h"

#define WARP_CXA_GUARD_BUSY 0x100ULL
#define WARP_CXA_GUARD_DONE 0x1ULL

static inline int warp_cxa_guard_acquire(unsigned long long* g) {
    for (;;) {
        unsigned long long expected = 0;
        if (__atomic_compare_exchange_n(
                g, &expected, WARP_CXA_GUARD_BUSY, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            warp_exception_note_guard(g);
            return 1;
        }
        if (expected == WARP_CXA_GUARD_DONE) {
            return 0;
        }
        arch_cpu_relax();
    }
}

static inline void warp_cxa_guard_release(unsigned long long* g) {
    warp_exception_drop_guard(g);
    __atomic_store_n(g, WARP_CXA_GUARD_DONE, __ATOMIC_RELEASE);
}

static inline void warp_cxa_guard_abort(unsigned long long* g) {
    warp_exception_drop_guard(g);
    __atomic_store_n(g, 0ULL, __ATOMIC_RELEASE);
}

#endif
