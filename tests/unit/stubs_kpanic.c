/* kpanic for the host tests.
 *
 * The kernel's kpanic dumps CPU state and halts, which a host process cannot do
 * and would not want to: a test that reaches it has hit a condition the kernel
 * calls fatal, and the useful outcome is a failed test with the reason on
 * stdout rather than a hung runner. Aborting gives both -- the harness sees a
 * non-zero exit and the message is already printed.
 */

#include <stdio.h>
#include <stdlib.h>

#include "kpanic.h"

__attribute__((noreturn)) void kpanic(const char* reason, uint64_t a, uint64_t b) {
    printf("  [KPANIC] %s (a=%llu b=%llu)\n", reason ? reason : "(null)", (unsigned long long)a,
           (unsigned long long)b);
    fflush(0);
    abort();
}

void kpanic_capture_origin(uint64_t rip, uint64_t rsp, uint64_t rbp, uint64_t rflags, uint64_t cs) {
    (void)rip;
    (void)rsp;
    (void)rbp;
    (void)rflags;
    (void)cs;
}
