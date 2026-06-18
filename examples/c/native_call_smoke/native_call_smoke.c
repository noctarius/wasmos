#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char msg[] = "native-call-smoke: start\n";
    volatile int32_t write_rc = putsn(msg, sizeof(msg) - 1);
    (void)write_rc;

    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < 100000u; ++i) {
        sink ^= (i ^ (uint32_t)write_rc);
    }

    {
        const int32_t endpoint = wasmos_ipc_create_endpoint();
        const int32_t request_id = 0x1234;
        wasmos_ipc_message_t reply;
        int32_t call_rc = -1;
        if (endpoint >= 0) {
            call_rc = wasmos_ipc_send(endpoint,
                                      endpoint,
                                      0x4242,
                                      request_id,
                                      0xDEAD,
                                      0xBEEF,
                                      0,
                                      0);
        }
        if (call_rc == 0) {
            call_rc = wasmos_ipc_select_one(endpoint);
        }
        if (call_rc >= 0) {
            reply.type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
            reply.request_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            reply.arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            reply.arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
            reply.source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
            reply.destination = wasmos_ipc_last_field(WASMOS_IPC_FIELD_DESTINATION);
            reply.arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
            reply.arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        }
        if (endpoint < 0 ||
            call_rc < 0 ||
            reply.type != 0x4242 ||
            reply.request_id != request_id ||
            reply.arg0 != 0xDEAD ||
            reply.arg1 != 0xBEEF) {
            static const char fail[] = "native-call-smoke: ipc-call fail\n";
            (void)putsn(fail, sizeof(fail) - 1);
            printf("[native-call-smoke] ep=%d rc=%d type=%d req=%d a0=%d a1=%d src=%d dst=%d a2=%d a3=%d\n",
                   endpoint, call_rc, reply.type, reply.request_id,
                   reply.arg0, reply.arg1, reply.source, reply.destination,
                   reply.arg2, reply.arg3);
            return -1;
        }
        static const char ok[] = "native-call-smoke: ipc-call ok\n";
        write_rc = putsn(ok, sizeof(ok) - 1);
        sink ^= (uint32_t)write_rc;
    }

    static const char done[] = "native-call-smoke: done\n";
    write_rc = putsn(done, sizeof(done) - 1);
    sink ^= (uint32_t)write_rc;

    return (int32_t)sink;
}
