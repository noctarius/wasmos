#include "wasm_chardev.h"
#include "serial.h"
#include "spinlock.h"

typedef struct {
    wamr_instance_t *instance;
    const char *init_fn;
    const char *read_fn;
    const char *write_fn;
} wasm_chardev_ctx_t;

static int wasm_chardev_read_cb(void *ctx, uint8_t *out_byte);
static int wasm_chardev_write_cb(void *ctx, uint8_t byte);
static void wasm_chardev_reply(uint32_t reply_endpoint,
                               uint32_t type,
                               uint32_t request_id,
                               int status,
                               uint32_t arg1);

static wasm_chardev_ctx_t g_wasm_ctx;
static chardev_t g_wasm_dev;
static spinlock_t g_wasm_lock;
static uint32_t g_service_endpoint;

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

int wasm_chardev_init(uint32_t owner_context_id) {
    g_wasm_ctx.instance = 0;
    g_wasm_ctx.init_fn = "chardev_init";
    g_wasm_ctx.read_fn = "chardev_read_byte";
    g_wasm_ctx.write_fn = "chardev_write_byte";
    spinlock_init(&g_wasm_lock);
    g_service_endpoint = 0;

    if (ipc_endpoint_create(owner_context_id, &g_service_endpoint) != 0) {
        serial_write("[chardev] endpoint allocation failed\n");
        return -1;
    }

    g_wasm_dev.name = "wasm.chardev0";
    g_wasm_dev.read_byte = wasm_chardev_read_cb;
    g_wasm_dev.write_byte = wasm_chardev_write_cb;
    g_wasm_dev.ctx = &g_wasm_ctx;

    serial_write("[chardev] wasm chardev ready (ipc)\n");
    return 0;
}

int wasm_chardev_attach_instance(wamr_instance_t *instance) {
    if (!instance) {
        return -1;
    }
    spinlock_lock(&g_wasm_lock);
    g_wasm_ctx.instance = instance;

    uint32_t argv[1] = {0};
    if (!wamr_call_function(instance, g_wasm_ctx.init_fn, 0, argv, 0)) {
        serial_write("[chardev] wasm attach without chardev_init\n");
    }
    spinlock_unlock(&g_wasm_lock);

    serial_write("[chardev] wasm chardev attached\n");
    return 0;
}

int wasm_chardev_endpoint(uint32_t *out_endpoint) {
    if (!out_endpoint) {
        return -1;
    }
    *out_endpoint = g_service_endpoint;
    return 0;
}

int wasm_chardev_service_once(void) {
    ipc_message_t req;
    int recv_result = ipc_recv(g_service_endpoint, &req);
    if (recv_result != 0) {
        return recv_result;
    }

    switch (req.type) {
        case WASM_CHARDEV_IPC_READ_REQ: {
            uint8_t value = 0;
            int status = chardev_read_byte(&g_wasm_dev, &value);
            wasm_chardev_reply(req.source,
                               WASM_CHARDEV_IPC_READ_RESP,
                               req.request_id,
                               status,
                               (uint32_t)value);
            return 0;
        }
        case WASM_CHARDEV_IPC_WRITE_REQ: {
            int status = g_wasm_dev.write_byte(g_wasm_dev.ctx, (uint8_t)(req.arg0 & 0xFFu));
            wasm_chardev_reply(req.source,
                               WASM_CHARDEV_IPC_WRITE_RESP,
                               req.request_id,
                               status,
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

chardev_t *wasm_chardev_get(void) {
    return &g_wasm_dev;
}

int wasm_chardev_ipc_read_request(uint32_t chardev_endpoint,
                                  uint32_t client_reply_endpoint,
                                  uint32_t request_id) {
    ipc_message_t req;
    req.type = WASM_CHARDEV_IPC_READ_REQ;
    req.source = client_reply_endpoint;
    req.destination = chardev_endpoint;
    req.request_id = request_id;
    req.arg0 = 0;
    req.arg1 = 0;
    req.arg2 = 0;
    req.arg3 = 0;
    return ipc_send(chardev_endpoint, &req);
}

int wasm_chardev_ipc_write_request(uint32_t chardev_endpoint,
                                   uint32_t client_reply_endpoint,
                                   uint32_t request_id,
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
    return ipc_send(chardev_endpoint, &req);
}

static int wasm_chardev_read_cb(void *ctx, uint8_t *out_byte) {
    wasm_chardev_ctx_t *wasm_ctx = (wasm_chardev_ctx_t *)ctx;
    if (!wasm_ctx || !out_byte) {
        return -1;
    }

    spinlock_lock(&g_wasm_lock);
    if (!wasm_ctx->instance) {
        spinlock_unlock(&g_wasm_lock);
        return -1;
    }

    uint32_t argv[1] = {0};
    if (!wamr_call_function(wasm_ctx->instance, wasm_ctx->read_fn, 0, argv, 0)) {
        spinlock_unlock(&g_wasm_lock);
        return -1;
    }
    spinlock_unlock(&g_wasm_lock);

    *out_byte = (uint8_t)(argv[0] & 0xFFu);
    return 0;
}

static int wasm_chardev_write_cb(void *ctx, uint8_t byte) {
    wasm_chardev_ctx_t *wasm_ctx = (wasm_chardev_ctx_t *)ctx;
    if (!wasm_ctx) {
        return -1;
    }

    spinlock_lock(&g_wasm_lock);
    if (!wasm_ctx->instance) {
        spinlock_unlock(&g_wasm_lock);
        return -1;
    }

    uint32_t argv[1];
    argv[0] = (uint32_t)byte;
    int ok = wamr_call_function(wasm_ctx->instance, wasm_ctx->write_fn, 1, argv, 0) ? 0 : -1;
    spinlock_unlock(&g_wasm_lock);
    return ok;
}

static void wasm_chardev_reply(uint32_t reply_endpoint,
                               uint32_t type,
                               uint32_t request_id,
                               int status,
                               uint32_t arg1) {
    ipc_message_t resp;
    resp.type = type;
    resp.source = g_service_endpoint;
    resp.destination = reply_endpoint;
    resp.request_id = request_id;
    resp.arg0 = (uint32_t)status;
    resp.arg1 = arg1;
    resp.arg2 = 0;
    resp.arg3 = 0;
    if (ipc_send(reply_endpoint, &resp) != 0) {
        serial_write("[chardev] reply dropped\n");
    }
}
