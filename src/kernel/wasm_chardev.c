/* wasm_chardev.c - WASM character-device server bootstrap.
 * Instantiates the chardev_server.wasm blob as a wasm_driver_t owned by the
 * caller-supplied context, hands the server its own IPC endpoint as entry
 * argument 0, and offers helpers that frame a read/write request to it.  No
 * process is created; wasm_chardev_run() drives the entry export on the calling
 * thread.
 *
 * FIXME(chardev-dead): this file is not in the kernel source list
 * (src/kernel/CMakeLists.txt), nothing calls wasm_chardev_init(), and no build
 * rule emits the _binary_chardev_server_wasm_* symbols it links against.  Wire
 * it up or delete it; as it stands it cannot be built. */
#include "wasm_chardev.h"
#include "klog.h"
#include "wasm_driver.h"

extern const uint8_t _binary_chardev_server_wasm_start[];
extern const uint8_t _binary_chardev_server_wasm_end[];

static wasm_driver_t g_chardev_driver;
static uint32_t g_owner_context_id;
static uint32_t g_entry_args[4];

static uint32_t wasm_chardev_module_size(void) {
    return (uint32_t)((uintptr_t)_binary_chardev_server_wasm_end -
                      (uintptr_t)_binary_chardev_server_wasm_start);
}

/* Instantiates the linked-in chardev_server.wasm blob as the single global
 * driver instance owned by owner_context_id, and passes the driver's own
 * endpoint to the guest as entry argument 0.  It does NOT call the entry export;
 * wasm_chardev_run does that.
 *
 * Returns 0 on success and -1 when the driver cannot be started or its endpoint
 * cannot be resolved.  Not re-entrant and not idempotent: there is one static
 * instance, so a second call overwrites the first without stopping it. */
int wasm_chardev_init(uint32_t owner_context_id) {
    wasm_driver_manifest_t manifest;
    __builtin_memset(&manifest, 0, sizeof(manifest));

    manifest.name = "chardev-server";
    manifest.module_bytes = _binary_chardev_server_wasm_start;
    manifest.module_size = wasm_chardev_module_size();
    manifest.entry_export = "initialize";
    manifest.stack_size = 64 * 1024;
    manifest.heap_size = 64 * 1024;
    manifest.entry_argc = 4;
    manifest.entry_argv = g_entry_args;

    g_owner_context_id = owner_context_id;
    if (wasm_driver_start(&g_chardev_driver, &manifest, owner_context_id) != 0) {
        klog_write("[chardev] wasm driver start failed\n");
        return -1;
    }

    if (wasm_driver_endpoint(&g_chardev_driver, &g_entry_args[0]) != 0) {
        klog_write("[chardev] endpoint lookup failed\n");
        return -1;
    }

    klog_write("[chardev] wasm chardev ready (ipc)\n");
    return 0;
}

int wasm_chardev_endpoint(uint32_t* out_endpoint) {
    return wasm_driver_endpoint(&g_chardev_driver, out_endpoint);
}

/* Calls the server's entry export on the CALLING thread, so it blocks for as
 * long as the guest's loop runs.  There is no process behind it.  Returns
 * wasm_driver_call_entry's status. */
int wasm_chardev_run(void) {
    return wasm_driver_call_entry(&g_chardev_driver);
}

/* Frames and sends one read request to the chardev server.  Returns the raw
 * ipc_send_from status (IPC_OK on success, an IPC_ERR_* otherwise), not a packed
 * error code.  Asynchronous: the byte arrives later as a reply on
 * client_reply_endpoint, correlated by request_id. */
int wasm_chardev_ipc_read_request(uint32_t client_context_id, uint32_t chardev_endpoint,
                                  uint32_t client_reply_endpoint, uint32_t request_id) {
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

/* As wasm_chardev_ipc_read_request, but carries one byte to write in arg0.  One
 * byte per message, so a string costs one IPC per character. */
int wasm_chardev_ipc_write_request(uint32_t client_context_id, uint32_t chardev_endpoint,
                                   uint32_t client_reply_endpoint, uint32_t request_id,
                                   uint8_t byte) {
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
