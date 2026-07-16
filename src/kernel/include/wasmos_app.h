/* wasmos_app.h - WASMOS-APP (.wap) package format and runtime instance management.
 *
 * A .wap package is a binary blob with an 8-byte magic "WASMOSAP", a version field,
 * and a section table that carries the WASM or native ELF payload, linker.metadata
 * (TOML driver match data), and optional resource hints.
 *
 * wasmos_app_parse() extracts the sections into a wasmos_app_desc_t without copying;
 * all name/entry/wasm_bytes pointers alias back into the original blob and remain valid
 * only as long as the blob is live.
 *
 * wasmos_app_start() creates a wasm_driver_t (or native_driver) from the descriptor
 * and handles endpoint resolution and capability granting via injected callbacks. */
#ifndef WASMOS_APP_H
#define WASMOS_APP_H

#include <stdint.h>
#include "ipc.h"
#include "subsystem_registry.h"
#include "wasm_driver.h"

#define WASMOS_APP_MAGIC "WASMOSAP"
#define WASMOS_APP_VERSION 5u
#define WASMOS_APP_SUBSYSTEM_TAG_LEN 8u

#define WASMOS_SUBSYSTEM_TAG_WASM "WASM"
#define WASMOS_SUBSYSTEM_TAG_WASM3 "WASM3"
#define WASMOS_SUBSYSTEM_TAG_WARP "WARP"
#define WASMOS_SUBSYSTEM_TAG_NATIVE "NATIVE"

/* Package type flags stored in the .wap header. */
#define WASMOS_APP_FLAG_DRIVER (1u << 0)
#define WASMOS_APP_FLAG_SERVICE (1u << 1)
#define WASMOS_APP_FLAG_APP (1u << 2)
#define WASMOS_APP_FLAG_NEEDS_PRIV (1u << 3)
/* Native ELF payload; valid for privileged service/driver payloads. */
#define WASMOS_APP_FLAG_NATIVE (1u << 4)
#define WASMOS_APP_FLAG_STORAGE_BOOTSTRAP (1u << 5)
/* Process wants a controlling TTY allocated at spawn; PM fills spawn_info.tty.
 * Replaces the old "cli.tty.alloc" entry-arg binding. */
#define WASMOS_APP_FLAG_WANTS_TTY (1u << 6)

#define WASMOS_DRIVER_MATCH_ANY_U8 0xFFu
#define WASMOS_DRIVER_MATCH_ANY_U16 0xFFFFu

#define WASMOS_APP_MEM_HINT_LINEAR 0u
#define WASMOS_APP_MEM_HINT_STACK 1u
#define WASMOS_APP_MEM_HINT_HEAP 2u
#define WASMOS_APP_MEM_HINT_IPC 3u
#define WASMOS_APP_MEM_HINT_DEVICE 4u

#define WASMOS_APP_MAX_REQUIRED_ENDPOINTS 8u
#define WASMOS_APP_MAX_CAP_REQUESTS 8u
#define WASMOS_APP_MAX_ENTRY_ARG_BINDINGS 4u
#define WASMOS_APP_MAX_DRIVER_MATCHES 8u

typedef struct {
    const uint8_t* name;
    uint32_t name_len;
    uint32_t rights;
} wasmos_app_req_endpoint_t;

typedef struct {
    const uint8_t* name;
    uint32_t name_len;
    uint32_t flags;
} wasmos_app_cap_request_t;

typedef struct {
    const uint8_t* name;
    uint32_t name_len;
} wasmos_app_entry_arg_binding_t;

typedef struct {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t reserved0;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint32_t priority;
} wasmos_app_driver_match_t;

typedef struct {
    const uint8_t* blob;
    uint32_t blob_size;
    uint32_t flags;
    char subsystem_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    const uint8_t* wasm_bytes;
    uint32_t wasm_size;
    const uint8_t* compiled_bytes; /* pre-compiled WARP AOT binary; NULL if absent */
    uint32_t compiled_size;
    const uint8_t* name;
    uint32_t name_len;
    const uint8_t* entry;
    uint32_t entry_len;
    uint32_t stack_pages_hint;
    uint32_t heap_pages_hint;
    uint32_t driver_match_count;
    wasmos_app_driver_match_t driver_matches[WASMOS_APP_MAX_DRIVER_MATCHES];
    uint32_t req_ep_count;
    wasmos_app_req_endpoint_t req_eps[WASMOS_APP_MAX_REQUIRED_ENDPOINTS];
    uint32_t cap_count;
    wasmos_app_cap_request_t caps[WASMOS_APP_MAX_CAP_REQUESTS];
    uint32_t entry_arg_binding_count;
    wasmos_app_entry_arg_binding_t entry_arg_bindings[WASMOS_APP_MAX_ENTRY_ARG_BINDINGS];
} wasmos_app_desc_t;

typedef struct {
    const char* name;
    const uint8_t* module_bytes;
    uint32_t module_size;
    const uint8_t* compiled_bytes;
    uint32_t compiled_size;
    const char* entry_export;
    uint32_t stack_size;
    uint32_t heap_size;
    uint32_t entry_argc;
    const uint32_t* entry_argv;
} wasmos_app_start_params_t;

typedef struct {
    uint8_t started;
    int32_t entry_rc;
} wasmos_native_instance_t;

typedef union {
    wasm_driver_t wasm;
    wasmos_native_instance_t native;
} wasmos_app_runtime_state_t;

typedef struct {
    char requested_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    char broker_name[WASMOS_APP_SUBSYSTEM_TAG_LEN + 1];
    wasmos_subsystem_handler_kind_t kind;
    uint8_t uses_wasm_payload;
    uint8_t needs_runtime_lock;
    uint8_t gates_ready_for_services;
    uint32_t broker_endpoint;
    const wasmos_subsystem_ops_t* ops;
} wasmos_app_subsystem_info_t;

typedef struct wasmos_subsystem_ops wasmos_subsystem_ops_t;
struct wasmos_subsystem_ops {
    const char* tag;
    uint8_t uses_wasm_payload;
    uint8_t needs_runtime_lock;
    uint8_t gates_ready_for_services;
    int (*start)(wasmos_app_runtime_state_t* state, const wasmos_app_start_params_t* params,
                 uint32_t owner_context_id, uint32_t flags);
    int (*call_entry)(wasmos_app_runtime_state_t* state, const char* entry_export,
                      uint32_t entry_argc, uint32_t* entry_argv);
    void (*stop)(wasmos_app_runtime_state_t* state);
};

typedef int (*wasmos_app_endpoint_resolver_t)(uint32_t owner_context_id, const uint8_t* name,
                                              uint32_t name_len, uint32_t rights,
                                              uint32_t* out_endpoint);
typedef int (*wasmos_app_capability_granter_t)(uint32_t owner_context_id, const uint8_t* name,
                                               uint32_t name_len, uint32_t flags);

typedef struct {
    const wasmos_subsystem_ops_t* ops;
    wasmos_app_runtime_state_t runtime;
    uint8_t active;
    uint32_t flags;
    uint32_t owner_context_id;
    char name[64];
    char entry[64];
    uint32_t resolved_ep_count;
    uint32_t resolved_eps[WASMOS_APP_MAX_REQUIRED_ENDPOINTS];
    uint32_t entry_argc;
    uint32_t entry_argv[4];
} wasmos_app_instance_t;

int wasmos_app_parse(const uint8_t* blob, uint32_t blob_size, wasmos_app_desc_t* out_desc);
int wasmos_app_init_subsystems(void);
int wasmos_subsystem_register(const char* request_tag, const char* runtime_tag,
                              const wasmos_subsystem_ops_t* ops);
int wasmos_app_resolve_subsystem(const wasmos_app_desc_t* desc,
                                 wasmos_app_subsystem_info_t* out_info);
int wasmos_app_requires_explicit_ready(const wasmos_app_desc_t* desc);
int wasmos_app_start(wasmos_app_instance_t* instance, const wasmos_app_desc_t* desc,
                     uint32_t owner_context_id, const uint32_t* init_argv, uint32_t init_argc);
int wasmos_app_call_entry(wasmos_app_instance_t* instance);
void wasmos_app_stop(wasmos_app_instance_t* instance);
void wasmos_app_set_policy_hooks(wasmos_app_endpoint_resolver_t endpoint_resolver,
                                 wasmos_app_capability_granter_t capability_granter);

#endif
