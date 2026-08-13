#include "kallsyms.h"

#include <stdint.h>

typedef struct {
    uint64_t addr;
    uint32_t name_off;
} kernel_kallsyms_entry_t;

/* Empty weak defaults so the kernel links before the symbol table exists.  The
 * real table is generated from the linked image by scripts/gen_kernel_kallsyms.py
 * and linked in a second pass, where it overrides these definitions.  Its entries
 * are sorted by ascending address (nm -n), which is what the search below
 * requires. */
__attribute__((weak)) const char g_kernel_kallsyms_names[] = "";
__attribute__((weak)) const kernel_kallsyms_entry_t g_kernel_kallsyms[] = {
    {0ULL, 0u},
};
__attribute__((weak)) const uint32_t g_kernel_kallsyms_count = 0u;

/* Resolve addr to the last symbol starting at or below it.  Returns 1 and fills
 * the requested outputs when one is found, 0 when the table is empty or addr
 * precedes every symbol — note this is a found/not-found flag, not a 0-success
 * status.  There is no upper bound on the match, so an address past the last
 * symbol still resolves to it. */
int kpanic_symbolize(uint64_t addr, const char** name, uint64_t* sym_addr) {
    uint32_t lo = 0;
    uint32_t hi = g_kernel_kallsyms_count;
    uint32_t match = 0;
    int found = 0;

    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) / 2u);
        uint64_t base = g_kernel_kallsyms[mid].addr;
        if (base <= addr) {
            match = mid;
            found = 1;
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }

    if (!found) {
        return 0;
    }
    if (name) {
        *name = g_kernel_kallsyms_names + g_kernel_kallsyms[match].name_off;
    }
    if (sym_addr) {
        *sym_addr = g_kernel_kallsyms[match].addr;
    }
    return 1;
}
