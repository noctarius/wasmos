#pragma once
/* compat/sys/signal.h — freestanding signal stub for bare-metal kernel.
 *
 * WARP includes signal headers to declare sigaction() for its optional
 * SIGSEGV-based fault recovery path.  In the kernel there is no signal
 * delivery mechanism; the declarations are present only so the code
 * compiles.  The sigaction() implementation returns 0 (success, no-op). */

#include <stdint.h>

/* Basic signal-related types. */
typedef int           sig_atomic_t;
typedef unsigned long sigset_t;

/* Signal numbers that WARP references. */
#define SIGSEGV  11
#define SIGFPE    8
#define SIGBUS    7

/* SA_SIGINFO flag — enables the three-argument sigaction handler form. */
#define SA_SIGINFO  4

/* siginfo_t — minimal subset; si_addr is used by WARP's fault handler. */
typedef struct {
    int   si_signo;   /* Signal number */
    int   si_code;    /* Signal code   */
    void *si_addr;    /* Faulting address (SIGSEGV / SIGBUS) */
} siginfo_t;

/* struct sigaction */
struct sigaction {
    /* Three-argument handler: signo, siginfo, ucontext (unused). */
    void     (*sa_sigaction)(int, siginfo_t *, void *);
    int        sa_flags;
    sigset_t   sa_mask;
};

#ifdef __cplusplus
extern "C" {
#endif

/* No-op implementation — declared here, defined in warp/compat/csignal.cpp
 * (or in a kernel stub object linked alongside WARP). */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#ifdef __cplusplus
} /* extern "C" */
#endif
