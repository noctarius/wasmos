/* unimplemented_aarch64.c - the arch interfaces the aarch64 port does not
 * implement yet. Each one fails by name, through kpanic, the first time it is
 * reached, so a boot stops at the first missing piece instead of running on
 * with a silently wrong answer.
 *
 * TODO: line interrupt routing behind irq.h (arm64 port, step 10 and Phase 3). */
#include <stdint.h>

#include "irq.h"
#include "klog.h"
#include "kpanic.h"

static __attribute__((noreturn)) void aarch64_unimplemented(const char* what) {
    klog_printf("[aarch64] not implemented: %s\n", what);
    kpanic(what, 0ULL, 0ULL);
}

int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    (void)context_id;
    (void)irq_line;
    (void)endpoint;
    aarch64_unimplemented("irq_register");
}

int irq_unregister(uint32_t context_id, uint32_t irq_line) {
    (void)context_id;
    (void)irq_line;
    aarch64_unimplemented("irq_unregister");
}

int irq_ack(uint32_t context_id, uint32_t irq_line) {
    (void)context_id;
    (void)irq_line;
    aarch64_unimplemented("irq_ack");
}

int irq_configure(uint32_t irq_line, uint32_t flags) {
    (void)irq_line;
    (void)flags;
    aarch64_unimplemented("irq_configure");
}
