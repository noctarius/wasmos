## Environment Scopes and Inheritance

This document describes the implemented environment model: the CLI variable
store, the script engine state object, scope creation and propagation rules,
the kernel kenv store, and how each execution context handles variable lookup,
mutation, and inheritance.

---

### Three Distinct Env Layers

The system has three separate, loosely coupled env layers:

| Layer                        | Owner               | Storage                           | Scope                      |
|------------------------------|---------------------|-----------------------------------|----------------------------|
| CLI variable list (`g_env`)  | `cli.c`             | heap-allocated singly-linked list | CLI session only           |
| Script state tables          | `libc/src/script.c` | `wasmos_script_state_t`           | per script run             |
| Kernel kenv store (`g_kenv`) | `wasm3_link.c`      | flat static array                 | global, all WASM processes |

These layers are bridged explicitly at CLI–script transitions and when
sysinit runs `export` commands. They are not automatically synchronized.

---

### CLI Variable List

Defined in `src/services/cli/cli_types.h` and implemented in
`src/services/cli/cli.c`.

#### `cli_env_var_t`

```c
typedef struct cli_env_var {
    char name[CLI_ENV_NAME_MAX];   // 24
    char value[CLI_ENV_VALUE_MAX]; // 96
    uint8_t is_export;             // 0 = local, 1 = exported
    struct cli_env_var *next;
} cli_env_var_t;
```

Limits: `CLI_ENV_NAME_MAX = 24`, `CLI_ENV_VALUE_MAX = 96`.

The entire CLI env is stored as a single singly-linked list `g_env`. There
are no separate local/export containers; the `is_export` flag distinguishes
the two kinds. New entries are prepended (LIFO order within a kind).

#### Lookup

`cli_env_get(name)` searches the list in two passes:
1. First scan for `is_export == 0` (local) matching `name`.
2. Then scan for `is_export == 1` (exported) matching `name`.

This gives locals priority over exports when both have the same name.

#### Mutation

`cli_env_set(name, value, is_export)`:
- If `value` is NULL or empty: find and remove the matching entry.
- If the entry exists (same name + same is_export): update `value` in-place.
- If not found: prepend a new node.
- Rejects names ≥ 24 characters; rejects values ≥ 96 characters.

CLI commands:

| Command            | Effect                                   |
|--------------------|------------------------------------------|
| `set VAR=value`    | `cli_env_set(name, value, 0)` — local    |
| `export VAR=value` | `cli_env_set(name, value, 1)` — exported |
| `set VAR=`         | removes local entry                      |
| `export VAR=`      | removes exported entry                   |

A name can appear independently in both local and exported lists. `cli_env_get`
returns the local value first.

#### Default Environment

`cli_env_init_defaults()` runs at CLI startup:

```c
cli_env_set("PATH", "/boot/apps:/boot/system/services:"
                    "/boot/system/drivers:/boot/system/utils", 1);
cli_env_set("?", "0", 0);
```

`PATH` is exported; `?` (last exit code) is local.

#### PATH Resolution

`cli_resolve_path_from_pathvar(prog, resolved, resolved_len)`:
- Reads `PATH` via `cli_env_get("PATH")`.
- Iterates colon-separated segments, builds `<segment>/<prog>` (max 96-char candidate).
- Tests existence with `fopen(candidate, "r")`.
- Returns the first matching path or -1 if not found.

Used when a command name contains no `/`.

---

### Script Engine State

Defined in `src/libc/include/wasmos/script.h`.

#### `wasmos_script_state_t`

```c
typedef struct {
    wasmos_script_env_node_t *locals;          // local variable table
    wasmos_script_env_node_t *exports;         // exported variable table
    int32_t                   last_exit_code;  // set by exec; read as ${?}
    int32_t                   exec_depth;      // currently executing nesting depth
    int32_t                   total_depth;     // total if/endif nesting depth seen
    uint8_t                   seen_else[WASMOS_SCRIPT_IF_DEPTH]; // 64
} wasmos_script_state_t;
```

#### `wasmos_script_env_node_t`

```c
typedef struct wasmos_script_env_node {
    wasmos_script_env_pair_t pair;              // name[33], value[129]
    struct wasmos_script_env_node *next;
} wasmos_script_env_node_t;
```

Limits: `WASMOS_SCRIPT_ENV_NAME_MAX = 33`, `WASMOS_SCRIPT_ENV_VAL_MAX = 129`,
`WASMOS_SCRIPT_LINE_MAX = 256`, `WASMOS_SCRIPT_IF_DEPTH = 64`.

Unlike the CLI list, the script engine maintains `locals` and `exports` as
two separate linked lists.

#### Variable Lookup in Script Context

`script_scope_get(state, name)`:
1. Search `state->locals`.
2. If not found, search `state->exports`.

`${}` expansion calls `script_scope_get` for all variable names other
than `?`, which is formatted from `state->last_exit_code`.

#### Child Scope Creation

`wasmos_script_state_init_child(child, parent)`:
- Clears child to zero.
- Sets `child->last_exit_code = parent->last_exit_code`.
- Deep-clones `parent->exports` into `child->exports` (order preserved).
- `child->locals` starts empty.
- After creation, parent and child scopes are fully independent.

The clone fails gracefully: if `malloc` fails mid-clone, the partially
built export list is disposed and `child->exports` is left null.

#### State Lifecycle

- `wasmos_script_state_init(state)` — `memset` to zero.
- `wasmos_script_state_dispose(state)` — frees locals and exports lists;
  resets exec_depth and total_depth to 0.

---

### Script Commands and Scope Semantics

`wasmos_script_run(state, ops, path)` opens the file, reads lines one at a
time, strips leading/trailing whitespace, skips blank lines and `#` comments,
and dispatches each line via `script_exec_line`.

Lines are only executed when `exec_depth == total_depth`
(i.e. the current conditional branch is taken). `if`/`else`/`endif` adjust
these counters without caring about the executing flag.

#### Command Reference

| Command              | Scope effect                                                                                                                         |
|----------------------|--------------------------------------------------------------------------------------------------------------------------------------|
| `set VAR=value`      | Write to `state->locals`                                                                                                             |
| `export VAR=value`   | Write to `state->exports`; call `ops->on_export`                                                                                     |
| `set VAR=`           | Delete from `state->locals`                                                                                                          |
| `export VAR=`        | Delete from `state->exports`                                                                                                         |
| `echo <text>`        | Expand `${}` and flags; call `ops->on_echo_ex`                                                                                       |
| `start <path>`       | Call `ops->on_start` (spawn + wait for ready)                                                                                        |
| `spawn <path>`       | Call `ops->on_spawn` (fire-and-forget)                                                                                               |
| `exec <path> [args]` | Call `ops->on_exec`; store exit code in `state->last_exit_code`                                                                      |
| `wait-svc <name>`    | Call `ops->on_wait_svc`                                                                                                              |
| `script <file>`      | Create child via `wasmos_script_state_init_child`; run recursively; propagate child's `last_exit_code` back to parent; dispose child |
| `source <file>`      | Call `wasmos_script_run(state, ops, file)` — same state, no child                                                                    |
| `. <file>`           | Same as `source`                                                                                                                     |

All path and value arguments are subject to `${}` expansion before use.

#### Conditional Execution

```
if <cond> then
    ...
else
    ...
endif
```

`total_depth` increments on `if`; decrements on `endif`. `exec_depth` tracks
which branches are active.

Conditions supported by `script_eval_condition`:

| Form         | Test                                                   |
|--------------|--------------------------------------------------------|
| `-f <path>`  | `fopen` succeeds (file exists)                         |
| `-d <path>`  | `stat` succeeds and mode is `S_IFDIR`                  |
| `LHS == RHS` | numeric equality if both parse as int64; else `strcmp` |
| `LHS != RHS` | numeric or string inequality                           |
| `LHS < RHS`  | numeric only                                           |
| `LHS > RHS`  | numeric only                                           |
| `LHS <= RHS` | numeric only                                           |
| `LHS >= RHS` | numeric only                                           |
| `! <cond>`   | negate any of the above                                |

LHS and RHS are both subject to `${}` expansion.

Unclosed `if` blocks at end-of-file are tolerated (warn path exists in
source but is currently a no-op).

#### `echo` Expansion

`wasmos_script_echo_expand` processes flags before the text body:
- `-n` — suppress trailing newline.
- `-e` — enable escape sequences (`\n`, `\t`, `\r`, `\a`, `\b`, `\f`, `\v`).
- `-E` — disable escape sequences (default).
- `--` — end of flags.

Within the text body: `${VAR}` expands via the provided resolver; single-
quoted tokens suppress `${}` expansion and escapes; double-quoted tokens
suppress whitespace-splitting but allow `${}`.

---

### CLI–Script Bridge

The CLI keeps `g_env` as its primary env store and converts to/from
`wasmos_script_state_t` only at script/source boundaries.

#### `cli_scope_to_script(state, include_locals)`

Disposes and reinitializes `state`, then walks `g_env`:
- Copies all `is_export == 1` entries into `state->exports`.
- If `include_locals == 1`: also copies `is_export == 0` entries into
  `state->locals`.

Called with `include_locals = 0` for `script`, and `include_locals = 1`
for `source`.

#### `cli_scope_from_script(state)`

Clears `g_env`, then rebuilds it from the script state:
- All entries in `state->exports` → `cli_env_set(name, value, 1)`.
- All entries in `state->locals` → `cli_env_set(name, value, 0)`.

Called only in source mode after `wasmos_script_run` returns.

#### `cli_run_script(path, source_mode)`

```
cli_scope_to_script(&g_cli_script_state, source_mode)
wasmos_script_run(&g_cli_script_state, &ops, path)
if source_mode:
    cli_scope_from_script(&g_cli_script_state)
```

#### `cli_script_on_export` is a no-op

In the CLI's `wasmos_script_ops_t`, `on_export` does nothing:

```c
static int cli_script_on_export(void *user, const char *name, const char *value) {
    (void)user; (void)name; (void)value;
    return 0;
}
```

`export VAR=value` in a CLI-driven script writes only to the in-memory script
state's `exports` table. It does not modify `g_env` and does not call
`wasmos_env_set`. Changes reach `g_env` only in source mode when
`cli_scope_from_script` replays the script state back into the CLI list.

---

### Scope Inheritance Rules

| Scenario               | Parent scope content transferred to child            | Child changes visible in parent       |
|------------------------|------------------------------------------------------|---------------------------------------|
| CLI `script <file>`    | exported vars only → `state.exports`; locals omitted | No (source_mode=0, no callback)       |
| CLI `source <file>`    | all vars → state (exports + locals)                  | Yes (cli_scope_from_script after run) |
| Script `script <file>` | `state.exports` deep-cloned into child               | Only `last_exit_code` propagates back |
| Script `source <file>` | same state object passed to recursive run            | All mutations persist in place        |
| CLI `spawn <cmd>`      | not transmitted (no env-passing mechanism)           | No                                    |

The CLI does not pass `g_env` to spawned processes via any IPC mechanism.
Spawned processes receive no env from their parent and read the kernel kenv
store directly if they call `wasmos_env_get`.

---

### Kernel kenv Store

Implemented in `src/kernel/wasm3_link.c`. This is a flat, global,
process-agnostic key-value store accessible via WASM hostcalls.

#### `kenv_entry_t`

```c
typedef struct {
    uint8_t in_use;
    char    key[KENV_KEY_MAX];    // 33 (null-terminated)
    char    value[KENV_VAL_MAX];  // 129 (null-terminated)
} kenv_entry_t;

static kenv_entry_t g_kenv[KENV_MAX_ENTRIES];  // 64 entries
```

Constants: `KENV_MAX_ENTRIES = 64`, `KENV_KEY_MAX = 33`, `KENV_VAL_MAX = 129`.

#### Hostcalls

| Function                                                 | Wasm3 sig   | Behavior                                                |
|----------------------------------------------------------|-------------|---------------------------------------------------------|
| `wasmos_env_get(name, name_len, buf, buf_len) → int32`   | `"i(*i*i)"` | Returns bytes written or -1; null-terminates buf        |
| `wasmos_env_set(name, name_len, value, val_len) → int32` | `"i(*i*i)"` | val_len=0 clears value to empty string; returns 0 or -1 |
| `wasmos_env_unset(name, name_len) → int32`               | `"i(*i)"`   | Marks entry `in_use = 0`; always returns 0              |

Declared in `src/libc/include/wasmos/api.h`:

```c
extern int32_t wasmos_env_get(const char *name, int32_t name_len,
                              char *buf, int32_t buf_len)
    WASMOS_WASM_IMPORT("wasmos", "env_get");
extern int32_t wasmos_env_set(const char *name, int32_t name_len,
                              const char *value, int32_t val_len)
    WASMOS_WASM_IMPORT("wasmos", "env_set");
extern int32_t wasmos_env_unset(const char *name, int32_t name_len)
    WASMOS_WASM_IMPORT("wasmos", "env_unset");
```

All three hostcalls validate pointer bounds against the calling process's
user memory range via `mm_user_range_permitted` before accessing memory.

#### kenv Semantics

The kenv table is a flat global: all WASM processes read and write the same
table. There is no per-process namespace, no scoping, and no inheritance.
`wasmos_env_get` fails with -1 if the key is not found.

The table is first written during sysinit: the sysinit script engine calls
`wasmos_env_set` from its `on_export` handler whenever `export VAR=value`
appears in `sysinit.rc`. This is the primary mechanism for publishing
system-wide values (e.g. paths, service flags) to all processes.

---

### Sysinit Script Engine

Sysinit (`src/services/sysinit/init.c`) uses the same `wasmos_script_run`
machinery with a single flat `g_script_state` (no parent state). The script
path is `SYSINIT_SCRIPT_PATH = "/boot/system/sysinit.rc"`.

The live `sysinit.rc`:

```sh
# sysinit.rc -- boot-time service startup script
echo "Starting system services..."
spawn /boot/apps/chardevc.wap
start /boot/system/services/vt.wap
if -f /boot/system/services/fontsvc.wap then
    start /boot/system/services/fontsvc.wap
endif
spawn /boot/system/services/gfxcomp.wap
start /boot/system/services/cli.wap
```

The sysinit `on_export` handler:
```c
int sysinit_on_export(void *user, const char *name, const char *value) {
    return wasmos_env_set(name, strlen(name), value, strlen(value));
}
```

This means `export` lines in `sysinit.rc` write directly to the kernel kenv
store and are immediately visible to all WASM processes.

---

### Variable Limits Summary

| Context                             | Name max | Value max | Table size          |
|-------------------------------------|----------|-----------|---------------------|
| CLI (`cli_env_var_t`)               | 24       | 96        | Unbounded heap list |
| Script (`wasmos_script_env_node_t`) | 33       | 129       | Unbounded heap list |
| Kernel kenv                         | 33       | 129       | 64 entries fixed    |

CLI names and values are more constrained than script/kenv. A name or value
that fits in a script variable may be silently truncated or rejected when
converted to a CLI variable via `cli_scope_from_script`.

---

### Structural Invariants

1. **`export` in a CLI-run script does not touch `g_env` directly.** The
   CLI's `on_export` is a no-op. Script exports reach `g_env` only in source
   mode via `cli_scope_from_script`, or not at all in script mode.

2. **kenv is process-global.** Any WASM process can overwrite any kenv entry.
   There is no per-process namespace or ownership check on `env_set`.

3. **PATH is CLI-local.** The default PATH is set in `g_env` at CLI startup
   and is used only by `cli_resolve_path_from_pathvar`. It is not exported
   to spawned processes and is not stored in kenv.

4. **`?` tracks exit code at script level, not CLI level.** The script engine
   updates `state->last_exit_code` after each `exec` command. The CLI
   separately maintains `?` as a local `cli_env_var_t`. These are not
   automatically synchronized when crossing script boundaries.

5. **Scope independence after child creation.** After
   `wasmos_script_state_init_child`, the child's export table is a deep
   copy. Mutations to child exports do not affect the parent's exports and
   vice versa.

6. **Unclosed if blocks are silently tolerated.** `wasmos_script_run` detects
   `total_depth > 0` at EOF but takes no error action beyond a no-op comment
   in the source.
   TODO: emit a diagnostic on unclosed if blocks for script debugging.
