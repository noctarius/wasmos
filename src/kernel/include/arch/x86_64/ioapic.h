/* ioapic.h - I/O APIC driver declarations: init and GSI→vector mapping. */
#pragma once
#include "boot.h"
#include <stdint.h>

void ioapic_init(const boot_info_t *boot_info);
void ioapic_mask_irq(uint32_t irq_line);
void ioapic_unmask_irq(uint32_t irq_line);
