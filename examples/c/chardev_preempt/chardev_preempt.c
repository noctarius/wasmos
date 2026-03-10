#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, import_name)
#define WASMOS_WASM_EXPORT
#endif

extern int32_t wasmos_console_write(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_write");
extern int32_t wasmos_ipc_create_endpoint(void)
    WASMOS_WASM_IMPORT("wasmos", "ipc_create_endpoint");
extern int32_t wasmos_ipc_send(int32_t destination_endpoint,
                               int32_t source_endpoint,
                               int32_t type,
                               int32_t request_id,
                               int32_t arg0,
                               int32_t arg1,
                               int32_t arg2,
                               int32_t arg3)
    WASMOS_WASM_IMPORT("wasmos", "ipc_send");
extern int32_t wasmos_ipc_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_recv");
extern int32_t wasmos_ipc_last_field(int32_t field)
    WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");

static void
write_line(const char *s, int32_t len)
{
    if (len > 0) {
        wasmos_console_write((int32_t)(uintptr_t)s, len);
    }
}

static int
wait_for_response(int32_t endpoint,
                  int32_t expected_type,
                  int32_t expected_req,
                  int32_t expected_value,
                  int32_t *out_value)
{
    const uint32_t spin_limit = 5000000u;
    for (uint32_t i = 0; i < spin_limit; ++i) {
        int32_t rc = wasmos_ipc_recv(endpoint);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            continue;
        }

        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t resp_value = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        if (resp_type != expected_type || resp_req != expected_req || resp_status != 0) {
            return -1;
        }
        if (out_value) {
            *out_value = resp_value;
        }
        if (expected_value >= 0 && (resp_value & 0xFF) != (expected_value & 0xFF)) {
            return -1;
        }
        return 0;
    }
    return -2;
}

WASMOS_WASM_EXPORT int32_t
chardev_preempt_step(int32_t ignored_type,
                     int32_t chardev_endpoint,
                     int32_t ignored_arg1,
                     int32_t ignored_arg2,
                     int32_t ignored_arg3)
{
    static int phase = 0;
    static int32_t reply_endpoint = -1;
    static const int32_t write_req = 101;
    static const int32_t read_req = 102;
    static const int32_t write_value = 0x5A;

    (void)ignored_type;
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (phase == 0) {
        reply_endpoint = wasmos_ipc_create_endpoint();
        if (reply_endpoint < 0) {
            return WASMOS_WASM_STEP_FAILED;
        }
        if (wasmos_ipc_send(chardev_endpoint,
                            reply_endpoint,
                            WASM_CHARDEV_IPC_WRITE_REQ,
                            write_req,
                            write_value,
                            0,
                            0,
                            0) != 0) {
            return WASMOS_WASM_STEP_FAILED;
        }
        phase = 1;
    }

    if (phase == 1) {
        int rc = wait_for_response(reply_endpoint,
                                   WASM_CHARDEV_IPC_WRITE_RESP,
                                   write_req,
                                   write_value,
                                   0);
        if (rc == -2) {
            static const char msg[] = "chardev-preempt: write timeout\n";
            write_line(msg, (int32_t)(sizeof(msg) - 1));
            return WASMOS_WASM_STEP_FAILED;
        }
        if (rc != 0) {
            return WASMOS_WASM_STEP_FAILED;
        }
        if (wasmos_ipc_send(chardev_endpoint,
                            reply_endpoint,
                            WASM_CHARDEV_IPC_READ_REQ,
                            read_req,
                            0,
                            0,
                            0,
                            0) != 0) {
            return WASMOS_WASM_STEP_FAILED;
        }
        phase = 2;
    }

    if (phase == 2) {
        int32_t value = -1;
        int rc = wait_for_response(reply_endpoint,
                                   WASM_CHARDEV_IPC_READ_RESP,
                                   read_req,
                                   write_value,
                                   &value);
        if (rc == -2) {
            static const char msg[] = "chardev-preempt: read timeout\n";
            write_line(msg, (int32_t)(sizeof(msg) - 1));
            return WASMOS_WASM_STEP_FAILED;
        }
        if (rc != 0) {
            return WASMOS_WASM_STEP_FAILED;
        }
        static const char ok[] = "chardev-preempt: ok\n";
        write_line(ok, (int32_t)(sizeof(ok) - 1));
        phase = 3;
    }

    if (phase == 3) {
        return WASMOS_WASM_STEP_DONE;
    }

    return WASMOS_WASM_STEP_FAILED;
}
