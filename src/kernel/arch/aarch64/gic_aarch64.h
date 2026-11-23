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

/* SPI range (shared peripheral interrupts, GICv3 2.2.1). */
#define GIC_SPI_FIRST 32u
#define GIC_SPI_END 1020u

/* Whether intid is an SPI the distributor implements. */
int gic_spi_valid(uint32_t intid);

/* Make SPI `intid` a Group 1 interrupt routed to the boot CPU at the default
 * priority, level-sensitive (`level` non-zero) or edge-triggered, and leave it
 * disabled. */
void gic_spi_configure(uint32_t intid, int level);

/* Enable / disable delivery of SPI `intid` at the distributor. */
void gic_spi_enable(uint32_t intid);
void gic_spi_disable(uint32_t intid);

/* Whether SPI `intid` is enabled at the distributor. */
int gic_spi_enabled(uint32_t intid);

/* First LPI INTID (GICv3: 8192 and up are LPIs). */
#define GIC_LPI_BASE 8192u

/* Physical address of the boot CPU's redistributor (RD_base), and its
 * GICR_TYPER.Processor_Number: the two forms an ITS collection target takes,
 * selected by GITS_TYPER.PTA. */
uint64_t gic_redistributor_phys(void);
uint32_t gic_redistributor_processor_number(void);

/* Enable LPIs on the boot CPU's redistributor, with the LPI configuration table
 * at `prop_phys` covering INTIDs below 2^id_bits and the pending table at
 * `pend_phys` (64 KiB aligned, zeroed). Returns 0, or -1 when the
 * redistributor implements no LPIs. Call once, before any LPI is mapped. */
int gic_lpi_enable(uint64_t prop_phys, uint32_t id_bits, uint64_t pend_phys);
