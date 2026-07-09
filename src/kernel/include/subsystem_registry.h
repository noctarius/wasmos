#ifndef WASMOS_SUBSYSTEM_REGISTRY_H
#define WASMOS_SUBSYSTEM_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#define WASMOS_SUBSYSTEM_TAG_LEN 8u
#define WASMOS_EXEC_HANDLER_NAME_LEN 32u
#define WASMOS_EXEC_MATCH_TEXT_LEN 32u
#define WASMOS_EXEC_MATCH_MAX_BYTES 16u
#define WASMOS_EXEC_MATCH_MAX_NODES 16u

struct wasmos_subsystem_ops;
typedef struct wasmos_subsystem_ops wasmos_subsystem_ops_t;

typedef enum {
    WASMOS_SUBSYSTEM_HANDLER_BUILTIN = 0,
    WASMOS_SUBSYSTEM_HANDLER_BROKER = 1,
} wasmos_subsystem_handler_kind_t;

typedef enum {
    WASMOS_EXEC_MATCH_PREFIX = 0,
    WASMOS_EXEC_MATCH_EXTENSION = 1,
    WASMOS_EXEC_MATCH_FILENAME = 2,
    WASMOS_EXEC_MATCH_AND = 3,
    WASMOS_EXEC_MATCH_OR = 4,
    WASMOS_EXEC_MATCH_NOT = 5,
} wasmos_exec_match_kind_t;

typedef struct {
    wasmos_exec_match_kind_t kind;
    uint16_t left_index;
    uint16_t right_index;
    uint8_t value_len;
    union {
        uint8_t prefix[WASMOS_EXEC_MATCH_MAX_BYTES];
        char text[WASMOS_EXEC_MATCH_TEXT_LEN + 1];
    } value;
} wasmos_exec_match_node_t;

typedef struct {
    const char *path;
    const char *filename;
    const uint8_t *initial_bytes;
    uint32_t initial_size;
} wasmos_exec_probe_t;

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

typedef struct wasmos_exec_handler_registry_entry {
    char handler_name[WASMOS_EXEC_HANDLER_NAME_LEN + 1];
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char broker_name[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    uint32_t broker_endpoint;
    uint32_t priority;
    uint32_t max_probe_bytes;
    uint32_t node_count;
    uint32_t root_index;
    wasmos_exec_match_node_t *nodes;
    struct wasmos_exec_handler_registry_entry *next;
} wasmos_exec_handler_registry_entry_t;

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
int wasmos_subsystem_registry_register_exec_handler(const char *handler_name,
                                                    const char *request_tag,
                                                    uint32_t priority,
                                                    uint32_t max_probe_bytes,
                                                    const wasmos_exec_match_node_t *nodes,
                                                    uint32_t node_count,
                                                    uint32_t root_index);
const wasmos_subsystem_registry_entry_t *wasmos_subsystem_registry_find(const char *request_tag);
const wasmos_exec_handler_registry_entry_t *wasmos_subsystem_registry_find_exec_handler(const wasmos_exec_probe_t *probe);
uint32_t wasmos_subsystem_registry_exec_max_probe_bytes(void);
void wasmos_subsystem_registry_reset(void);

#endif
