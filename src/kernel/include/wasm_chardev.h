/* wasm_chardev.h - Character device IPC bridge for WASM driver processes.
 * Accepts read/write IPC requests from WASM drivers and routes them through the
 * kernel serial/console path, decoupling WASM from direct hardware access. */
#ifndef WASMOS_WASM_CHARDEV_H
#define WASMOS_WASM_CHARDEV_H

#include <stdint.h>
#include "ipc.h"
#include "wasmos_driver_abi.h"

/* Initialize the chardev service for the given owner context; creates an IPC endpoint. */
int wasm_chardev_init(uint32_t owner_context_id);

/* Return the IPC endpoint number for the chardev service. */
int wasm_chardev_endpoint(uint32_t *out_endpoint);

/* Process one pending chardev IPC message; returns 0 if nothing was ready. */
int wasm_chardev_run(void);

/* Deliver a read request IPC message from client to the chardev endpoint. */
int wasm_chardev_ipc_read_request(uint32_t client_context_id,
                                  uint32_t chardev_endpoint,
                                  uint32_t client_reply_endpoint,
                                  uint32_t request_id);

/* Deliver a write-byte request IPC message from client to the chardev endpoint. */
int wasm_chardev_ipc_write_request(uint32_t client_context_id,
                                   uint32_t chardev_endpoint,
                                   uint32_t client_reply_endpoint,
                                   uint32_t request_id,
                                   uint8_t byte);

#endif
