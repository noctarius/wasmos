/* phys_map_unmapped - test fixture: phys_map on memory nothing backs.
 *
 * Usage: phys_map_unmapped
 *
 * Holds mmio.map and asks wasmos_phys_map to copy one page from physical
 * address 4 GiB: beyond the kernel's alias of physical memory on both
 * architectures (512 MiB on x86_64, 4 GiB on aarch64) and beyond the guest's
 * RAM on both QEMU machines the tests boot. tests/test_phys_map_unmapped.py
 * runs it.
 *
 * Prints "phys-map-unmapped: refused code=N" with the packed code the kernel
 * returned, or "phys-map-unmapped: copied" when the runtime read the page (WARP
 * reads through a remapped scratch page, which reaches any physical address). */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"

/* The first byte past either architecture's alias window. */
#define UNBACKED_PHYS_HI 1u

static uint8_t g_window[4096] __attribute__((aligned(4096)));

int main(void) {
    int32_t rc = wasmos_phys_map(
        0, (int32_t)UNBACKED_PHYS_HI, (int32_t)sizeof(g_window), (int32_t)(uintptr_t)g_window);
    if (rc == 0) {
        puts("phys-map-unmapped: copied");
        return 0;
    }
    printf("phys-map-unmapped: refused code=%d\n", (int)rc);
    return 0;
}
