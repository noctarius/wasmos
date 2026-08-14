/* wasm_chardev.h - Character device IPC bridge for WASM driver processes.
 * Accepts read/write IPC requests from WASM drivers and routes them through the
 * kernel serial/console path, decoupling WASM from direct hardware access. */
#ifndef WASMOS_WASM_CHARDEV_H
#define WASMOS_WASM_CHARDEV_H

#include <stdint.h>
#include "ipc.h"
#include "wasmos_driver_abi.h"

/* Load and start the bundled chardev-server WASM module under owner_context_id
 * and create its IPC endpoint. Returns 0 on success, -1 on failure. */
int wasm_chardev_init(uint32_t owner_context_id);

/* Return the IPC endpoint number for the chardev service in *out_endpoint.
 * Returns 0 on success, -1 if the service is not started. */
int wasm_chardev_endpoint(uint32_t* out_endpoint);

/* Invoke the chardev module's entry export, which drains whatever requests are
 * queued on its endpoint. Returns 0 once the call completes, -1 if the module
 * is not running or the call trapped. The return value says nothing about how
 * many messages were handled. */
int wasm_chardev_run(void);

/* Deliver a read request IPC message from client to the chardev endpoint.  The reply is
 * addressed to client_reply_endpoint and carries request_id back, so the caller correlates
 * it itself.  Returns the ipc_send_from status (IPC_OK on success, IPC_ERR_FULL when the
 * chardev queue is full), not a 0/-1 flag.  Sends only — it does not wait for the reply. */
int wasm_chardev_ipc_read_request(uint32_t client_context_id, uint32_t chardev_endpoint,
                                  uint32_t client_reply_endpoint, uint32_t request_id);

/* Deliver a write-byte request IPC message from client to the chardev endpoint.  One byte
 * per message; same reply addressing and same ipc_send_from status convention as the read
 * request. */
int wasm_chardev_ipc_write_request(uint32_t client_context_id, uint32_t chardev_endpoint,
                                   uint32_t client_reply_endpoint, uint32_t request_id,
                                   uint8_t byte);

#endif
