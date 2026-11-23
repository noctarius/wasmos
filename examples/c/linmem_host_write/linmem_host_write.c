/* linmem_host_write - test fixture: a host call is the first to touch a page.
 *
 * Usage: linmem_host_write
 *
 * Hands the kernel an out pointer into linear memory past __heap_base that
 * nothing has touched yet, high enough to lie beyond the pages a WARP module
 * has mapped for ring 3 when it starts, then reads the result back. Under WARP
 * the kernel's write grows the module's live linear memory on the kernel side;
 * the read is the guest's own access to the grown range, which passes WARP's
 * bounds check without asking the kernel for anything.
 * tests/test_linmem_host_write.py runs it.
 *
 * Prints "linmem-host-write: ok dispatch=N" when the read returned what the
 * kernel wrote, "linmem-host-write: no room" or "linmem-host-write: call
 * failed" and exits 1 otherwise. */
#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"

/* The first byte past the module's static data and stack (wasm-ld). */
extern uint8_t __heap_base;

/* Inside the 128 KiB of linear memory the manifest declares, past the first
 * wasm page and past what a module starts with mapped. */
#define TARGET_OFFSET (96u * 1024u)
#define MEMORY_BYTES (128u * 1024u)

int main(void) {
    if ((uintptr_t)&__heap_base >= TARGET_OFFSET ||
        TARGET_OFFSET + sizeof(wasmos_sched_cpu_stats_t) > MEMORY_BYTES) {
        puts("linmem-host-write: no room");
        return 1;
    }
    wasmos_sched_cpu_stats_t* stats = (wasmos_sched_cpu_stats_t*)(uintptr_t)TARGET_OFFSET;
    if (wasmos_sched_cpu_stats(0, (int32_t)TARGET_OFFSET) != 0) {
        puts("linmem-host-write: call failed");
        return 1;
    }
    /* CPU 0 has dispatched threads since boot, so the count is non-zero once
     * the kernel's write is visible here. */
    printf("linmem-host-write: ok dispatch=%u\n", (unsigned)stats->dispatch_count);
    return 0;
}
