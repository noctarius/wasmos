/* kpanic.h - Unified kernel panic: stop the whole machine and dump every CPU.
 *
 * kpanic() is the single fatal-error entry point. It:
 *   1. wins a global first-panicker race (so concurrent panics don't interleave),
 *   2. NMI-IPIs every other CPU (NMI cannot be masked by cli, so even a CPU
 *      spinning with interrupts disabled is stopped),
 *   3. each stopped CPU snapshots its interrupted register context/stack into a
 *      per-CPU slot and halts,
 *   4. the panicking CPU dumps the reason plus every CPU's context + backtrace,
 *   5. halts forever.
 *
 * x86_nmi_handler() is the C side of the NMI ISR (isr_nmi in cpu_isr.S). During
 * a panic it performs the per-CPU capture; outside a panic an NMI is unexpected
 * and it just logs and returns.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stop the machine.  `reason` is a short NUL-terminated description and `a`/`b` are two
 * free-form context values printed as hex alongside it (0 when there is nothing useful
 * to pass).  Disables interrupts immediately and never returns; a second caller — on any
 * CPU — halts silently so the first panicker's dump is not interleaved.  Callable from
 * any context, including an ISR and before scheduling exists. */
__attribute__((noreturn)) void kpanic(const char* reason, uint64_t a, uint64_t b);

/* Record the register context a fault came from, so a later kpanic reports the faulting
 * instruction instead of the panic call site.  Called by exception entry paths with the
 * values out of the IRET frame; the calling CPU's slot is filled in, and once captured it
 * is not overwritten by kpanic's own self-capture.  A CPU id past WASMOS_MAX_CPUS is
 * silently ignored. */
void kpanic_capture_origin(uint64_t rip, uint64_t rsp, uint64_t rbp, uint64_t rflags, uint64_t cs);

/* Called from isr_nmi with a pointer to the pushed register frame (PUSH_REGS
 * order followed by the CPU-pushed iret frame). */
void x86_nmi_handler(uint64_t* regs);

#ifdef __cplusplus
}
#endif
