/* phys_map_refused - test fixture: phys_map on memory that is not a firmware table.
 *
 * Usage: phys_map_refused
 *
 * Holds mmio.map and asks wasmos_phys_map for two ranges no caller may map:
 * the page at 1 MiB, which is system RAM on both QEMU machines the tests boot,
 * and the page at 4 GiB, which nothing backs. tests/test_phys_map.py runs it.
 *
 * Prints "phys-map-refused: <ram|unbacked> refused code=N" with the packed code
 * the kernel returned, or "phys-map-refused: <ram|unbacked> mapped" when the
 * call succeeded. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"

#define RAM_PHYS 0x100000ull
#define UNBACKED_PHYS 0x100000000ull

static void probe(const char* label, uint64_t phys) {
    int32_t rc = wasmos_phys_map((int32_t)(uint32_t)phys, (int32_t)(uint32_t)(phys >> 32), 4096);
    if (rc >= 0) {
        printf("phys-map-refused: %s mapped\n", label);
        return;
    }
    printf("phys-map-refused: %s refused code=%d\n", label, (int)rc);
}

int main(void) {
    probe("ram", RAM_PHYS);
    probe("unbacked", UNBACKED_PHYS);
    return 0;
}
