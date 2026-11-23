/* phys_map_acpi - test fixture: phys_map of firmware tables, then exit.
 *
 * Usage: phys_map_acpi
 *
 * Holds mmio.map, finds the root system description table through the RSDP
 * the kernel hands over (the XSDT, or the RSDT before ACPI 2.0), maps it with
 * wasmos_phys_map, reads its signature through the mapping, maps a range inside
 * it a second time, and then follows FACP to the DSDT to check exact bytes: an
 * ACPI table's bytes sum to 0 mod 256 over its length, a reference the mapping
 * cannot fake. The DSDT is read starting at a non-page-aligned address and
 * across at least one page boundary. The process exits with every window still
 * mapped, so the kernel unmaps them at reap. tests/test_phys_map.py runs it
 * more than once.
 *
 * Prints, when every map succeeds:
 *   "phys-map-acpi: <SIG> again=<same|moved> root_page_off=0x<N>"
 *     <same>: the second request resolved to the first window.
 *   "phys-map-acpi: dsdt unaligned cross-page checksum=<ok|bad> start_page_off=0x<N>"
 * and otherwise "phys-map-acpi: refused code=N", "phys-map-acpi: no rsdp",
 * "phys-map-acpi: no dsdt" or "phys-map-acpi: dsdt too small". */
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

/* The common ACPI table header: signature at 0, length at 4. */
#define SDT_HEADER_LEN 36u

static const uint8_t* map(uint64_t phys, uint32_t size) {
    int32_t off =
        wasmos_phys_map((int32_t)(uint32_t)phys, (int32_t)(uint32_t)(phys >> 32), (int32_t)size);
    if (off < 0) {
        printf("phys-map-acpi: refused code=%d\n", (int)off);
        return 0;
    }
    return (const uint8_t*)(uintptr_t)(uint32_t)off;
}

static uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t* p) {
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static uint8_t sum(const uint8_t* p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; ++i) {
        s = (uint8_t)(s + p[i]);
    }
    return s;
}

/* The DSDT's physical address through the root table's FACP entry, or 0. */
static uint64_t find_dsdt(const uint8_t* root, int xsdt) {
    uint32_t entry_size = xsdt ? 8u : 4u;
    uint32_t entries = (read_u32(root + 4) - SDT_HEADER_LEN) / entry_size;
    for (uint32_t i = 0; i < entries; ++i) {
        const uint8_t* e = root + SDT_HEADER_LEN + i * entry_size;
        uint64_t table = xsdt ? read_u64(e) : (uint64_t)read_u32(e);
        const uint8_t* hdr = map(table, SDT_HEADER_LEN);
        if (!hdr || hdr[0] != 'F' || hdr[1] != 'A' || hdr[2] != 'C' || hdr[3] != 'P') {
            continue;
        }
        uint32_t len = read_u32(hdr + 4);
        const uint8_t* facp = map(table, len);
        if (!facp) {
            return 0;
        }
        uint64_t dsdt = len >= 148u ? read_u64(facp + 140) : 0;
        if (dsdt == 0 && len >= 44u) {
            dsdt = read_u32(facp + 40);
        }
        return dsdt;
    }
    return 0;
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
    int xsdt = rsdp.revision >= 2 && rsdp.xsdt_address != 0;
    uint64_t root_phys = xsdt ? rsdp.xsdt_address : (uint64_t)rsdp.rsdt_address;
    const uint8_t* hdr = map(root_phys, SDT_HEADER_LEN);
    if (!hdr) {
        return 0;
    }
    const uint8_t* again = map(root_phys + 4, 4);
    if (!again) {
        return 0;
    }
    printf("phys-map-acpi: %c%c%c%c again=%s root_page_off=0x%x\n",
           hdr[0],
           hdr[1],
           hdr[2],
           hdr[3],
           again == hdr + 4 ? "same" : "moved",
           (unsigned)(root_phys & 0xFFFu));

    const uint8_t* root = map(root_phys, read_u32(hdr + 4));
    if (!root) {
        return 0;
    }
    uint64_t dsdt = find_dsdt(root, xsdt);
    if (dsdt == 0) {
        puts("phys-map-acpi: no dsdt");
        return 0;
    }
    const uint8_t* dsdt_hdr = map(dsdt, SDT_HEADER_LEN);
    if (!dsdt_hdr) {
        return 0;
    }
    uint32_t len = read_u32(dsdt_hdr + 4);
    /* A split point that is not page-aligned and leaves a tail longer than a
     * page, so the tail starts mid-page and crosses a page boundary. */
    uint32_t split = 0x123u;
    if (((dsdt + split) & 0xFFFu) == 0) {
        split++;
    }
    if (len <= split + 0x1000u) {
        puts("phys-map-acpi: dsdt too small");
        return 0;
    }
    const uint8_t* tail = map(dsdt + split, len - split);
    const uint8_t* head = map(dsdt, split);
    if (!tail || !head) {
        return 0;
    }
    uint8_t total = (uint8_t)(sum(head, split) + sum(tail, len - split));
    printf("phys-map-acpi: dsdt unaligned cross-page checksum=%s start_page_off=0x%x\n",
           total == 0 ? "ok" : "bad",
           (unsigned)((dsdt + split) & 0xFFFu));
    return 0;
}
