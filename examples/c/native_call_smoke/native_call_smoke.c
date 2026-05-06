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
        if (endpoint < 0 ||
            wasmos_ipc_call(endpoint,
                            endpoint,
                            0x4242,
                            request_id,
                            0xDEAD,
                            0xBEEF,
                            0,
                            0,
                            &reply) != 0 ||
            reply.type != 0x4242 ||
            reply.arg0 != 0xDEAD ||
            reply.arg1 != 0xBEEF) {
            static const char fail[] = "native-call-smoke: ipc-call fail\n";
            (void)putsn(fail, sizeof(fail) - 1);
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
