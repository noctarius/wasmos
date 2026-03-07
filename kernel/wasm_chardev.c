#include "wasm_chardev.h"
#include "serial.h"

typedef struct {
    wamr_instance_t *instance;
    const char *init_fn;
    const char *read_fn;
    const char *write_fn;
} wasm_chardev_ctx_t;

static int wasm_chardev_read_cb(void *ctx, uint8_t *out_byte);
static int wasm_chardev_write_cb(void *ctx, uint8_t byte);

static wasm_chardev_ctx_t g_wasm_ctx;
static chardev_t g_wasm_dev;

int chardev_read_byte(chardev_t *dev, uint8_t *out_byte) {
    if (!dev || !dev->read_byte || !out_byte) {
        return -1;
    }
    return dev->read_byte(dev->ctx, out_byte);
}

int chardev_write_string(chardev_t *dev, const char *str) {
    if (!dev || !dev->write_byte || !str) {
        return -1;
    }
    for (const char *p = str; *p; ++p) {
        if (dev->write_byte(dev->ctx, (uint8_t)*p) != 0) {
            return -1;
        }
    }
    return 0;
}

int wasm_chardev_init(void) {
    g_wasm_ctx.instance = 0;
    g_wasm_ctx.init_fn = "chardev_init";
    g_wasm_ctx.read_fn = "chardev_read_byte";
    g_wasm_ctx.write_fn = "chardev_write_byte";

    g_wasm_dev.name = "wasm.chardev0";
    g_wasm_dev.read_byte = wasm_chardev_read_cb;
    g_wasm_dev.write_byte = wasm_chardev_write_cb;
    g_wasm_dev.ctx = &g_wasm_ctx;

    serial_write("[chardev] wasm chardev ready\n");
    return 0;
}

int wasm_chardev_attach_instance(wamr_instance_t *instance) {
    if (!instance) {
        return -1;
    }
    g_wasm_ctx.instance = instance;

    uint32_t argv[1] = {0};
    if (!wamr_call_function(instance, g_wasm_ctx.init_fn, 0, argv, 0)) {
        serial_write("[chardev] wasm attach without chardev_init\n");
    }

    serial_write("[chardev] wasm chardev attached\n");
    return 0;
}

chardev_t *wasm_chardev_get(void) {
    return &g_wasm_dev;
}

static int wasm_chardev_read_cb(void *ctx, uint8_t *out_byte) {
    wasm_chardev_ctx_t *wasm_ctx = (wasm_chardev_ctx_t *)ctx;
    if (!wasm_ctx || !wasm_ctx->instance || !out_byte) {
        return -1;
    }

    uint32_t argv[1] = {0};
    if (!wamr_call_function(wasm_ctx->instance, wasm_ctx->read_fn, 0, argv, 0)) {
        return -1;
    }

    *out_byte = (uint8_t)(argv[0] & 0xFFu);
    return 0;
}

static int wasm_chardev_write_cb(void *ctx, uint8_t byte) {
    wasm_chardev_ctx_t *wasm_ctx = (wasm_chardev_ctx_t *)ctx;
    if (!wasm_ctx || !wasm_ctx->instance) {
        return -1;
    }

    uint32_t argv[1];
    argv[0] = (uint32_t)byte;
    return wamr_call_function(wasm_ctx->instance, wasm_ctx->write_fn, 1, argv, 0) ? 0 : -1;
}
