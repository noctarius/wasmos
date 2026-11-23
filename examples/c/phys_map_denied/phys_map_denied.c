/* phys_map_denied - test fixture: phys_map without the mmio.map capability.
 *
 * Usage: phys_map_denied
 *
 * Holds only ipc.basic and asks wasmos_phys_map for one page at physical
 * address 1 MiB. tests/test_phys_map.py runs it.
 *
 * Prints "phys-map-denied: refused code=N" with the packed code the kernel
 * returned, or "phys-map-denied: mapped" when the call succeeded. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"

#define BACKED_PHYS_LO 0x100000u

int main(void) {
    int32_t rc = wasmos_phys_map((int32_t)BACKED_PHYS_LO, 0, 4096);
    if (rc >= 0) {
        puts("phys-map-denied: mapped");
        return 0;
    }
    printf("phys-map-denied: refused code=%d\n", (int)rc);
    return 0;
}
