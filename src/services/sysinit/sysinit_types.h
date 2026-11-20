/* sysinit_types.h - constants and state struct for the system initialiser */
#ifndef WASMOS_SYSINIT_TYPES_H
#define WASMOS_SYSINIT_TYPES_H

#include <stdint.h>

/* Max retries for a single PROC_IPC_SPAWN_PATH before giving up. */
#define SYSINIT_MAX_SPAWN_ATTEMPTS 128u
/* Timeout in ms sent as PROC_IPC_SPAWN_PATH_SYNC arg3 for 'start' commands: how
 * long the PM waits for the child's PROC_IPC_NOTIFY_READY before erroring. */
#define SYSINIT_START_TIMEOUT_MS 30000
/* How long a 'wait-svc' waits for the name to be registered before it is
 * reported as missing and the script moves on. */
#define SYSINIT_WAIT_SVC_TIMEOUT_MS 30000
/* The loop's clock while a command is in flight: the precision of wait-svc
 * deadlines and the spacing of spawn resends after PROC_PM_BUSY. */
#define SYSINIT_TICK_MS 50
/* Script file read at boot to drive the service startup sequence. */
#define SYSINIT_SCRIPT_PATH "/boot/system/sysinit.rc"

/* Minimal service state; only the IPC endpoints needed by script callbacks.
 * spawn_request_id seeds the event loop's request-id counter. */
typedef struct {
    int32_t reply_endpoint;
    int32_t spawn_request_id;
    int32_t proc_endpoint;
} sysinit_state_t;

#endif
