## Environment Scopes and Inheritance

### Goals
- Provide deterministic environment-variable behavior across CLI, scripts, and spawned processes.
- Align runtime semantics with POSIX expectations for process environment inheritance.
- Separate local-only variables from exported inheritable variables.

### Scope Model
Each execution context owns an environment scope:
- CLI session scope.
- Script run scope.
- Process scope for spawned/executed programs.

Each scope contains two maps:
- `locals`: visible only in the current scope.
- `exports`: visible in the current scope and inherited by future children.

Child scopes are created with **snapshot clone semantics**:
- Child `exports` is a shallow value copy of parent `exports` at creation time.
- Child `locals` starts empty.
- After creation, parent and child scopes are independent.

### Lookup and Mutation
Lookup order in current scope:
1. `locals`
2. `exports`
3. unset

Mutation rules:
- `set VAR=value`: write `locals[VAR]`.
- `set VAR=`: remove `locals[VAR]`.
- `export VAR=value`: write `exports[VAR]`.
- `export VAR=`: remove `exports[VAR]`.

Notes:
- Exported values are not written into ancestors.
- Changing exports in a scope affects only that scope and future descendants created from it.
- Already-running siblings/parents/children are unaffected.

### CLI, Script, and Source Semantics
`script <file>`:
- Runs in a child scope cloned from caller exports.
- Script `set` and `export` changes are isolated to the script scope.
- Caller receives only exit status updates (for `${?}` behavior).

`source <file>` (or `. <file>`):
- Runs in the current scope.
- `set` and `export` mutations persist in caller scope.

Nested scripts:
- Script launched from script creates another child scope.
- Sourced file from script reuses current script scope.

### Spawn and Exec Semantics
`spawn`:
- New process gets child scope cloned from caller exports.
- Child locals start empty.

`exec`:
- If implemented as in-place image replacement, keep the same scope.
- If implemented as spawn+wait in user-space conventions, use spawn semantics.

### PATH and Runtime Visibility
`PATH` is modeled as an exported variable.

If a scope updates `export PATH=...`:
- New children of that scope inherit the new PATH value.
- Already-running children keep their startup snapshot.
- Parent/sibling scopes are unchanged.

### Ownership and Placement
Process-manager ownership is the target architecture:
- PM stores scope objects and parent-child lifecycle.
- Scope IDs are associated with process/script contexts.
- Env operations become scope-aware IPC/syscalls.

Transitional compatibility can keep existing `wasmos_env_get/set` interfaces,
with scope-aware routing under the hood.

### Required Behavior Tests
- CLI `set` shadows exported value without mutating exports.
- CLI `export` is inherited by newly spawned process.
- Parent export updates do not retroactively change already-running child process.
- `script` does not leak env changes back to caller.
- `source` persists env changes in caller.
- Nested `script` and `source` combinations obey scope rules.
