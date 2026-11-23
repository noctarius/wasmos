/* acpi_aarch64.h - ACPI table lookup for the aarch64 platform code (MCFG now,
 * MADT/SPCR/GTDT as they replace QEMU virt's fixed addresses). Private to
 * arch/aarch64. */
#pragma once

#include <stdint.h>

#include "boot.h"

/* Header every ACPI system description table starts with (ACPI 6.5, 5.2.6). */
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* Record the RSDP from boot_info (the kernel's shadow copy). Called once from
 * irq_late_init, before any table is looked up. */
void acpi_init(const boot_info_t* boot_info);

/* The first table with `signature` listed in the XSDT, through the kernel
 * direct map, or 0 when there is no RSDP, no XSDT or no such table. */
const acpi_sdt_header_t* acpi_find_table(const char signature[4]);
