#include "phys_alias.h"

int phys_alias_range_mapped(uint64_t phys, uint64_t size, uint64_t alias_base,
                            uint64_t window_bytes, uint64_t (*lookup)(uint64_t virt)) {
    /* Compared by subtraction: phys + size can wrap past 2^64. */
    if (phys >= window_bytes || size > window_bytes - phys) {
        return 0;
    }
    /* Every probed address is nonzero -- phys itself for its page, frame
     * boundaries from 0x1000 up after it -- so a 0 from lookup means unmapped
     * and never frame 0's translation. */
    for (uint64_t addr = phys; addr < phys + size; addr = (addr & ~0xFFFULL) + 0x1000ULL) {
        if (lookup(addr | alias_base) == 0) {
            return 0;
        }
    }
    return 1;
}
