/* unimplemented_aarch64.c - the arch interfaces the aarch64 port does not
 * implement yet. Each one fails by name, through kpanic, the first time it is
 * reached, so a boot stops at the first missing piece instead of running on
 * with a silently wrong answer.
 *
 * TODO: the MMU (paging.h; arm64 port, Phase 2), the context switch and
 * preemption trampoline (Phase 2), the GIC behind irq.h and msi.h (Phase 2),
 * the generic timer (Phase 2) and ECAM configuration access (Phase 3). */
#include <stdint.h>

#include "arch/arch_context.h"
#include "arch/arch_pci.h"
#include "arch/arch_timer.h"
#include "arch/vm_layout.h"
#include "irq.h"
#include "klog.h"
#include "kpanic.h"
#include "msi.h"
#include "paging.h"

static __attribute__((noreturn)) void aarch64_unimplemented(const char* what) {
    klog_printf("[aarch64] not implemented: %s\n", what);
    kpanic(what, 0ULL, 0ULL);
}

int paging_init(void) {
    aarch64_unimplemented("paging_init");
}

uint64_t paging_get_higher_half_base(void) {
    return KERNEL_HIGHER_HALF_BASE;
}

uint64_t paging_get_root_table(void) {
    aarch64_unimplemented("paging_get_root_table");
}

uint64_t paging_get_current_root_table(void) {
    aarch64_unimplemented("paging_get_current_root_table");
}

int paging_switch_root(uint64_t root_table) {
    (void)root_table;
    aarch64_unimplemented("paging_switch_root");
}

int paging_create_address_space(uint64_t* out_root_table) {
    (void)out_root_table;
    aarch64_unimplemented("paging_create_address_space");
}

void paging_destroy_address_space(uint64_t root_table) {
    (void)root_table;
    aarch64_unimplemented("paging_destroy_address_space");
}

int paging_clone_low_slot_in_root(uint64_t root_table) {
    (void)root_table;
    aarch64_unimplemented("paging_clone_low_slot_in_root");
}

int paging_strip_low_slot_in_root(uint64_t root_table) {
    (void)root_table;
    aarch64_unimplemented("paging_strip_low_slot_in_root");
}

int paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)root_table;
    (void)virt;
    (void)phys;
    (void)flags;
    aarch64_unimplemented("paging_map_4k_in_root");
}

int paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt) {
    (void)root_table;
    (void)virt;
    aarch64_unimplemented("paging_unmap_4k_in_root");
}

int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)virt;
    (void)phys;
    (void)flags;
    aarch64_unimplemented("paging_map_4k");
}

int paging_unmap_4k(uint64_t virt) {
    (void)virt;
    aarch64_unimplemented("paging_unmap_4k");
}

void paging_invalidate(uint64_t virt) {
    (void)virt;
    aarch64_unimplemented("paging_invalidate");
}

uint64_t paging_virt_to_phys_in_root(uint64_t root_table, uint64_t virt) {
    (void)root_table;
    (void)virt;
    aarch64_unimplemented("paging_virt_to_phys_in_root");
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    (void)virt;
    aarch64_unimplemented("paging_virt_to_phys");
}

int paging_verify_user_root(uint64_t root_table, int log_failures) {
    (void)root_table;
    (void)log_failures;
    aarch64_unimplemented("paging_verify_user_root");
}

int paging_verify_user_root_no_low_slot(uint64_t root_table, int log_failures) {
    (void)root_table;
    (void)log_failures;
    aarch64_unimplemented("paging_verify_user_root_no_low_slot");
}

void paging_dump_user_root_kernel_mappings(uint64_t root_table) {
    (void)root_table;
    aarch64_unimplemented("paging_dump_user_root_kernel_mappings");
}

void context_switch(process_context_t* out, process_context_t* in) {
    (void)out;
    (void)in;
    aarch64_unimplemented("context_switch");
}

void context_switch_to(process_context_t* in) {
    (void)in;
    aarch64_unimplemented("context_switch_to");
}

/* process.c takes the address of the resume label inside context_switch; this
 * byte stands in for it until the assembly context switch exists. */
uint8_t context_switch_resume;

void process_preempt_trampoline(void) {
    aarch64_unimplemented("process_preempt_trampoline");
}

void irq_late_init(const boot_info_t* boot_info) {
    (void)boot_info;
    aarch64_unimplemented("irq_late_init");
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

void irq_release_context(uint32_t context_id) {
    (void)context_id;
    aarch64_unimplemented("irq_release_context");
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

void msi_release_context(uint32_t context_id) {
    (void)context_id;
    aarch64_unimplemented("msi_release_context");
}

void arch_timer_start(uint32_t hz) {
    (void)hz;
    aarch64_unimplemented("arch_timer_start");
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
