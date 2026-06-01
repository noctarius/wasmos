## CLI and User Space

This document describes the WASMOS interactive shell: its phase state machine,
command set, environment model, IPC sequences for process execution, VT
integration, keyboard input handling, and the script runner. The CLI is
implemented in `src/services/cli/cli.c` and `src/services/cli/cli_types.h`.

---

### Manifest and Entry Point

The CLI is packaged as a WASMOS-APP service. Its `linker.metadata` manifest
declares:

| Field                 | Value                                  |
|-----------------------|----------------------------------------|
| Name                  | `cli`                                  |
| Entry export          | `initialize`                           |
| Kind                  | `service`                              |
| Capabilities          | none (service-tier IPC only)           |
| Entry arg binding 0   | `proc.endpoint`                        |

At runtime the process manager resolves `proc.endpoint` to a live IPC endpoint
ID and passes it as `arg0` to:

```c
WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t home_tty_arg,
           int32_t ignored_arg2,
           int32_t ignored_arg3);
```

`home_tty_arg` is the VT TTY number the CLI should attach to (normally 1).

The entry function never returns; it drives an infinite dispatch loop over the
phase state machine.

---

### Constants

All sizing constants live in `cli_types.h`:

| Constant               | Value | Meaning                                      |
|------------------------|-------|----------------------------------------------|
| `CLI_MAX_PROCS`        | 48    | Maximum process slots in `ps` output         |
| `CLI_HISTORY_MAX`      | 8     | History ring size (entries)                  |
| `CLI_ENV_NAME_MAX`     | 24    | Maximum env var name length including NUL    |
| `CLI_ENV_VALUE_MAX`    | 96    | Maximum env var value length including NUL   |
| `CLI_VT_SEND_RETRIES`  | 16384 | Spin retries for VT write/switch sends       |
| `CLI_REQ_SEND_RETRIES` | 8192  | Spin retries for FS/proc request sends       |
| `CLI_VT_RESP_RETRIES`  | 4096  | Poll retries waiting for a VT response       |

The line buffer is a fixed 128-byte array (`g_line[128]`). The current working
directory is a fixed 64-byte string (`g_cwd[64]`), initialized to `"/"`.

---

### Phase State Machine

The main loop dispatches on a single `cli_phase_t` variable:

```
CLI_PHASE_INIT (0)
  │  resolve endpoints, register VT writer, set default env
  ▼
CLI_PHASE_PROMPT (1)
  │  if foreground: emit "<cwd> wamos> " prompt
  ▼
CLI_PHASE_READ (2)
  │  read one character per iteration; assemble line
  │  on \r/\n: invoke cli_handle_line()
  │    → returns 0 (synchronous command) → back to PROMPT
  │    → returns 1 (async IPC command) → advance to WAIT_IPC
  ▼
CLI_PHASE_WAIT_IPC (3)
  │  blocking wasmos_ipc_recv() on reply endpoint
  │  on FS_IPC_STREAM: stream bytes to console, stay in WAIT_IPC
  │  on final response: clear pending state, back to PROMPT
  ▼
CLI_PHASE_FAILED (4) — stall loop, no recovery
```

The `PENDING_*` enum tracks which async operation is in flight during
`WAIT_IPC`:

| Pending state      | Triggered by                   | Resolution                         |
|--------------------|--------------------------------|------------------------------------|
| `PENDING_NONE`     | —                              | —                                  |
| `PENDING_LIST`     | `ls`                           | FS stream → print entries          |
| `PENDING_CAT`      | `cat`                          | FS stream → print bytes            |
| `PENDING_CD`       | `cd` (short or trailing leg)   | FS_IPC_RESP → update `g_cwd`       |
| `PENDING_CD_CHAIN` | `cd` (absolute path ≥16 chars) | FS_IPC_RESP → re-send packed tail  |
| `PENDING_EXEC`     | exec fallthrough               | PROC_IPC_RESP → pid → PENDING_WAIT |
| `PENDING_WAIT`     | foreground process wait        | PROC_IPC_RESP → exit code → `$?`   |
| `PENDING_SPAWN`    | `spawn`                        | PROC_IPC_RESP → back to PROMPT     |

---

### Environment Model

The CLI maintains a singly-linked list of `cli_env_var_t` nodes rooted at
`g_env`:

```c
typedef struct cli_env_var {
    char             name[CLI_ENV_NAME_MAX];   /* 24 bytes */
    char             value[CLI_ENV_VALUE_MAX];  /* 96 bytes */
    int              is_export;
    struct cli_env_var *next;
} cli_env_var_t;
```

Variables are stored by value; no hash table, no fixed capacity. Lookup and
update walk the list linearly.

**Default environment** initialized at startup:

| Name   | Value                                                                      | Exported |
|--------|----------------------------------------------------------------------------|----------|
| `PATH` | `/boot/apps:/boot/system/services:/boot/system/drivers:/boot/system/utils` | yes      |
| `?`    | `0`                                                                        | no       |

#### Variable Commands

| Command                  | Effect                                                          |
|--------------------------|-----------------------------------------------------------------|
| `set VAR=<value>`        | Write or update a local (non-exported) variable                 |
| `export VAR=<value>`     | Write or update an exported variable                            |
| `export VAR=`            | Delete variable (empty value removes the entry)                 |
| `echo ${VAR}`            | Expand variable in output (two-pass: pre-expand then tokenize)  |

`is_export=1` marks variables that child script scopes inherit.
`is_export=0` variables are locals and never cross scope boundaries.

#### `$?` — Exit Status

`$?` is a local variable holding the most recent exit code:
- Set to `"0"` when a service or driver starts successfully (PM has sent
  `NOTIFY_READY` before the spawn reply arrives).
- Set to the decimal exit code string when a foreground application exits.
- Set to `"-1"` when spawn fails.
- Not modified by `spawn` (detached background invocations).

---

### Command Reference

Command matching is case-insensitive. `line_eq_ci` tests for an exact match;
`line_starts_with_ci` tests for a prefix. Commands are evaluated in declaration
order; the first match wins.

#### Informational

| Command     | Implementation               | Notes                                             |
|-------------|------------------------------|---------------------------------------------------|
| `help`      | inline `console_write`       | Lists all commands on one line                    |
| `mount`     | `FSMGR_IPC_QUERY_MOUNTS_REQ` | Receives mount table as a text blob via FS buffer |
| `kmaps`     | `wasmos_kmap_dump()`         | Dumps active kernel memory mappings               |
| `kmaps all` | `wasmos_kmap_dump_all()`     | Includes all process address spaces               |
| `ps`        | see below                    | Flat table                                        |
| `ps tree`   | see below                    | Tree view only                                    |
| `ps all`    | see below                    | Flat table + tree view                            |

#### `ps` Output Format

```
processes: <count>
sched: ticks <n> ready <n> running <pid>
 pid ppid state wasm thr/live vm(bytes) kstack(bytes) heap(bytes) rss_est(bytes) cpu(ticks) name
...
```

State values: `ready` (1), `run` (2), `blk` (3), `zmb` (4), `unk` (else).

The `ps tree` view prints a DFS-ordered hierarchy with 2-space indentation per
depth level; maximum depth is 16.

Per-process data is collected via `wasmos_proc_info_stats(index, name_ptr,
name_size, parent_ptr, stats_ptr)` which fills `wasmos_proc_stats_t`:

```c
typedef struct {
    uint32_t state;
    uint32_t is_wasm;
    uint32_t thread_count;
    uint32_t live_thread_count;
    uint64_t vm_total_bytes;
    uint64_t thread_kstack_total_bytes;
    uint64_t heap_committed_bytes;
    uint64_t rss_est_bytes;
    uint64_t cpu_ticks;
} wasmos_proc_stats_t;
```

#### Directory Navigation

`cd <path>` sends `FS_IPC_CHDIR_REQ` to the FS manager and waits for a
response. Two code paths:

- **Short path** (absolute path shorter than 16 chars, or any relative path):
  path is packed into four `uint32_t` words via `wasmos_sys_ipc_pack_name16`
  and sent as the four IPC arg fields → `PENDING_CD`.
- **Long absolute path** (absolute path ≥ 16 chars): `FS_IPC_CHDIR_REQ` is
  sent with arg fields all zero → `PENDING_CD_CHAIN`. On the response,
  the tail component is packed and a second `FS_IPC_CHDIR_REQ` is sent →
  `PENDING_CD`.

`set_cwd_path()` normalizes the path in memory: strips trailing slashes,
resolves `.` (skip) and `..` (pop last segment), handles absolute vs.
relative input relative to `g_cwd`. The buffer limit of 64 bytes is enforced
by truncation.

#### File Listing and Reading

`ls` sends `FS_IPC_LIST_REQ` and enters `PENDING_LIST`. The FS manager
streams directory entries back as `FS_IPC_STREAM` messages (4 bytes per
message, packed as `arg0..arg3`); the CLI prints them until the final
`FS_IPC_RESP` is received.

`cat <path>` uses libc `fopen`/`fread`, which routes through the `fs.vfs`
endpoint. Output is written to the console via `console_write`. No streaming
IPC is involved; the file is read synchronously.

#### TTY Switching

`tty <0-3>` calls `cli_switch_tty(tty, 1, &err)`, which sends
`VT_IPC_SWITCH_TTY` with the target TTY number and waits up to
`CLI_VT_RESP_RETRIES` polls for a `VT_IPC_RESP`. On success,
`g_last_seen_active_tty` is updated. Switching to TTY 0 prints
`"switched to tty0 (system console)"`.

#### Scripting

| Command            | Scope semantics                                          |
|--------------------|----------------------------------------------------------|
| `script <file>`    | Child snapshot: only exported vars are cloned into the   |
|                    | script's scope; mutations do not propagate back          |
| `source <file>`    | Current scope: locals + exports cloned; mutations        |
| `. <file>`         | written back to CLI env after the script exits           |

Both modes use the same `wasmos_script_run()` call via callback ops:

| Callback      | Maps to                                            |
|---------------|----------------------------------------------------|
| `on_start`    | `cli_spawn_exec_path` (no-wait)                    |
| `on_spawn`    | `cli_spawn_exec_path` (no-wait)                    |
| `on_exec`     | `cli_spawn_exec_path` + `cli_wait_for_pid_exit`    |
| `on_wait_svc` | `wasmos_sys_svc_lookup_retry` (up to 256 attempts) |
| `on_echo`     | `console_write` + newline                          |
| `on_echo_ex`  | `console_write` + optional newline                 |
| `on_export`   | no-op (exports handled by script state)            |

#### Environment Commands

`export VAR=value` — sets `is_export=1`. Empty value deletes the variable.

`set VAR=value` — sets `is_export=0`. Useful for locals that should not
propagate to scripts.

`echo [-n] [-e|-E] [--] [text|${VAR}...]` — two-phase expansion:
1. `${VAR}` references in the raw expression are expanded before flag parsing.
2. Tokens are parsed with single-quote and double-quote support, backslash
   escaping (only active inside double-quotes or unquoted text), and
   `-e`/`-E` escape mode toggling (`\n`, `\t`, `\r`, etc.). Unterminated
   quotes return an error.

#### System Commands

`halt` calls `wasmos_system_halt()`. `reboot` calls `wasmos_system_reboot()`.

---

### Process Execution

#### Path Resolution

The CLI resolves executable paths through `cli_resolve_exec_path()`:

1. Strip leading whitespace; take the first whitespace-delimited token as the
   program name.
2. Convert any backslash to forward-slash in the extracted name.
3. Append `.wap` if the name does not already end with `.wap`.
4. If the name is an absolute path (`/`-prefixed), use it directly.
5. If the name contains no `/`, search `PATH` directories in order:
   - Split `PATH` on `:`.
   - For each segment, try `fopen(<segment>/<name>)`. First success wins.
   - If `PATH` exhausted, try `<cwd>/<name>`.
6. If the name contains `/` but is not absolute, combine with `g_cwd`.

Everything after the first whitespace-delimited token is treated as raw
arguments and passed unchanged to the process.

#### Foreground Execution (exec fallthrough)

Any command that is not a built-in falls through to foreground execution:

```
1. cli_resolve_exec_path()
2. Write resolved path to FS buffer at offset 0 (path_len bytes)
3. Write args string to FS buffer at offset path_len+1 (args_len bytes)
4. Send PROC_IPC_SPAWN_PATH(path_len=arg1, args_len=arg2)
5. Wait for PROC_IPC_RESP → get spawned pid (arg0) + spawn_flags (arg1)
   → if service/driver flag set: PM already waited for NOTIFY_READY
     → set $?="0", back to PROMPT
   → else: send PROC_IPC_WAIT(pid) → PENDING_WAIT
6. Wait for PROC_IPC_RESP → exit_code in arg1 → set $?=exit_code
7. Back to CLI_PHASE_PROMPT
```

The CLI blocks in `WAIT_IPC` during step 5 and again during step 6.

#### Background Execution (`spawn`)

`spawn <cmd>` uses the same path resolution and FS buffer write as foreground,
but sets `PROC_SPAWN_PATH_FLAG_DETACH` in the IPC flags:

```
1. cli_resolve_exec_path()
2. Write path + args to FS buffer
3. Send PROC_IPC_SPAWN_PATH with FLAG_DETACH set → PENDING_SPAWN
4. Wait for PROC_IPC_RESP (pm confirms process started)
5. $? is not modified; back to CLI_PHASE_PROMPT
```

`spawn` does not block on process exit. The spawned process runs independently
until it terminates or crashes.

---

### Keyboard Input and VT Integration

#### Input Sources

The CLI reads input from two sources, tried in order each `READ` phase step:

1. **VT (virtual terminal)**: `VT_IPC_READ_REQ` sent to the VT service on the
   CLI's registered TTY. Response contains one byte or a "no char" status.
   Up to 32 poll iterations before giving up and yielding.
   - If the VT read fails (`rc < 0`), the CLI falls back permanently to serial
     and clears the VT endpoints.
   - A `vt_read_backoff` counter (7 cycles) throttles requests when no
     character was available.
2. **Serial fallback**: `wasmos_console_read()`. Always tried if VT yields no
   character.

If neither source has a character, `wasmos_sched_yield()` is called and the
READ step returns.

#### Keyboard Handling

| Input          | Action                                       |
|----------------|----------------------------------------------|
| `\r` or `\n`   | Submit line; store to history; dispatch      |
| `\b` or 0x7F   | Backspace: erase last character              |
| ESC `[` `A`    | Up arrow (VT only): navigate history older   |
| ESC `[` `B`    | Down arrow (VT only): navigate history newer |
| Any other char | Append to `g_line`, echo to console          |

Escape sequences are parsed by a 3-state machine (`g_esc_state` 0/1/2): raw
byte (0) → saw ESC (1) → saw `[` (2) → consume final letter and reset to 0.
Escape sequence handling only applies to VT input, not serial.

History navigation saves the current in-progress line to a scratch buffer on
the first up-arrow press and restores it on return-to-present (down to index 0).
Duplicate consecutive entries are suppressed.

#### Console Output

`console_write(s)` routes output to one of two paths:

- **VT path** (if VT endpoint is connected and CLI is foreground):
  `VT_IPC_WRITE_REQ` messages, 4 bytes per message packed into `arg0..arg3`,
  sent to the VT service with up to `CLI_VT_SEND_RETRIES=16384` spin retries.
- **Serial path**: `putsn(s, len)` always called in addition to VT (both paths
  run simultaneously when VT is active).

The prompt string is assembled atomically in an 80-byte buffer (`"<cwd> wamos>
"`) and emitted as a single `console_write` to prevent serial-log interleaving
from splitting it across rows.

#### Foreground Detection

`cli_is_foreground()` queries `VT_IPC_GET_ACTIVE_TTY` and compares the result
to `g_home_tty`. A backoff counter (`g_fg_query_backoff`) reduces VT round
trips: 31 cycles when foreground, 3 cycles when background. A special case
prevents CLI on TTY 1 from treating TTY 0 as background during compositor
graphics transitions (the compositor may temporarily own TTY 0).

The PROMPT phase only emits the prompt when foreground. The READ phase only
reads input when foreground. When background, the CLI yields each cycle without
consuming characters.

---

### VT Registration

During `CLI_PHASE_INIT`, the CLI sends `VT_IPC_REGISTER_WRITER` with its
`g_home_tty` number. This registers the CLI as the writer for that TTY slot in
the VT service. The response includes the current switch generation counter
(`g_vt_switch_generation`), which the CLI includes in subsequent
`VT_IPC_WRITE_REQ` messages so the VT service can discard stale writes from a
previous TTY occupant.

---

### IPC Endpoints Used

The CLI connects to three services during initialization:

| Endpoint name | Stored in          | Used for                              |
|---------------|--------------------|---------------------------------------|
| `proc.pm`     | `g_proc_endpoint`  | Spawn, wait, svc lookup               |
| `fs.vfs`      | `g_fs_endpoint`    | List, chdir, mount query              |
| `vt`          | `g_vt_endpoint`    | Write, read, switch TTY, register     |

The CLI also allocates its own reply endpoint (`g_reply_endpoint`) for
receiving responses, and a VT client endpoint (`g_vt_client_endpoint`) for
VT interactions.

---

### Structural Invariants

1. **One outstanding request at a time.** The CLI tracks at most one
   `g_pending_req` ID. This keeps the state machine simple and makes
   test sequences deterministic.

2. **Stale response = fatal.** If `resp_req != g_pending_req` on a final
   response (non-stream), the CLI calls `cli_fail_and_stall()`. FS stream
   messages with mismatched IDs are handled by re-entering the stream loop
   (the FS service sends them in order with the same `req_id`).

3. **Serial always receives output.** `console_write` calls `putsn`
   unconditionally so the QEMU serial monitor always shows CLI output
   regardless of VT state.

4. **Path resolution never modifies the line buffer.** `cli_extract_exec_path`
   copies into a separate 96-byte stack buffer; the original `g_line` is
   used unchanged for the args extraction step.

5. **Script scope is explicit.** `source`/`.` propagates mutations back;
   `script` does not. This is enforced by the `source_mode` parameter to
   `cli_run_script()` and the `cli_scope_from_script()` call that only
   executes in source mode.
