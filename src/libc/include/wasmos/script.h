/* script.h - simple .rc script engine for service startup sequencing */
#ifndef WASMOS_SCRIPT_H
#define WASMOS_SCRIPT_H

#include <stdint.h>

#define WASMOS_SCRIPT_IF_DEPTH     64
#define WASMOS_SCRIPT_ENV_NAME_MAX 33
#define WASMOS_SCRIPT_ENV_VAL_MAX  129
#define WASMOS_SCRIPT_LINE_MAX     256

typedef struct {
    char name[WASMOS_SCRIPT_ENV_NAME_MAX];
    char value[WASMOS_SCRIPT_ENV_VAL_MAX];
} wasmos_script_env_pair_t;

typedef struct wasmos_script_env_node {
    wasmos_script_env_pair_t pair;
    struct wasmos_script_env_node *next;
} wasmos_script_env_node_t;

/* Interpreter state: local and exported variable chains, last exit code,
 * recursion depth, and an if/else nesting tracker. */
typedef struct {
    wasmos_script_env_node_t *locals;
    wasmos_script_env_node_t *exports;
    int32_t                   last_exit_code;
    int32_t                   exec_depth;   /* depth of nested exec calls */
    int32_t                   total_depth;  /* total call depth for guard */
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
    /* echo extended: print expanded text, newline=1 appends trailing newline */
    void (*on_echo_ex)(void *user, const char *text, int newline);
    /* export VAR=value — publish to kernel env store. Return 0 or -1 */
    int (*on_export)(void *user, const char *name, const char *value);
    void *user;
} wasmos_script_ops_t;

/* Callback to resolve ${VAR} substitutions during echo expansion;
 * returns 0 and writes to out[] on success, -1 if var is unknown. */
typedef int (*wasmos_script_echo_resolve_var_fn)(void *user,
                                                 const char *name,
                                                 int32_t name_len,
                                                 char *out,
                                                 int32_t out_len);

/* Expand ${VAR} references and -n/-e flags in expr; sets *out_newline.
 * Returns 0 on success, -1 if out buffer is too small. */
int wasmos_script_echo_expand(const char *expr,
                              wasmos_script_echo_resolve_var_fn resolve_var,
                              void *resolve_user,
                              char *out,
                              int32_t out_len,
                              int *out_newline);

void wasmos_script_state_init(wasmos_script_state_t *state);
void wasmos_script_state_dispose(wasmos_script_state_t *state);
/* Inherit exported variables from parent into a child state for nested runs. */
void wasmos_script_state_init_child(wasmos_script_state_t *child,
                                    const wasmos_script_state_t *parent);
/* Read and interpret the .rc file at path; returns 0 on success, -1 on error. */
int  wasmos_script_run(wasmos_script_state_t *state,
                       const wasmos_script_ops_t *ops,
                       const char *path);

#endif
