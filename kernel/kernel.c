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

typedef enum {
    CHARDEV_TEST_PHASE_INIT = 0,
    CHARDEV_TEST_PHASE_WAIT_WRITE,
    CHARDEV_TEST_PHASE_WAIT_READ
} chardev_test_phase_t;

typedef struct {
    uint32_t chardev_endpoint;
    uint32_t reply_endpoint;
    uint32_t write_request_id;
    uint32_t read_request_id;
    uint8_t write_value;
    chardev_test_phase_t phase;
} chardev_test_client_state_t;

static chardev_test_client_state_t g_chardev_test_client;

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

static process_run_result_t chardev_test_client_entry(process_t *process, void *arg) {
    chardev_test_client_state_t *state = (chardev_test_client_state_t *)arg;
    ipc_message_t resp;
    int rc;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }

    if (state->phase == CHARDEV_TEST_PHASE_INIT) {
        if (state->reply_endpoint == IPC_ENDPOINT_NONE &&
            ipc_endpoint_create(process->context_id, &state->reply_endpoint) != 0) {
            serial_write("[test] chardev client endpoint alloc failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }

        state->write_request_id = 1;
        if (wasm_chardev_ipc_write_request(process->context_id,
                                           state->chardev_endpoint,
                                           state->reply_endpoint,
                                           state->write_request_id,
                                           state->write_value) != 0) {
            serial_write("[test] chardev write req failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->phase = CHARDEV_TEST_PHASE_WAIT_WRITE;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == CHARDEV_TEST_PHASE_WAIT_WRITE) {
        rc = ipc_recv_for(process->context_id, state->reply_endpoint, &resp);
        if (rc == 1) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (rc != 0 || resp.type != WASM_CHARDEV_IPC_WRITE_RESP
            || resp.request_id != state->write_request_id
            || (int32_t)resp.arg0 != 0
            || ((resp.arg1 & 0xFFu) != state->write_value)) {
            serial_write("[test] chardev write resp invalid\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }

        state->read_request_id = 2;
        if (wasm_chardev_ipc_read_request(process->context_id,
                                          state->chardev_endpoint,
                                          state->reply_endpoint,
                                          state->read_request_id) != 0) {
            serial_write("[test] chardev read req failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }

        state->phase = CHARDEV_TEST_PHASE_WAIT_READ;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == CHARDEV_TEST_PHASE_WAIT_READ) {
        rc = ipc_recv_for(process->context_id, state->reply_endpoint, &resp);
        if (rc == 1) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (rc != 0 || resp.type != WASM_CHARDEV_IPC_READ_RESP
            || resp.request_id != state->read_request_id
            || (int32_t)resp.arg0 != 0
            || ((resp.arg1 & 0xFFu) != state->write_value)) {
            serial_write("[test] chardev read resp invalid\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }

        serial_write("[test] chardev ipc roundtrip ok\n");
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }

    process_set_exit_status(process, -1);
    return PROCESS_RUN_EXITED;
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

    serial_write("[kernel] wamr init on-demand\n");

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

    uint32_t chardev_endpoint = IPC_ENDPOINT_NONE;
    if (wasm_chardev_endpoint(&chardev_endpoint) != 0) {
        serial_write("[kernel] chardev endpoint lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    g_chardev_test_client.chardev_endpoint = chardev_endpoint;
    g_chardev_test_client.reply_endpoint = IPC_ENDPOINT_NONE;
    g_chardev_test_client.write_request_id = 0;
    g_chardev_test_client.read_request_id = 0;
    g_chardev_test_client.write_value = 0x41u;
    g_chardev_test_client.phase = CHARDEV_TEST_PHASE_INIT;

    uint32_t test_pid = 0;
    if (process_spawn("chardev-test-client",
                      chardev_test_client_entry,
                      &g_chardev_test_client,
                      &test_pid) != 0) {
        serial_write("[kernel] chardev test process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    serial_write("[kernel] chardev test pid=");
    serial_write_hex64(test_pid);

    serial_write("[kernel] scheduler loop\n");

    run_kernel_loop();
}
