/* unimplemented_aarch64.c - the arch interfaces the aarch64 port does not
 * implement yet. Each one fails by name, through kpanic, the first time it is
 * reached, so a boot stops at the first missing piece instead of running on
 * with a silently wrong answer.
 *
 * TODO: device interrupt routing behind irq.h and msi.h (arm64 port, step 10
 * and Phase 3) and ECAM configuration access (Phase 3). */
#include <stdint.h>

#include "arch/arch_pci.h"
#include "irq.h"
#include "klog.h"
#include "kpanic.h"
#include "msi.h"

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

int msi_alloc(uint32_t context_id, uint32_t endpoint, uint32_t* out_address_lo,
              uint32_t* out_address_hi, uint32_t* out_data, uint32_t* out_vector) {
    (void)context_id;
    (void)endpoint;
    (void)out_address_lo;
    (void)out_address_hi;
    (void)out_data;
    (void)out_vector;
    aarch64_unimplemented("msi_alloc");
}

int msi_free(uint32_t context_id, uint32_t vector) {
    (void)context_id;
    (void)vector;
    aarch64_unimplemented("msi_free");
}

uint32_t arch_pci_cfg_read32(uint16_t bdf, uint8_t reg) {
    (void)bdf;
    (void)reg;
    aarch64_unimplemented("arch_pci_cfg_read32");
}

void arch_pci_cfg_write32(uint16_t bdf, uint8_t reg, uint32_t value) {
    (void)bdf;
    (void)reg;
    (void)value;
    aarch64_unimplemented("arch_pci_cfg_write32");
}
