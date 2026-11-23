/* phys_map_unmapped - test fixture: phys_map on memory nothing backs.
 *
 * Usage: phys_map_unmapped
 *
 * Holds mmio.map and asks wasmos_phys_map to copy one page from a physical
 * address beyond the guest's RAM, below any device window, on both QEMU
 * machines the tests boot (512 MiB of RAM at 0 on x86_64 pc, at 1 GiB on
 * aarch64 virt). tests/test_phys_map_unmapped.py runs it.
 *
 * Prints "phys-map-unmapped: refused code=N" with the packed code the kernel
 * returned, or "phys-map-unmapped: copied" when the runtime read the page (WARP
 * reads through a remapped scratch page, which reaches any physical address). */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"

/* Beyond 512 MiB of RAM at 0 (x86_64) and at 0x40000000 (aarch64), and below
 * either machine's PCI and device windows. */
#define UNBACKED_PHYS 0x70000000u

static uint8_t g_window[4096] __attribute__((aligned(4096)));

int main(void) {
    int32_t rc = wasmos_phys_map(
        (int32_t)UNBACKED_PHYS, 0, (int32_t)sizeof(g_window), (int32_t)(uintptr_t)g_window);
    if (rc == 0) {
        puts("phys-map-unmapped: copied");
        return 0;
    }
    printf("phys-map-unmapped: refused code=%d\n", (int)rc);
    return 0;
}
