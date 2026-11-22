/* gic_aarch64.h - the GICv3 interrupt controller of QEMU virt (arm64 port).
 * Private to arch/aarch64. INTIDs follow the GICv3 architecture: 0-15 SGIs,
 * 16-31 PPIs (per CPU), 32-1019 SPIs, 1020-1023 special. */
#pragma once

#include <stdint.h>

#define GIC_INTID_SPURIOUS 1023u

/* Bring the distributor, the boot CPU's redistributor and its system-register
 * CPU interface up with every interrupt disabled. Call once, with IRQs masked,
 * before any other gic_* call. */
void gic_init(void);

/* Enable a PPI (16..31) on the calling CPU at the default priority. */
void gic_enable_ppi(uint32_t intid);

/* Acknowledge the highest-priority pending interrupt and return its INTID
 * (GIC_INTID_SPURIOUS and the other special values when there is none). */
uint32_t gic_ack(void);

/* Signal the end of an acknowledged interrupt. */
void gic_eoi(uint32_t intid);
