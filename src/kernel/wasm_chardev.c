#include "wasm_chardev.h"
#include "serial.h"
#include "wasm_driver.h"

extern const uint8_t _binary_chardev_server_wasm_start[];
extern const uint8_t _binary_chardev_server_wasm_end[];

static wasm_driver_t g_chardev_driver;
static uint32_t g_owner_context_id;
static uint32_t g_entry_args[1];

static uint32_t
wasm_chardev_module_size(void)
{
    return (uint32_t)(
        (uintptr_t)_binary_chardev_server_wasm_end -
        (uintptr_t)_binary_chardev_server_wasm_start);
}

int
wasm_chardev_init(uint32_t owner_context_id)
{
    wasm_driver_manifest_t manifest;

    manifest.name = "chardev-server";
    manifest.module_bytes = _binary_chardev_server_wasm_start;
    manifest.module_size = wasm_chardev_module_size();
    manifest.entry_export = "initialize";
    manifest.stack_size = 64 * 1024;
    manifest.heap_size = 64 * 1024;
    manifest.entry_argc = 1;
    manifest.entry_argv = g_entry_args;

    g_owner_context_id = owner_context_id;
    if (wasm_driver_start(&g_chardev_driver, &manifest, owner_context_id) != 0) {
        serial_write("[chardev] wasm driver start failed\n");
        return -1;
    }

    if (wasm_driver_endpoint(&g_chardev_driver, &g_entry_args[0]) != 0) {
        serial_write("[chardev] endpoint lookup failed\n");
        return -1;
    }

    serial_write("[chardev] wasm chardev ready (ipc)\n");
    return 0;
}

int
wasm_chardev_endpoint(uint32_t *out_endpoint)
{
    return wasm_driver_endpoint(&g_chardev_driver, out_endpoint);
}

int
wasm_chardev_run(void)
{
    return wasm_driver_call_entry(&g_chardev_driver);
}

int
wasm_chardev_ipc_read_request(uint32_t client_context_id,
                              uint32_t chardev_endpoint,
                              uint32_t client_reply_endpoint,
                              uint32_t request_id)
{
    ipc_message_t req;
    req.type = WASM_CHARDEV_IPC_READ_REQ;
    req.source = client_reply_endpoint;
    req.destination = chardev_endpoint;
    req.request_id = request_id;
    req.arg0 = 0;
    req.arg1 = 0;
    req.arg2 = 0;
    req.arg3 = 0;
    return ipc_send_from(client_context_id, chardev_endpoint, &req);
}

int
wasm_chardev_ipc_write_request(uint32_t client_context_id,
                               uint32_t chardev_endpoint,
                               uint32_t client_reply_endpoint,
                               uint32_t request_id,
                               uint8_t byte)
{
    ipc_message_t req;
    req.type = WASM_CHARDEV_IPC_WRITE_REQ;
    req.source = client_reply_endpoint;
    req.destination = chardev_endpoint;
    req.request_id = request_id;
    req.arg0 = (uint32_t)byte;
    req.arg1 = 0;
    req.arg2 = 0;
    req.arg3 = 0;
    return ipc_send_from(client_context_id, chardev_endpoint, &req);
}
