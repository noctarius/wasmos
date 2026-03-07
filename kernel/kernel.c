#include "boot.h"
#include "memory.h"
#include "serial.h"
#include <stdint.h>
#include "wamr_runtime.h"

extern char __bss_start;
extern char __bss_end;

static void hang(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void write_hex(const char *label, uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[64];
    int idx = 0;
    for (const char *p = label; *p && idx < (int)sizeof(buf) - 1; ++p) {
        buf[idx++] = *p;
    }
    if (idx + 19 >= (int)sizeof(buf)) {
        buf[idx] = '\0';
        serial_write(buf);
        return;
    }
    buf[idx++] = '0';
    buf[idx++] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[idx++] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[idx++] = '\n';
    buf[idx] = '\0';
    serial_write(buf);
}

void kmain(boot_info_t *boot_info) {
    (void)boot_info;

    serial_init();
    serial_write("[kernel] kmain\n");
    write_hex("[kernel] __bss_start=", (uint64_t)(uintptr_t)&__bss_start);
    write_hex("[kernel] __bss_end=", (uint64_t)(uintptr_t)&__bss_end);
    write_hex("[kernel] __bss_size=", (uint64_t)((uintptr_t)&__bss_end - (uintptr_t)&__bss_start));

    mm_init(boot_info);

    // Placeholder: initialize memory management, drivers, then WAMR.
    wamr_runtime_init();

    hang();
}
