#include "wasm_fat.h"
#include "serial.h"
#include "wasm_driver.h"
#include "wasmos_driver_abi.h"
#include "physmem.h"

extern const uint8_t _binary_fs_fat_wasm_start[];
extern const uint8_t _binary_fs_fat_wasm_end[];

static wasm_driver_t g_fat_driver;
static uint32_t g_owner_context_id;
static ipc_message_t g_pending_req;
static uint8_t g_has_pending;
static uint64_t g_buffer_phys;
static uint32_t g_reply_endpoint;

static uint32_t
wasm_fat_module_size(void)
{
    return (uint32_t)(
        (uintptr_t)_binary_fs_fat_wasm_end -
        (uintptr_t)_binary_fs_fat_wasm_start);
}

static void
wasm_fat_reply(uint32_t reply_endpoint,
               uint32_t request_id,
               int status)
{
    uint32_t service_endpoint = IPC_ENDPOINT_NONE;
    ipc_message_t resp;

    if (wasm_driver_endpoint(&g_fat_driver, &service_endpoint) != 0) {
        return;
    }

    if (status == 0) {
        resp.type = FS_IPC_RESP;
        resp.arg0 = 0;
    } else {
        resp.type = FS_IPC_ERROR;
        resp.arg0 = (uint32_t)status;
    }

    resp.source = service_endpoint;
    resp.destination = reply_endpoint;
    resp.request_id = request_id;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    if (ipc_send_from(g_owner_context_id, reply_endpoint, &resp) != IPC_OK) {
        serial_write("[fat] reply dropped\n");
    }
}

int
wasm_fat_init(uint32_t owner_context_id, uint32_t block_endpoint)
{
    wasm_driver_manifest_t manifest;
    uint32_t init_args[3];

    manifest.name = "fs-fat";
    manifest.module_bytes = _binary_fs_fat_wasm_start;
    manifest.module_size = wasm_fat_module_size();
    manifest.init_export = "fat_init";
    manifest.dispatch_export = "fat_ipc_dispatch";
    manifest.stack_size = 64 * 1024;
    manifest.heap_size = 64 * 1024;
    manifest.init_argc = 3;

    g_reply_endpoint = IPC_ENDPOINT_NONE;
    if (ipc_endpoint_create(owner_context_id, &g_reply_endpoint) != IPC_OK) {
        serial_write("[fat] reply endpoint create failed\n");
        return -1;
    }

    if (!g_buffer_phys) {
        g_buffer_phys = pfa_alloc_pages_below(2, 0x100000000ULL);
    }
    if (!g_buffer_phys) {
        serial_write("[fat] buffer alloc failed\n");
        return -1;
    }
    init_args[0] = block_endpoint;
    init_args[1] = g_reply_endpoint;
    init_args[2] = (uint32_t)g_buffer_phys;
    manifest.init_argv = init_args;

    g_owner_context_id = owner_context_id;
    g_has_pending = 0;
    if (wasm_driver_start(&g_fat_driver, &manifest, owner_context_id) != 0) {
        serial_write("[fat] wasm driver start failed\n");
        return -1;
    }

    serial_write("[fat] wasm fs ready (ipc)\n");
    return 0;
}

int
wasm_fat_endpoint(uint32_t *out_endpoint)
{
    return wasm_driver_endpoint(&g_fat_driver, out_endpoint);
}

int
wasm_fat_service_once(void)
{
    ipc_message_t req;
    uint32_t service_endpoint = IPC_ENDPOINT_NONE;
    int32_t dispatch_result = 0;
    int recv_result;

    if (wasm_driver_endpoint(&g_fat_driver, &service_endpoint) != 0) {
        return -1;
    }

    if (!g_has_pending) {
        recv_result = ipc_recv_for(g_owner_context_id, service_endpoint, &req);
        if (recv_result == IPC_EMPTY) {
            return 1;
        }
        if (recv_result != IPC_OK) {
            return -1;
        }
        g_pending_req = req;
        g_has_pending = 1;
    }

    if (wasm_driver_dispatch(&g_fat_driver, &g_pending_req, &dispatch_result) != 0) {
        wasm_fat_reply(g_pending_req.source, g_pending_req.request_id, -1);
        g_has_pending = 0;
        return 0;
    }

    if (dispatch_result == WASMOS_WASM_STEP_BLOCKED) {
        return 1;
    }

    if (dispatch_result != 0) {
        wasm_fat_reply(g_pending_req.source, g_pending_req.request_id, dispatch_result);
        g_has_pending = 0;
        return 0;
    }

    wasm_fat_reply(g_pending_req.source, g_pending_req.request_id, 0);
    g_has_pending = 0;
    return 0;
}
