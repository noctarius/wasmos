/* script.h - simple .rc script engine for service startup sequencing */
#ifndef WASMOS_SCRIPT_H
#define WASMOS_SCRIPT_H

#include <stdint.h>

/* Interpreter limits. IF_DEPTH bounds only the else-once bookkeeping array (see
 * seen_else below), not how deep if-blocks may nest. The two ENV maxima and
 * LINE_MAX are buffer sizes INCLUDING the terminator, so they admit 32-, 128-
 * and 255-character values; anything longer is truncated by the snprintf that
 * stores it, or fails expansion when the expanded result no longer fits. */
#define WASMOS_SCRIPT_IF_DEPTH 64
#define WASMOS_SCRIPT_ENV_NAME_MAX 33
#define WASMOS_SCRIPT_ENV_VAL_MAX 129
#define WASMOS_SCRIPT_LINE_MAX 256

/* One variable binding, stored by value (no pointers into the script text). */
typedef struct {
    char name[WASMOS_SCRIPT_ENV_NAME_MAX];
    char value[WASMOS_SCRIPT_ENV_VAL_MAX];
} wasmos_script_env_pair_t;

/* Singly-linked node of an environment chain; each node is a separate malloc
 * owned by the state that holds the chain and freed by
 * wasmos_script_state_dispose. Newly set variables are pushed at the head, so a
 * chain is in most-recently-created-first order. */
typedef struct wasmos_script_env_node {
    wasmos_script_env_pair_t pair;
    struct wasmos_script_env_node* next;
} wasmos_script_env_node_t;

/* Interpreter state: local and exported variable chains, last exit code, and the
 * if/else nesting tracker. A line runs only while exec_depth == total_depth. */
typedef struct {
    /* `set` bindings, visible to this script only. Looked up before exports. */
    wasmos_script_env_node_t* locals;
    /* `export` bindings, published through on_export and copied into a child
     * state by wasmos_script_state_init_child. */
    wasmos_script_env_node_t* exports;
    /* Exit code of the last `exec`, or -1 when a nested script failed; this is
     * what ${?} expands to. */
    int32_t last_exit_code;
    int32_t exec_depth;  /* open if-blocks whose branch is being executed */
    int32_t total_depth; /* open if-blocks, executed or skipped */
    /* Per-open-block flag: an `else` was already taken at that depth. Only the
     * first WASMOS_SCRIPT_IF_DEPTH levels are tracked; deeper nesting still
     * parses but its else-once rule is not enforced. */
    uint8_t seen_else[WASMOS_SCRIPT_IF_DEPTH];
} wasmos_script_state_t;

/* Host hooks the interpreter calls for every effectful command; `user` is passed
 * back to each unchanged. on_start, on_spawn, on_exec and on_wait_svc are
 * invoked WITHOUT a NULL check, so a host must supply every hook whose command
 * can appear in the scripts it runs. on_export is optional. on_echo_ex takes
 * precedence over on_echo when both are set, and only on_echo_ex is told whether
 * a trailing newline was requested. All the string arguments are borrowed stack
 * buffers valid only for the duration of the call. */
typedef struct {
    /* start <path> — spawn + wait for notify_ready. Return 0 on ok, -1 on fatal */
    int (*on_start)(void* user, const char* path);
    /* spawn <path> — fire-and-forget. Return 0 on ok, -1 ignored */
    int (*on_spawn)(void* user, const char* path);
    /* exec <path> [args] — spawn and wait for exit. Sets *out_exit_code. Return 0 or -1 */
    int (*on_exec)(void* user, const char* path, const char* args, int32_t* out_exit_code);
    /* wait-svc <name> — block until endpoint registered. Return 0 or -1 */
    int (*on_wait_svc)(void* user, const char* name);
    /* echo <text> — print expanded text */
    void (*on_echo)(void* user, const char* text);
    /* echo extended: print expanded text, newline=1 appends trailing newline */
    void (*on_echo_ex)(void* user, const char* text, int newline);
    /* export VAR=value — publish to kernel env store. Return 0 or -1 */
    int (*on_export)(void* user, const char* name, const char* value);
    void* user;
} wasmos_script_ops_t;

/* Callback to resolve ${VAR} substitutions during echo expansion. Returns 0 with
 * a NUL-terminated value in out[] (an unknown name resolves to the empty
 * string); any non-zero return makes the reference expand to nothing. */
typedef int (*wasmos_script_echo_resolve_var_fn)(void* user, const char* name, int32_t name_len,
                                                 char* out, int32_t out_len);

/* Expand ${VAR} references, quotes and backslash escapes in expr, consuming
 * leading -n/-e/-E flags (-n clears *out_newline, -e enables escape decoding).
 * Returns 0 on success, -1 on a bad argument, an unterminated quote, or an out[]
 * too small for the result. */
int wasmos_script_echo_expand(const char* expr, wasmos_script_echo_resolve_var_fn resolve_var,
                              void* resolve_user, char* out, int32_t out_len, int* out_newline);

/* Zero a state before its first run. Does not free anything, so a state that
 * already holds variables must be disposed first. `state` must not be NULL. */
void wasmos_script_state_init(wasmos_script_state_t* state);
/* Free both variable chains and reset the if-nesting counters, leaving the
 * state reusable. Idempotent, and a NULL `state` is a no-op. */
void wasmos_script_state_dispose(wasmos_script_state_t* state);
/* Inherit exported variables from parent into a child state for nested runs.
 * The child starts empty except for a deep copy of the parent's exports and its
 * last exit code; locals are not inherited and nothing the child changes flows
 * back. The copy is dropped when it cannot be completed, so the child may end up
 * with fewer exports rather than a partial chain. A NULL `parent` just zeroes
 * the child; the child must later be disposed. */
void wasmos_script_state_init_child(wasmos_script_state_t* child,
                                    const wasmos_script_state_t* parent);
/* Read and interpret the .rc file at path; returns 0 on success, -1 on error.
 *
 * Lines are trimmed, blank lines and #-comments skipped, and each remaining line
 * dispatched to one command; an unrecognised line is ignored rather than being
 * an error. -1 means the file could not be opened or a command reported a fatal
 * failure (a failing `start`, or a nested `script`/`source`), and interpretation
 * stops there. Most other command failures only set last_exit_code. `if` blocks
 * left unclosed at end of file are not reported. Runs synchronously and blocks
 * for as long as the host's hooks do. */
int wasmos_script_run(wasmos_script_state_t* state, const wasmos_script_ops_t* ops,
                      const char* path);

#endif
