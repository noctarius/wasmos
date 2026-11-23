/* phys_map_acpi - test fixture: phys_map of a firmware table, then exit.
 *
 * Usage: phys_map_acpi
 *
 * Holds mmio.map, finds the root system description table through the RSDP
 * the kernel hands over (the XSDT, or the RSDT before ACPI 2.0), maps it with
 * wasmos_phys_map, reads its signature through the mapping, maps it a second
 * time, and exits with the windows still mapped, so the kernel unmaps them at
 * reap. tests/test_phys_map.py runs it more than once.
 *
 * Prints "phys-map-acpi: <SIG> again=<same|moved>" when both maps succeed,
 * where <same> means the second request resolved to the first window, or
 * "phys-map-acpi: refused code=N" / "phys-map-acpi: no rsdp". */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"

typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} rsdp_t;

static int32_t map(uint64_t phys, int32_t size) {
    return wasmos_phys_map((int32_t)(uint32_t)phys, (int32_t)(uint32_t)(phys >> 32), size);
}

int main(void) {
    rsdp_t rsdp;
    uint32_t rsdp_len = 0;
    if (wasmos_acpi_rsdp_info(
            (int32_t)(uintptr_t)&rsdp, (int32_t)(uintptr_t)&rsdp_len, (int32_t)sizeof(rsdp)) != 0 ||
        wasmos_sync_user_read((int32_t)(uintptr_t)&rsdp, (int32_t)sizeof(rsdp)) != 0) {
        puts("phys-map-acpi: no rsdp");
        return 0;
    }
    uint64_t root = (rsdp.revision >= 2 && rsdp.xsdt_address != 0) ? rsdp.xsdt_address
                                                                   : (uint64_t)rsdp.rsdt_address;
    int32_t off = map(root, 36);
    if (off < 0) {
        printf("phys-map-acpi: refused code=%d\n", (int)off);
        return 0;
    }
    int32_t again = map(root + 4, 4);
    if (again < 0) {
        printf("phys-map-acpi: refused code=%d\n", (int)again);
        return 0;
    }
    const char* sig = (const char*)(uintptr_t)(uint32_t)off;
    printf("phys-map-acpi: %c%c%c%c again=%s\n",
           sig[0],
           sig[1],
           sig[2],
           sig[3],
           again == off + 4 ? "same" : "moved");
    return 0;
}
