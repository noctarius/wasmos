#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_EXPORT
#endif

static uint8_t g_last_byte;
static uint8_t g_has_data;

WASMOS_WASM_EXPORT int32_t
chardev_init(void)
{
    g_last_byte = 0;
    g_has_data = 0;
    return 0;
}

WASMOS_WASM_EXPORT int32_t
chardev_read_byte(void)
{
    if (!g_has_data) {
        return -1;
    }
    return (int32_t)g_last_byte;
}

WASMOS_WASM_EXPORT int32_t
chardev_write_byte(int32_t value)
{
    g_last_byte = (uint8_t)(value & 0xFF);
    g_has_data = 1;
    return 0;
}

WASMOS_WASM_EXPORT int32_t
chardev_ipc_dispatch(int32_t type,
                     int32_t arg0,
                     int32_t arg1,
                     int32_t arg2,
                     int32_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    switch ((uint32_t)type) {
        case WASM_CHARDEV_IPC_READ_REQ:
            return chardev_read_byte();
        case WASM_CHARDEV_IPC_WRITE_REQ:
            return chardev_write_byte(arg0);
        default:
            return -1;
    }
}
