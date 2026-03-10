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

WASMOS_WASM_EXPORT int32_t
main(int32_t chardev_endpoint,
     int32_t ignored_arg1,
     int32_t ignored_arg2,
     int32_t ignored_arg3)
{
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    int32_t reply_endpoint = wasmos_ipc_create_endpoint();
    if (reply_endpoint < 0) {
        return -1;
    }

    const int32_t write_req = 101;
    const int32_t read_req = 102;
    const int32_t write_value = 0x5A;

    if (wasmos_ipc_send(chardev_endpoint,
                        reply_endpoint,
                        WASM_CHARDEV_IPC_WRITE_REQ,
                        write_req,
                        write_value,
                        0,
                        0,
                        0) != 0) {
        return -1;
    }

    if (wasmos_ipc_recv(reply_endpoint) < 0) {
        return -1;
    }
    int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    int32_t resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    int32_t resp_value = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    if (resp_type != WASM_CHARDEV_IPC_WRITE_RESP
        || resp_req != write_req
        || resp_status != 0
        || (resp_value & 0xFF) != (write_value & 0xFF)) {
        return -1;
    }

    if (wasmos_ipc_send(chardev_endpoint,
                        reply_endpoint,
                        WASM_CHARDEV_IPC_READ_REQ,
                        read_req,
                        0,
                        0,
                        0,
                        0) != 0) {
        return -1;
    }

    if (wasmos_ipc_recv(reply_endpoint) < 0) {
        return -1;
    }
    resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    resp_status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    resp_value = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    if (resp_type != WASM_CHARDEV_IPC_READ_RESP
        || resp_req != read_req
        || resp_status != 0
        || (resp_value & 0xFF) != (write_value & 0xFF)) {
        return -1;
    }

    static const char ok[] = "chardev-preempt: ok\n";
    write_line(ok, (int32_t)(sizeof(ok) - 1));
    return 0;
}
