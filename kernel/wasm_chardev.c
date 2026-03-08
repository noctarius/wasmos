#include "wasm_chardev.h"
#include "serial.h"
#include "wasm_driver.h"

extern const uint8_t _binary_chardev_server_wasm_start[];
extern const uint8_t _binary_chardev_server_wasm_end[];

static wasm_driver_t g_chardev_driver;
static uint32_t g_owner_context_id;

static uint32_t
wasm_chardev_module_size(void)
{
    return (uint32_t)(
        (uintptr_t)_binary_chardev_server_wasm_end -
        (uintptr_t)_binary_chardev_server_wasm_start);
}

static void
wasm_chardev_reply(uint32_t reply_endpoint,
                   uint32_t type,
                   uint32_t request_id,
                   int status,
                   uint32_t arg1)
{
    uint32_t service_endpoint = IPC_ENDPOINT_NONE;
    ipc_message_t resp;

    if (wasm_driver_endpoint(&g_chardev_driver, &service_endpoint) != 0) {
        return;
    }

    resp.type = type;
    resp.source = service_endpoint;
    resp.destination = reply_endpoint;
    resp.request_id = request_id;
    resp.arg0 = (uint32_t)status;
    resp.arg1 = arg1;
    resp.arg2 = 0;
    resp.arg3 = 0;
    if (ipc_send_from(g_owner_context_id, reply_endpoint, &resp) != IPC_OK) {
        serial_write("[chardev] reply dropped\n");
    }
}

int
wasm_chardev_init(uint32_t owner_context_id)
{
    wasm_driver_manifest_t manifest;

    manifest.name = "chardev-server";
    manifest.module_bytes = _binary_chardev_server_wasm_start;
    manifest.module_size = wasm_chardev_module_size();
    manifest.init_export = "chardev_init";
    manifest.dispatch_export = "chardev_ipc_dispatch";
    manifest.stack_size = 64 * 1024;
    manifest.heap_size = 64 * 1024;
    manifest.init_argc = 0;
    manifest.init_argv = 0;

    g_owner_context_id = owner_context_id;
    if (wasm_driver_start(&g_chardev_driver, &manifest, owner_context_id) != 0) {
        serial_write("[chardev] wasm driver start failed\n");
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
wasm_chardev_service_once(void)
{
    ipc_message_t req;
    uint32_t service_endpoint = IPC_ENDPOINT_NONE;
    int recv_result;

    if (wasm_driver_endpoint(&g_chardev_driver, &service_endpoint) != 0) {
        return -1;
    }

    recv_result = ipc_recv_for(g_owner_context_id, service_endpoint, &req);
    if (recv_result != IPC_OK) {
        return recv_result;
    }

    switch (req.type) {
        case WASM_CHARDEV_IPC_READ_REQ: {
            int32_t value = -1;
            int status = wasm_driver_dispatch(&g_chardev_driver, &req, &value);
            if (status != 0 || value < 0 || value > 0xFF) {
                wasm_chardev_reply(req.source,
                                   WASM_CHARDEV_IPC_READ_RESP,
                                   req.request_id,
                                   -1,
                                   0);
            }
            else {
                wasm_chardev_reply(req.source,
                                   WASM_CHARDEV_IPC_READ_RESP,
                                   req.request_id,
                                   0,
                                   (uint32_t)value);
            }
            return 0;
        }
        case WASM_CHARDEV_IPC_WRITE_REQ: {
            int32_t value = -1;
            int status = wasm_driver_dispatch(&g_chardev_driver, &req, &value);
            if (status != 0) {
                value = -1;
            }
            wasm_chardev_reply(req.source,
                               WASM_CHARDEV_IPC_WRITE_RESP,
                               req.request_id,
                               (int)value,
                               req.arg0 & 0xFFu);
            return 0;
        }
        default:
            wasm_chardev_reply(req.source,
                               WASM_CHARDEV_IPC_ERROR_RESP,
                               req.request_id,
                               -1,
                               req.type);
            return -1;
    }
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
