#ifndef WASMOS_CLI_TYPES_H
#define WASMOS_CLI_TYPES_H

#include <stdint.h>

typedef enum {
    CLI_PHASE_INIT = 0,
    CLI_PHASE_PROMPT,
    CLI_PHASE_READ,
    CLI_PHASE_WAIT_IPC,
    CLI_PHASE_FAILED
} cli_phase_t;

#define CLI_MAX_PROCS 48
#define CLI_HISTORY_MAX 8
#define CLI_ENV_MAX 16
#define CLI_ENV_NAME_MAX 24
#define CLI_ENV_VALUE_MAX 96

#define CLI_VT_SEND_RETRIES 16384
#define CLI_REQ_SEND_RETRIES 8192
#define CLI_VT_RESP_RETRIES 4096

typedef struct {
    uint8_t in_use;
    char name[CLI_ENV_NAME_MAX];
    char value[CLI_ENV_VALUE_MAX];
} cli_env_var_t;

enum {
    PENDING_NONE = 0,
    PENDING_LIST,
    PENDING_CAT,
    PENDING_CD,
    PENDING_CD_CHAIN,
    PENDING_EXEC,
    PENDING_WAIT,
    PENDING_SPAWN
};

#endif
