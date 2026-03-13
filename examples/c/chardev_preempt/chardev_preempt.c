#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

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
    wasmos_ipc_message_t resp;
    wasmos_ipc_message_read_last(&resp);
    if (resp.type != WASM_CHARDEV_IPC_WRITE_RESP
        || resp.request_id != write_req
        || resp.arg0 != 0
        || (resp.arg1 & 0xFF) != (write_value & 0xFF)) {
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
    wasmos_ipc_message_read_last(&resp);
    if (resp.type != WASM_CHARDEV_IPC_READ_RESP
        || resp.request_id != read_req
        || resp.arg0 != 0
        || (resp.arg1 & 0xFF) != (write_value & 0xFF)) {
        return -1;
    }

    static const char ok[] = "chardev-preempt: ok\n";
    putsn(ok, sizeof(ok) - 1);
    return 0;
}
