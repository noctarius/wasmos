/* cli_types.h - shared types and constants for the CLI service */
#ifndef WASMOS_CLI_TYPES_H
#define WASMOS_CLI_TYPES_H

#include <stdint.h>

/* State-machine phases of the interactive CLI event loop. */
typedef enum {
    CLI_PHASE_INIT = 0,
    CLI_PHASE_PROMPT,
    CLI_PHASE_READ,
    CLI_PHASE_WAIT_IPC,   /* blocked waiting for an IPC response */
    CLI_PHASE_FAILED
} cli_phase_t;

#define CLI_MAX_PROCS 48
#define CLI_HISTORY_MAX 8
#define CLI_ENV_NAME_MAX 24
#define CLI_ENV_VALUE_MAX 96

/* Retry budgets used when sending to VT or waiting for a response. */
#define CLI_VT_SEND_RETRIES 16384
#define CLI_REQ_SEND_RETRIES 8192
#define CLI_VT_RESP_RETRIES 4096

/* Linked-list node for shell environment variables. */
typedef struct cli_env_var {
    char name[CLI_ENV_NAME_MAX];
    char value[CLI_ENV_VALUE_MAX];
    uint8_t is_export;   /* non-zero if variable is exported to child processes */
    struct cli_env_var *next;
} cli_env_var_t;

/* Tag for the asynchronous operation that is currently in flight. */
enum {
    PENDING_NONE = 0,
    PENDING_LIST,
    PENDING_CAT,
    PENDING_CD,
    PENDING_CD_CHAIN,  /* chained cd: waiting for first segment reply */
    PENDING_EXEC,
    PENDING_WAIT,
    PENDING_SPAWN
};

#endif
