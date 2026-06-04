/* sysinit_types.h - constants and state struct for the system initialiser */
#ifndef WASMOS_SYSINIT_TYPES_H
#define WASMOS_SYSINIT_TYPES_H

#include <stdint.h>

/* Max retries for a single PROC_IPC_SPAWN_PATH before giving up. */
#define SYSINIT_MAX_SPAWN_ATTEMPTS 128u
/* Timeout in ms passed to wasmos_sys_spawn_path_sync for 'start' commands. */
#define SYSINIT_START_TIMEOUT_MS 30000
/* Script file read at boot to drive the service startup sequence. */
#define SYSINIT_SCRIPT_PATH "/boot/system/sysinit.rc"

/* Minimal service state; only the IPC endpoints needed by script callbacks. */
typedef struct {
    int32_t reply_endpoint;
    int32_t spawn_request_id;  /* monotonically incremented per IPC call */
    int32_t proc_endpoint;
} sysinit_state_t;

#endif
