#ifndef WASMOS_WASM_CHARDEV_H
#define WASMOS_WASM_CHARDEV_H

#include <stdint.h>
#include "wamr_runtime.h"

typedef struct {
    const char *name;
    int (*read_byte)(void *ctx, uint8_t *out_byte);
    int (*write_byte)(void *ctx, uint8_t byte);
    void *ctx;
} chardev_t;

int chardev_read_byte(chardev_t *dev, uint8_t *out_byte);
int chardev_write_string(chardev_t *dev, const char *str);

int wasm_chardev_init(void);
int wasm_chardev_attach_instance(wamr_instance_t *instance);
chardev_t *wasm_chardev_get(void);

#endif
