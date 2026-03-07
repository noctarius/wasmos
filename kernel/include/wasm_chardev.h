#ifndef WASMOS_WASM_CHARDEV_H
#define WASMOS_WASM_CHARDEV_H

#include <stdint.h>
#include "ipc.h"
#include "wasmos_driver_abi.h"

int wasm_chardev_init(uint32_t owner_context_id);
int wasm_chardev_endpoint(uint32_t *out_endpoint);
int wasm_chardev_service_once(void);

int wasm_chardev_ipc_read_request(uint32_t client_context_id,
                                  uint32_t chardev_endpoint,
                                  uint32_t client_reply_endpoint,
                                  uint32_t request_id);
int wasm_chardev_ipc_write_request(uint32_t client_context_id,
                                   uint32_t chardev_endpoint,
                                   uint32_t client_reply_endpoint,
                                   uint32_t request_id,
                                   uint8_t byte);

#endif
