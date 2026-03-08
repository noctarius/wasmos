#ifndef WASMOS_APP_H
#define WASMOS_APP_H

#include <stdint.h>
#include "ipc.h"
#include "wasm_driver.h"

#define WASMOS_APP_MAGIC "WASMOSAP"
#define WASMOS_APP_VERSION 1u

#define WASMOS_APP_FLAG_DRIVER     (1u << 0)
#define WASMOS_APP_FLAG_SERVICE    (1u << 1)
#define WASMOS_APP_FLAG_APP        (1u << 2)
#define WASMOS_APP_FLAG_NEEDS_PRIV (1u << 3)

#define WASMOS_APP_MEM_HINT_LINEAR 0u
#define WASMOS_APP_MEM_HINT_STACK  1u
#define WASMOS_APP_MEM_HINT_HEAP   2u
#define WASMOS_APP_MEM_HINT_IPC    3u
#define WASMOS_APP_MEM_HINT_DEVICE 4u

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;
    uint32_t flags;
    const uint8_t *wasm_bytes;
    uint32_t wasm_size;
    const uint8_t *name;
    uint32_t name_len;
    const uint8_t *entry;
    uint32_t entry_len;
    uint32_t stack_pages_hint;
    uint32_t heap_pages_hint;
} wasmos_app_desc_t;

typedef struct {
    wasm_driver_t driver;
    uint8_t active;
    uint32_t flags;
    uint32_t owner_context_id;
    char name[64];
    char entry[64];
} wasmos_app_instance_t;

int wasmos_app_parse(const uint8_t *blob, uint32_t blob_size, wasmos_app_desc_t *out_desc);
int wasmos_app_start(wasmos_app_instance_t *instance, const wasmos_app_desc_t *desc, uint32_t owner_context_id);
int wasmos_app_dispatch(wasmos_app_instance_t *instance, const ipc_message_t *request, int32_t *out_value);
void wasmos_app_stop(wasmos_app_instance_t *instance);

#endif
