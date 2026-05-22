## CLI and User-Space Baseline
The CLI intentionally stays small and testable.

Supported commands:
- `help`
- `ps`
- `ls`
- `cat <path>`
- `cd <path>`
- `<app>` (resolved through `PATH`; default includes `/boot/apps`, `/boot/system/services`, and `/boot/system/drivers`)
- `script <file>` (run commands line-by-line; abort on first non-zero process exit status)
- `export VAR=<value>` (set/update variable, delete when value is empty)
- `echo ${VAR}` (expand and print one variable)
- `halt`
- `reboot`

The CLI is also part of the scheduler regression story because it yields while
idle instead of monopolizing CPU time in a polling loop.
