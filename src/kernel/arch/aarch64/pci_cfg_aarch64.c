/* pci_cfg_aarch64.c - PCI configuration space through ECAM (PCI Firmware
 * Specification 3.3, 4.1.2), located by the ACPI MCFG. Implements
 * arch/arch_pci.h; pci_config.c serialises every access.
 *
 * The window is far above the direct map on QEMU virt (0x4010000000), so each
 * bus's 1 MiB is mapped Device-nGnRE into KERNEL_ECAM_VA the first time a
 * function on it is touched, and stays mapped. Only the MCFG's first
 * allocation (segment 0) is used. A function outside the decoded bus range
 * reads as absent (all ones) and ignores writes, which is what an empty slot
 * looks like through ECAM itself.
 *
 * Each access is one 32-bit load or store through arch_mmio_*, which a
 * hypervisor can emulate from the fault syndrome. */
#include "arch/arch_pci.h"

#include "acpi_aarch64.h"
#include "arch/arch_mmio.h"
#include "arch/vm_layout.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"

/* MCFG (PCI Firmware 3.3, 4.1.2): the SDT header, 8 reserved bytes, then
 * 16-byte allocations. */
typedef struct {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed)) mcfg_allocation_t;

#define MCFG_ALLOCATIONS_OFFSET (sizeof(acpi_sdt_header_t) + 8u)
#define ECAM_BUS_BYTES (1ULL << 20)
#define ECAM_PAGE 0x1000ULL

static uint64_t g_ecam_phys;
static uint8_t g_ecam_start_bus;
static uint8_t g_ecam_end_bus;
static uint8_t g_ecam_probed;
static uint8_t g_ecam_bus_mapped[256];

static void ecam_probe(void) {
    g_ecam_probed = 1;
    const acpi_sdt_header_t* mcfg = acpi_find_table("MCFG");
    if (!mcfg || mcfg->length < MCFG_ALLOCATIONS_OFFSET + sizeof(mcfg_allocation_t)) {
        klog_write("[pci] no ACPI MCFG: configuration space unavailable\n");
        return;
    }
    mcfg_allocation_t alloc;
    __builtin_memcpy(&alloc, (const uint8_t*)mcfg + MCFG_ALLOCATIONS_OFFSET, sizeof(alloc));
    uint64_t span = (uint64_t)(alloc.end_bus - alloc.start_bus + 1u) * ECAM_BUS_BYTES;
    if (span > KERNEL_ECAM_BYTES) {
        klog_write("[pci] MCFG window exceeds the ECAM VA span\n");
        return;
    }
    g_ecam_phys = alloc.base;
    g_ecam_start_bus = alloc.start_bus;
    g_ecam_end_bus = alloc.end_bus;
    klog_printf("[pci] ECAM base=%016llx buses %u-%u\n",
                (unsigned long long)g_ecam_phys,
                (unsigned int)g_ecam_start_bus,
                (unsigned int)g_ecam_end_bus);
}

/* Kernel VA of `reg` of function `bdf`, mapping its bus on first use, or 0
 * when the function lies outside the ECAM window. */
static uintptr_t ecam_address(uint16_t bdf, uint8_t reg) {
    if (!g_ecam_probed) {
        ecam_probe();
    }
    uint8_t bus = (uint8_t)(bdf >> 8);
    if (!g_ecam_phys || bus < g_ecam_start_bus || bus > g_ecam_end_bus) {
        return 0;
    }
    uint64_t bus_offset = (uint64_t)(bus - g_ecam_start_bus) * ECAM_BUS_BYTES;
    if (!g_ecam_bus_mapped[bus]) {
        for (uint64_t off = 0; off < ECAM_BUS_BYTES; off += ECAM_PAGE) {
            if (paging_map_4k(KERNEL_ECAM_VA + bus_offset + off,
                              g_ecam_phys + bus_offset + off,
                              MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                  MEM_REGION_FLAG_DEVICE) != 0) {
                klog_printf("[pci] ECAM map failed bus=%u\n", (unsigned int)bus);
                return 0;
            }
        }
        g_ecam_bus_mapped[bus] = 1;
    }
    /* bdf's low byte is device << 3 | function: ECAM offset bits 19:12. */
    return (uintptr_t)(KERNEL_ECAM_VA + bus_offset + ((uint64_t)(bdf & 0xFFu) << 12) +
                       (reg & 0xFCu));
}

uint32_t arch_pci_cfg_read32(uint16_t bdf, uint8_t reg) {
    uintptr_t addr = ecam_address(bdf, reg);
    return addr ? arch_mmio_read32(addr) : 0xFFFFFFFFu;
}

void arch_pci_cfg_write32(uint16_t bdf, uint8_t reg, uint32_t value) {
    uintptr_t addr = ecam_address(bdf, reg);
    if (addr) {
        arch_mmio_write32(addr, value);
    }
}
