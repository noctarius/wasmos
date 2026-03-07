#include "boot.h"
#include "serial.h"
#include <stdint.h>
#include "wamr_runtime.h"

static void hang(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void kmain(boot_info_t *boot_info) {
    (void)boot_info;

    serial_init();
    serial_write("[kernel] kmain\n");

    // Placeholder: initialize memory management, drivers, then WAMR.
    wamr_runtime_init();

    hang();
}
