#ifndef WASMOS_SUBSYSTEM_REGISTRY_H
#define WASMOS_SUBSYSTEM_REGISTRY_H

#include <stdint.h>

#define WASMOS_SUBSYSTEM_TAG_LEN 8u

struct wasmos_subsystem_ops;
typedef struct wasmos_subsystem_ops wasmos_subsystem_ops_t;

typedef struct wasmos_subsystem_registry_entry {
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    const wasmos_subsystem_ops_t *ops;
    struct wasmos_subsystem_registry_entry *next;
} wasmos_subsystem_registry_entry_t;

int wasmos_subsystem_registry_register(const char *request_tag,
                                       const char *runtime_tag,
                                       const wasmos_subsystem_ops_t *ops);
const wasmos_subsystem_registry_entry_t *wasmos_subsystem_registry_find(const char *request_tag);
void wasmos_subsystem_registry_reset(void);

#endif
