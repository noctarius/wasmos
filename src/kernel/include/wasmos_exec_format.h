/* wasmos_exec_format.h - Deciding what an executable blob is, and validating the
 * spawn plan a registered broker answers with. */
#ifndef WASMOS_EXEC_FORMAT_H
#define WASMOS_EXEC_FORMAT_H

#include <stdint.h>
#include "wasmos_driver_abi.h"
#include "subsystem_registry.h"

/* Highest .wap container version the built-in probe recognises.  It MUST equal
 * WASMOS_APP_VERSION (wasmos_app.h); wasmos_app.c static-asserts that, because
 * the two live in different translation units and nothing else couples them.
 *
 * The failure mode when they drift is quiet: the probe only answers "is this a
 * WAP", so a version the parser accepts but the probe does not know is reported
 * as NOT a WAP.  That downgrades the classify() in pm_resolve_spawn_target from
 * WAP to NONE and fails broker-delegated spawns with
 * WASMOS_ERR_PROC_SPAWN_BROKER_PLAN, rather than rejecting the package. */
#define WASMOS_EXEC_APP_VERSION 6u

typedef enum {
    WASMOS_EXEC_FORMAT_NONE = 0,   /* nothing claimed it; not spawnable */
    WASMOS_EXEC_FORMAT_WAP = 1,    /* native .wap package, handled in-kernel */
    WASMOS_EXEC_FORMAT_BROKER = 2, /* a registered exec handler claimed it */
} wasmos_exec_format_kind_t;

typedef struct {
    wasmos_exec_format_kind_t kind;
    /* Meaningful only when kind == WASMOS_EXEC_FORMAT_BROKER. Held BY VALUE: the
     * registry frees an exec handler with the context that registered it, so a
     * borrowed pointer here could dangle before the caller acts on the match. */
    wasmos_exec_handler_registry_entry_t handler;
} wasmos_exec_format_match_t;

/* All pointers alias into the plan bytes passed to the validator and are live
 * only as long as that buffer is. */
typedef struct {
    const char* host_path;
    uint32_t host_path_len;
    const char* host_args;
    uint32_t host_args_len;
    uint32_t plan_flags;
} wasmos_exec_broker_plan_t;

/* Bytes a caller must read from the head of a file before classifying it: the
 * largest probe any registered handler declared, never less than the .wap
 * header prefix. */
uint32_t wasmos_exec_format_probe_bytes_needed(void);

/* Classify a blob by magic first, then by handler probe against `path` and the
 * leading bytes. Returns 0 with *out_match filled -- including the "nothing
 * claimed it" answer, WASMOS_EXEC_FORMAT_NONE -- and -1 only when out_match is
 * NULL. Callers must check kind, not just the return code. */
int wasmos_exec_format_classify(const char* path, const uint8_t* blob, uint32_t blob_size,
                                wasmos_exec_format_match_t* out_match);

/* Validate a broker's spawn-plan reply against the handler that produced it and
 * project it into *out_plan. Rejects a version or plan-kind mismatch, tags that
 * disagree with the handler's, string regions that escape the buffer or are not
 * NUL-terminated exactly at their declared length, and a host path not ending in
 * ".wap" -- all with -1, which also clears *out_plan. Returns 0 on success. */
int wasmos_exec_broker_plan_validate(const uint8_t* plan_bytes, uint32_t plan_size,
                                     const wasmos_exec_handler_registry_entry_t* handler,
                                     wasmos_exec_broker_plan_t* out_plan);

#endif
