/* acpi_aarch64.c - XSDT walk over the firmware's ACPI tables. The tables live
 * in RAM below KERNEL_HIGHER_HALF_WINDOW_BYTES on QEMU virt and are read
 * through the direct map; the RSDP itself is the kernel's shadow copy (see
 * kernel_boot_build_bootinfo_shadow), already a kernel pointer. */
#include "acpi_aarch64.h"

#include "arch/vm_layout.h"

static const uint8_t* g_rsdp;
static uint32_t g_rsdp_length;

/* RSDP 2.0 layout (ACPI 6.5, 5.2.5.3): revision at 15, XsdtAddress at 24. */
#define RSDP_REVISION_OFFSET 15u
#define RSDP_XSDT_OFFSET 24u
#define RSDP_V2_LENGTH 36u

void acpi_init(const boot_info_t* boot_info) {
    if (!boot_info || !boot_info->rsdp || boot_info->rsdp_length < RSDP_V2_LENGTH) {
        return;
    }
    g_rsdp = (const uint8_t*)boot_info->rsdp;
    g_rsdp_length = boot_info->rsdp_length;
}

static const void* phys_to_kernel(uint64_t phys) {
    if (phys == 0 || phys >= KERNEL_HIGHER_HALF_WINDOW_BYTES) {
        return 0;
    }
    return ptr_cast(void, (phys | KERNEL_HIGHER_HALF_BASE));
}

const acpi_sdt_header_t* acpi_find_table(const char signature[4]) {
    if (!g_rsdp || g_rsdp[RSDP_REVISION_OFFSET] < 2u) {
        return 0;
    }
    uint64_t xsdt_phys;
    __builtin_memcpy(&xsdt_phys, g_rsdp + RSDP_XSDT_OFFSET, sizeof(xsdt_phys));
    const acpi_sdt_header_t* xsdt = (const acpi_sdt_header_t*)phys_to_kernel(xsdt_phys);
    if (!xsdt || xsdt->length < sizeof(*xsdt)) {
        return 0;
    }
    uint32_t count = (uint32_t)((xsdt->length - sizeof(*xsdt)) / 8u);
    const uint8_t* entries = (const uint8_t*)xsdt + sizeof(*xsdt);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t table_phys;
        __builtin_memcpy(&table_phys, entries + (uint64_t)i * 8u, sizeof(table_phys));
        const acpi_sdt_header_t* table = (const acpi_sdt_header_t*)phys_to_kernel(table_phys);
        if (table && __builtin_memcmp(table->signature, signature, 4) == 0) {
            return table;
        }
    }
    return 0;
}
