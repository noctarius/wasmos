#ifndef WASMOS_EXEC_FORMAT_H
#define WASMOS_EXEC_FORMAT_H

#include <stdint.h>
#include "wasmos_driver_abi.h"
#include "subsystem_registry.h"

typedef enum {
    WASMOS_EXEC_FORMAT_NONE = 0,
    WASMOS_EXEC_FORMAT_WAP = 1,
    WASMOS_EXEC_FORMAT_BROKER = 2,
} wasmos_exec_format_kind_t;

typedef struct {
    wasmos_exec_format_kind_t kind;
    const wasmos_exec_handler_registry_entry_t* handler;
} wasmos_exec_format_match_t;

typedef struct {
    const char* host_path;
    uint32_t host_path_len;
    const char* host_args;
    uint32_t host_args_len;
    uint32_t plan_flags;
} wasmos_exec_broker_plan_t;

uint32_t wasmos_exec_format_probe_bytes_needed(void);
int wasmos_exec_format_classify(const char* path, const uint8_t* blob, uint32_t blob_size,
                                wasmos_exec_format_match_t* out_match);
int wasmos_exec_broker_plan_validate(const uint8_t* plan_bytes, uint32_t plan_size,
                                     const wasmos_exec_handler_registry_entry_t* handler,
                                     wasmos_exec_broker_plan_t* out_plan);

#endif
