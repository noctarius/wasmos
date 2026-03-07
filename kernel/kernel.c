#include "boot.h"
#include "cpu.h"
#include "memory.h"
#include "serial.h"
#include <stdint.h>
#include "ipc.h"
#include "process.h"
#include "wamr_context.h"
#include "wamr_runtime.h"
#include "wasm_chardev.h"

static void serial_write_hex64(uint64_t value) {
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static process_run_result_t chardev_server_entry(process_t *process, void *arg) {
    (void)process;
    (void)arg;

    int rc = wasm_chardev_service_once();
    if (rc == 0) {
        return PROCESS_RUN_YIELDED;
    }
    if (rc == 1) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }
    return PROCESS_RUN_IDLE;
}

static void run_kernel_loop(void) {
    for (;;) {
        if (process_schedule_once() != 0) {
            __asm__ volatile("pause");
        }
    }
}

void kmain(boot_info_t *boot_info) {
    (void)boot_info;

    serial_init();
    serial_write("[kernel] kmain\n");
    cpu_init();

    mm_init(boot_info);
    ipc_init();
    process_init();

    // Keep boot path non-blocking: WAMR init is deferred until platform stubs
    // are complete enough for runtime initialization.
    serial_write("[kernel] wamr init deferred\n");

    uint32_t chardev_pid = 0;
    if (process_spawn("chardev-server", chardev_server_entry, 0, &chardev_pid) != 0) {
        serial_write("[kernel] chardev process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    serial_write("[kernel] chardev pid=");
    serial_write_hex64(chardev_pid);

    process_t *chardev_proc = process_get(chardev_pid);
    if (!chardev_proc || wasm_chardev_init(chardev_proc->context_id) != 0) {
        serial_write("[kernel] chardev service init failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    serial_write("[kernel] scheduler loop\n");

    run_kernel_loop();
}
