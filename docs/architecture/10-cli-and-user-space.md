## CLI and User-Space Baseline
The CLI intentionally stays small and testable.

Supported commands:
- `help`
- `ps`
- `ls`
- `cat <path>`
- `cd <path>`
- `<app>` (resolved through `PATH`; default includes `/boot/apps`, `/boot/system/services`, and `/boot/system/drivers`)
  - raw command arguments are forwarded unchanged: everything after the first whitespace is passed to the spawned process via `PROC_IPC_SPAWN_PATH` payload (no CLI parsing/quoting layer)
- `script <file>` (run commands line-by-line; abort on first non-zero process exit status)
- `source <file>` / `. <file>` (run script in current scope; env updates persist)
- `export VAR=<value>` (set/update variable, delete when value is empty)
- `echo [-n] [-e|-E] [--] [text|${VAR}...]` (print text with `${VAR}` expansion; supports basic quoting/escaping)
- `halt`
- `reboot`

The CLI is also part of the scheduler regression story because it yields while
idle instead of monopolizing CPU time in a polling loop.

Environment scopes are modeled with dynamic linked lists instead of fixed slots:
- `set` writes locals in the current scope.
- `export` writes exported variables in the current scope.
- `script <file>` runs in a child snapshot scope (exported vars cloned from parent).
- `source`/`.` runs in the current scope (mutations persist in caller).
