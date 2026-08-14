#pragma once
/* compat/sys/signal.h — freestanding signal stub for bare-metal kernel.
 *
 * Provides sig_atomic_t, sigset_t, a three-field siginfo_t, struct sigaction,
 * SIGSEGV/SIGFPE/SIGBUS, SA_SIGINFO, and a declaration of sigaction().
 * compat/csignal re-exports the types from here into namespace std.
 *
 * sigaction() is the only function, and it is backed by a real definition in
 * src/kernel/warp/posix_kernel.c.  Everything POSIX would also give you —
 * sigemptyset/sigaddset, sigprocmask, pthread_sigmask, kill, raise, sigaltstack
 * and the SIG_DFL/SIG_IGN dispositions — is absent, so reaching for one is a
 * compile error rather than a call that quietly does nothing.
 *
 * THE HANDLER IS NEVER CALLED.  The kernel has no signal delivery mechanism at
 * all: sigaction() discards act, leaves oldact untouched, and returns 0, so a
 * caller sees a successful registration for a handler that can never run.  A
 * caller may rely on the registration call succeeding and on nothing else.
 *
 * That is safe because the signal paths are dead in this build.  WARP uses
 * SIGSEGV/SIGFPE/SIGBUS handlers for stack-overflow and linear-memory bounds
 * recovery; the kernel compiles WARP with ACTIVE_STACK_OVERFLOW_CHECK=1 and
 * LINEAR_MEMORY_BOUNDS_CHECKS=1, which select the explicit checks instead, and
 * the sources that would install the handlers (utils/RAIISignalHandler.cpp,
 * utils/SignalFunctionWrapper_unix.cpp) are not compiled into the kernel.  A
 * real fault in JIT code lands in the kernel's own #PF/#GP handlers.
 *
 * The consequence of that arrangement being disturbed is silent: turning either
 * check off would leave WARP believing it had a handler installed, and the
 * fault would reach the kernel exception path unfiltered. */

#include <stdint.h>

/* Basic signal-related types. */
typedef int sig_atomic_t;
typedef unsigned long sigset_t;

/* Signal numbers that WARP references — Linux/x86_64 values.  Never raised by
 * the kernel; they exist so the handler-installation code compiles. */
#define SIGSEGV 11
#define SIGFPE 8
#define SIGBUS 7

/* SA_SIGINFO flag — enables the three-argument sigaction handler form. */
#define SA_SIGINFO 4

/* siginfo_t — minimal subset; si_addr is used by WARP's fault handler.  The
 * field order is this file's own, not the platform ABI's, which is harmless
 * only because nothing ever fills one in. */
typedef struct {
    int si_signo;  /* Signal number */
    int si_code;   /* Signal code   */
    void* si_addr; /* Faulting address (SIGSEGV / SIGBUS) */
} siginfo_t;

/* struct sigaction — only the three-argument sa_sigaction form.  There is no
 * sa_handler member and no sa_restorer, so the one-argument handler style does
 * not compile. */
struct sigaction {
    /* Three-argument handler: signo, siginfo, ucontext (unused). */
    void (*sa_sigaction)(int, siginfo_t*, void*);
    int sa_flags;
    sigset_t sa_mask;
};

#ifdef __cplusplus
extern "C" {
#endif

/* No-op implementation — declared here, defined in
 * src/kernel/warp/posix_kernel.c. */
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact);

#ifdef __cplusplus
} /* extern "C" */
#endif
