#ifndef WASMOS_SUBSYSTEM_REGISTRY_H
#define WASMOS_SUBSYSTEM_REGISTRY_H

#include <stdint.h>

#define WASMOS_SUBSYSTEM_TAG_LEN 8u

struct wasmos_subsystem_ops;
typedef struct wasmos_subsystem_ops wasmos_subsystem_ops_t;

typedef enum {
    WASMOS_SUBSYSTEM_HANDLER_BUILTIN = 0,
    WASMOS_SUBSYSTEM_HANDLER_BROKER = 1,
} wasmos_subsystem_handler_kind_t;

typedef struct wasmos_subsystem_registry_entry {
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char broker_name[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    wasmos_subsystem_handler_kind_t kind;
    uint8_t uses_wasm_payload;
    uint8_t needs_runtime_lock;
    uint8_t gates_ready_for_services;
    uint32_t broker_endpoint;
    const wasmos_subsystem_ops_t *ops;
    struct wasmos_subsystem_registry_entry *next;
} wasmos_subsystem_registry_entry_t;

int wasmos_subsystem_registry_register_builtin(const char *request_tag,
                                               const char *runtime_tag,
                                               uint8_t uses_wasm_payload,
                                               uint8_t needs_runtime_lock,
                                               uint8_t gates_ready_for_services,
                                               const wasmos_subsystem_ops_t *ops);
int wasmos_subsystem_registry_register_broker(const char *request_tag,
                                              const char *runtime_tag,
                                              const char *broker_name,
                                              uint32_t broker_endpoint,
                                              uint8_t uses_wasm_payload,
                                              uint8_t needs_runtime_lock,
                                              uint8_t gates_ready_for_services);
const wasmos_subsystem_registry_entry_t *wasmos_subsystem_registry_find(const char *request_tag);
void wasmos_subsystem_registry_reset(void);

#endif
