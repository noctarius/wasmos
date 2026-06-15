# WASMOS Script Engine

The script engine (`src/libc/src/script.c`) is a minimal line-oriented interpreter
used by sysinit.rc and by the CLI `script`/`source` commands to sequence service
startup.  It has no loops or functions; its purpose is conditional spawning,
variable substitution, and synchronisation with the service registry.

## Syntax rules

- One command per line.
- Lines beginning with `#` are comments and are ignored.
- Blank lines are ignored.
- Leading and trailing whitespace is stripped from every line.
- Maximum line length: 256 characters (`WASMOS_SCRIPT_LINE_MAX`).

## Variable expansion

`${NAME}` is replaced with the value of variable `NAME` before a command is
executed.  The lookup order is: locals first, then exports.  Unknown variables
expand to an empty string (no error).

The special variable `${?}` expands to the exit code of the last `exec` or
`script` command (decimal, may be negative).

Expansion happens on every argument field of every command.

## Commands

### `start <path>`

Spawn the app at `<path>` and **block until it calls `notify_ready`**.  If the
service fails to start the script aborts with a fatal error.

```
start /boot/system/services/vt.wap
```

Use `start` for services that must be fully initialised before the next line
runs (e.g. vt, font service).

### `spawn <path>`

Spawn the app at `<path>` and **return immediately** without waiting.

```
spawn /boot/system/services/gfxcomp.wap
```

Use `spawn` for background services or apps that don't need to signal readiness
before subsequent commands proceed.

### `exec <path> [args]`

Spawn the app at `<path>`, pass optional whitespace-separated `args`, and
**block until the process exits**.  The exit code is stored in `${?}`.

```
exec /boot/tools/check-hw.wap --probe
if ${?} == 0 then
    echo "hardware ok"
endif
```

### `wait-svc <name>`

**Block until a service endpoint named `<name>` is registered** in the service
registry.  Polls with CPU yields between attempts (up to 256 retries).

```
spawn /boot/system/services/gfxcomp.wap
wait-svc gfx
spawn /boot/apps/menu_bar.wap
```

Use `wait-svc` after a `spawn` when the next command needs the service to be
reachable but does not need to own its startup sequence with `start`.

### `echo [-n] [-e|-E] [--] <text>`

Print `<text>` to the console after variable expansion.

| Flag | Effect |
|------|--------|
| `-n` | Omit the trailing newline |
| `-e` | Interpret backslash escapes (`\n \t \r \a \b \f \v`) |
| `-E` | Disable escape interpretation (default) |
| `--` | Stop flag parsing; everything after is literal text |

Quoting inside `<text>`:

- `'single quotes'` — literal; no expansion, no escapes.
- `"double quotes"` — allows `${VAR}` expansion; `\\` and `\"` are recognised.
- Backslash outside quotes escapes the next character.

```
echo "booting ${RELEASE}"
echo -n "progress: "
echo -e "line1\nline2"
```

### `export NAME=value`

Set variable `NAME` to `value` and **publish it to the kernel environment
store** so spawned processes inherit it.  The value is variable-expanded before
being stored.  Double-quoted values have their surrounding quotes stripped.

```
export DISPLAY_MODE=1280x720
export FLAGS="${FLAGS} --verbose"
```

### `set NAME=value`

Set a **local** variable visible only within the current script run.  Not
inherited by child scripts or spawned processes.

```
set TMP=/boot/tmp
```

### `script <path>`

Run the script at `<path>` in a **child context**.  The child inherits exported
variables but not locals.  Variables set or exported inside the child do not
propagate back.  `${?}` is set to the child's exit code on return.

```
script /boot/scripts/detect-hw.rc
```

### `source <path>` / `. <path>`

Run the script at `<path>` **in the current context**.  All variables (locals
and exports) are shared; changes made inside the sourced file are visible after
it returns.

```
source /boot/lib/common.rc
. /boot/lib/common.rc   # identical
```

## Conditionals

```
if <condition> then
    ...
else
    ...
endif
```

`else` is optional.  Conditionals nest up to 64 levels deep.  Each `if` must
have a matching `endif`.

### Conditions

**File test**

```
if -f /boot/system/services/fontsvc.wap then
    start /boot/system/services/fontsvc.wap
endif
```

`-f <path>` — true if the file exists.  
`-d <path>` — true if the path is a directory.

**Comparison**

Both sides are variable-expanded before comparison.  If both sides parse as
integers the comparison is numeric; otherwise only `==` and `!=` are available
and they compare as strings.

| Operator | Meaning |
|----------|---------|
| `==`     | equal |
| `!=`     | not equal |
| `<`      | less than (numeric only) |
| `>`      | greater than (numeric only) |
| `<=`     | less than or equal (numeric only) |
| `>=`     | greater than or equal (numeric only) |

```
if ${?} == 0 then
    echo "ok"
else
    echo "failed: ${?}"
endif

if ${COUNT} >= 3 then
    echo "retry limit reached"
endif
```

**Negation**

Prefix any condition with `!` to invert it.

```
if ! -f /boot/apps/optional.wap then
    echo "optional app not present, skipping"
endif
```

## Variable scoping

| Declared with | Visible in current script | Inherited by `script` child | Inherited by `source` | Inherited by spawned processes |
|---|---|---|---|---|
| `set`    | yes | no  | yes (same context) | no  |
| `export` | yes | yes | yes (same context) | yes |

## Limits

| Limit | Value |
|-------|-------|
| Maximum line length | 256 characters |
| Maximum if nesting depth | 64 levels |
| Variable name length | 32 characters |
| Variable value length | 128 characters |

## Example: sysinit.rc

```sh
# sysinit.rc -- boot-time service startup script

echo "Starting system services..."

spawn /boot/apps/chardevc.wap

start /boot/system/services/vt.wap

if -f /boot/system/services/fontsvc.wap then
    start /boot/system/services/fontsvc.wap
endif

spawn /boot/system/services/gfxcomp.wap
wait-svc gfx

if -f /boot/apps/menu_bar.wap then
    spawn /boot/apps/menu_bar.wap
endif

if -f /boot/apps/gfx_smoke.wap then
    spawn /boot/apps/gfx_smoke.wap
endif

start /boot/system/services/cli.wap
```

## Source locations

| File | Role |
|------|------|
| `src/libc/include/wasmos/script.h` | Public API and state types |
| `src/libc/src/script.c` | Interpreter implementation |
| `scripts/system/sysinit.rc` | Boot-time startup script (source of truth) |
