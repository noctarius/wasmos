## CLI and User-Space Baseline
The CLI intentionally stays small and testable.

Supported commands:
- `help`
- `ps`
- `ls`
- `cat <path>`
- `cd <path>`
- `exec <app>`
- `script <file>` (run commands line-by-line; abort on first non-zero process exit status)
- `halt`
- `reboot`

The CLI is also part of the scheduler regression story because it yields while
idle instead of monopolizing CPU time in a polling loop.
