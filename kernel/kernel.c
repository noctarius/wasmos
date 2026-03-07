#include "boot.h"
#include <stdint.h>

extern void wasm_runtime_init(void);

static void hang(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void kmain(boot_info_t *boot_info) {
    (void)boot_info;

    // Placeholder: initialize memory management, drivers, then WAMR.
    wasm_runtime_init();

    hang();
}
