#ifndef WASMOS_WASM_CHARDEV_H
#define WASMOS_WASM_CHARDEV_H

#include <stdint.h>
#include "ipc.h"
#include "wamr_runtime.h"

typedef struct {
    const char *name;
    int (*read_byte)(void *ctx, uint8_t *out_byte);
    int (*write_byte)(void *ctx, uint8_t byte);
    void *ctx;
} chardev_t;

int chardev_read_byte(chardev_t *dev, uint8_t *out_byte);
int chardev_write_string(chardev_t *dev, const char *str);

enum {
    WASM_CHARDEV_IPC_READ_REQ = 0x100,
    WASM_CHARDEV_IPC_WRITE_REQ = 0x101,
    WASM_CHARDEV_IPC_READ_RESP = 0x180,
    WASM_CHARDEV_IPC_WRITE_RESP = 0x181,
    WASM_CHARDEV_IPC_ERROR_RESP = 0x1FF
};

int wasm_chardev_init(uint32_t owner_context_id);
int wasm_chardev_attach_instance(wamr_instance_t *instance);
int wasm_chardev_endpoint(uint32_t *out_endpoint);
int wasm_chardev_service_once(void);
chardev_t *wasm_chardev_get(void);

int wasm_chardev_ipc_read_request(uint32_t chardev_endpoint,
                                  uint32_t client_reply_endpoint,
                                  uint32_t request_id);
int wasm_chardev_ipc_write_request(uint32_t chardev_endpoint,
                                   uint32_t client_reply_endpoint,
                                   uint32_t request_id,
                                   uint8_t byte);

#endif
