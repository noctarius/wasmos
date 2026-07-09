#ifndef WASMOS_EXEC_FORMAT_H
#define WASMOS_EXEC_FORMAT_H

#include <stdint.h>
#include "subsystem_registry.h"

typedef enum {
    WASMOS_EXEC_FORMAT_NONE = 0,
    WASMOS_EXEC_FORMAT_WAP = 1,
    WASMOS_EXEC_FORMAT_BROKER = 2,
} wasmos_exec_format_kind_t;

typedef struct {
    wasmos_exec_format_kind_t kind;
    const wasmos_exec_handler_registry_entry_t *handler;
} wasmos_exec_format_match_t;

uint32_t wasmos_exec_format_probe_bytes_needed(void);
int wasmos_exec_format_classify(const char *path,
                                const uint8_t *blob,
                                uint32_t blob_size,
                                wasmos_exec_format_match_t *out_match);

#endif
