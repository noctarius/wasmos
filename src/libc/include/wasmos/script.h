#ifndef WASMOS_SCRIPT_H
#define WASMOS_SCRIPT_H

#include <stdint.h>

#define WASMOS_SCRIPT_IF_DEPTH     64
#define WASMOS_SCRIPT_ENV_MAX      32
#define WASMOS_SCRIPT_ENV_NAME_MAX 33
#define WASMOS_SCRIPT_ENV_VAL_MAX  129
#define WASMOS_SCRIPT_LINE_MAX     256

typedef struct {
    uint8_t in_use;
    char    name[WASMOS_SCRIPT_ENV_NAME_MAX];
    char    value[WASMOS_SCRIPT_ENV_VAL_MAX];
} wasmos_script_local_var_t;

typedef struct {
    wasmos_script_local_var_t locals[WASMOS_SCRIPT_ENV_MAX];
    int32_t                   last_exit_code;
    int32_t                   exec_depth;
    int32_t                   total_depth;
    uint8_t                   seen_else[WASMOS_SCRIPT_IF_DEPTH];
} wasmos_script_state_t;

typedef struct {
    /* start <path> — spawn + wait for notify_ready. Return 0 on ok, -1 on fatal */
    int (*on_start)(void *user, const char *path);
    /* spawn <path> — fire-and-forget. Return 0 on ok, -1 ignored */
    int (*on_spawn)(void *user, const char *path);
    /* exec <path> [args] — spawn and wait for exit. Sets *out_exit_code. Return 0 or -1 */
    int (*on_exec)(void *user, const char *path, const char *args, int32_t *out_exit_code);
    /* wait-svc <name> — block until endpoint registered. Return 0 or -1 */
    int (*on_wait_svc)(void *user, const char *name);
    /* echo <text> — print expanded text */
    void (*on_echo)(void *user, const char *text);
    /* export VAR=value — publish to kernel env store. Return 0 or -1 */
    int (*on_export)(void *user, const char *name, const char *value);
    void *user;
} wasmos_script_ops_t;

void wasmos_script_state_init(wasmos_script_state_t *state);
int  wasmos_script_run(wasmos_script_state_t *state,
                       const wasmos_script_ops_t *ops,
                       const char *path);

#endif
