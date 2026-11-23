/* phys_map_denied - test fixture: phys_map without the mmio.map capability.
 *
 * Usage: phys_map_denied
 *
 * Holds only ipc.basic and asks wasmos_phys_map to copy one page from physical
 * address 1 MiB, which is RAM on both QEMU machines the tests boot, so only the
 * capability check stands between the caller and the copy.
 * tests/test_phys_map.py runs it.
 *
 * Prints "phys-map-denied: refused code=N" with the packed code the kernel
 * returned, or "phys-map-denied: copied" when the runtime performed the copy. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"

#define BACKED_PHYS_LO 0x100000u

static uint8_t g_window[4096] __attribute__((aligned(4096)));

int main(void) {
    int32_t rc = wasmos_phys_map(
        (int32_t)BACKED_PHYS_LO, 0, (int32_t)sizeof(g_window), (int32_t)(uintptr_t)g_window);
    if (rc == 0) {
        puts("phys-map-denied: copied");
        return 0;
    }
    printf("phys-map-denied: refused code=%d\n", (int)rc);
    return 0;
}
