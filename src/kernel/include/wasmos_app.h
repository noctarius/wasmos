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

#define WASMOS_APP_MAX_REQUIRED_ENDPOINTS 8u
#define WASMOS_APP_MAX_CAP_REQUESTS 8u

typedef struct {
    const uint8_t *name;
    uint32_t name_len;
    uint32_t rights;
} wasmos_app_req_endpoint_t;

typedef struct {
    const uint8_t *name;
    uint32_t name_len;
    uint32_t flags;
} wasmos_app_cap_request_t;

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
    uint32_t req_ep_count;
    wasmos_app_req_endpoint_t req_eps[WASMOS_APP_MAX_REQUIRED_ENDPOINTS];
    uint32_t cap_count;
    wasmos_app_cap_request_t caps[WASMOS_APP_MAX_CAP_REQUESTS];
} wasmos_app_desc_t;

typedef int (*wasmos_app_endpoint_resolver_t)(uint32_t owner_context_id,
                                              const uint8_t *name,
                                              uint32_t name_len,
                                              uint32_t rights,
                                              uint32_t *out_endpoint);
typedef int (*wasmos_app_capability_granter_t)(uint32_t owner_context_id,
                                               const uint8_t *name,
                                               uint32_t name_len,
                                               uint32_t flags);

typedef struct {
    wasm_driver_t driver;
    uint8_t active;
    uint8_t has_init_entry;
    uint32_t flags;
    uint32_t owner_context_id;
    char name[64];
    char entry[64];
    uint32_t resolved_ep_count;
    uint32_t resolved_eps[WASMOS_APP_MAX_REQUIRED_ENDPOINTS];
    uint32_t init_argc;
    uint32_t init_argv[4];
} wasmos_app_instance_t;

int wasmos_app_parse(const uint8_t *blob, uint32_t blob_size, wasmos_app_desc_t *out_desc);
int wasmos_app_start(wasmos_app_instance_t *instance,
                     const wasmos_app_desc_t *desc,
                     uint32_t owner_context_id,
                     const uint32_t *init_argv,
                     uint32_t init_argc);
int wasmos_app_dispatch(wasmos_app_instance_t *instance, const ipc_message_t *request, int32_t *out_value);
int wasmos_app_has_init_entry(const wasmos_app_instance_t *instance);
int wasmos_app_call_init(wasmos_app_instance_t *instance);
void wasmos_app_stop(wasmos_app_instance_t *instance);
void wasmos_app_set_policy_hooks(wasmos_app_endpoint_resolver_t endpoint_resolver,
                                 wasmos_app_capability_granter_t capability_granter);

#endif
