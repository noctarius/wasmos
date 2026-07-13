## Runtime Hosting and WASMOS-APP Format

This document describes how WASMOS loads, instantiates, and runs code: the
WASM runtime integration (wasm3 or WARP), the WASMOS-APP binary container
format, the `make_wasmos_app` packaging tool, and the language-shim ABI that
lets C, Rust, Go, Zig, and AssemblyScript programs share a single
kernel-facing entry point.

Two WASM runtime backends are available, selected at CMake configure time:

| Backend             | CMake flag                      | Character                                           |
|---------------------|---------------------------------|-----------------------------------------------------|
| **wasm3** (default) | *(none)*                        | Tree-walking interpreter; pure C; minimal footprint |
| **WARP**            | `-DWASMOS_WASM_RUNTIME_WARP=ON` | Single-pass x86_64 JIT compiler; near-native speed  |

Both backends implement the same `wasm_driver_t` API and pass `run-qemu-test`.

---

### wasm3 Runtime Integration

The default runtime is wasm3. The kernel hosts it directly in
`src/kernel/wasm_driver.c` and `src/kernel/wasm3_shim.c`. There is no shared
runtime state between processes; every process that runs WASM gets its own
`IM3Environment`, `IM3Runtime`, and `IM3Module`.

#### Heap Model

The wasm3 heap allocator is a chunked bump allocator defined in
`wasm3_shim.c`. Key constants:

| Constant                   | Value    | Meaning                                   |
|----------------------------|----------|-------------------------------------------|
| `WASM3_HEAP_DEFAULT_PAGES` | 1024     | Default preferred chunk size (4 MB)       |
| `WASM3_HEAP_MIN_PAGES`     | 32       | Minimum chunk size (128 KB)               |
| `WASM3_HEAP_MAX_CHUNKS`    | 64       | Maximum number of heap chunks per process |
| `WASM3_HEAP_ALIGN`         | 16 bytes | Allocation alignment                      |
| `WASM3_HEAP_MAX_BYTES`     | 2 GB     | Hard cap on runtime heap per process      |

Each process has a `wasm3_heap_slot_t` identified by PID:

```c
typedef struct {
    uint32_t pid;
    size_t preferred_chunk_size;    /* set by wasm3_heap_configure() */
    size_t max_size;                /* 2 GB ceiling */
    size_t committed_size;          /* sum of allocated chunks */
    uint32_t chunk_count;
    wasm3_heap_chunk_t chunks[WASM3_HEAP_MAX_CHUNKS];
} wasm3_heap_slot_t;
```

The allocator grows by appending chunks. Each new allocation advances the
tail chunk offset; once the chunk is exhausted a new physical-memory chunk is
appended. Freeing and in-place `realloc` are only optimized for tail
allocations — non-tail frees are silently accepted but the memory is not
reclaimed until the whole runtime is torn down at process exit.

#### Preemption Guard

All wasm3 entry points (`wasm_driver_start`, `wasm_driver_call`,
`wasm_driver_call_entry`, and the VM-thread entry) call `preempt_disable()`
before touching runtime state. wasm3 is not thread-safe and not re-entrant;
disabling preemption is the simplest correct guard on a single-core system.
`preempt_enable()` is always paired in the same call frame (or the
`wasm_driver_leave_runtime` wrapper handles it).

#### PID Binding

The heap is keyed by PID, not context ID. Before a wasm3 operation the caller
calls `wasm3_heap_bind_pid(pid)` to pin the heap lookup to the owner process,
even when the call originates from a different kernel context. The previous PID
is saved and restored via `wasm3_heap_restore_pid(previous_pid)`. This allows
the process manager (running as its own process) to start a new driver's
runtime under the driver's PID.

#### VM Thread Support

For drivers and services that need to spawn additional WASM execution contexts,
`wasm_driver_spawn_vm_thread()` allocates a `wasm_driver_thread_slot_t` from a
fixed table of 64 slots (`WASM_DRIVER_THREAD_SLOTS=64`) and spawns a kernel
worker thread. Each VM thread creates a fresh `IM3Environment`/`IM3Runtime`/
`IM3Module` stack, links the WASMOS host imports, and calls the requested
export with up to 4 `uint32_t` arguments. The slot is freed when the thread
exits.

#### Startup Sequence

`wasm_driver_start()` follows this order:

1. `wasm3_heap_configure()` — registers the PID/heap slot with size hints from
   the WASMOS-APP manifest (`stack_pages_hint * 4096`, `heap_pages_hint * 4096`;
   64 KB default when hints are zero).
2. `preempt_disable()` + `wasm3_heap_bind_pid()`.
3. `m3_NewEnvironment()` → `m3_NewRuntime(env, stack_size, NULL)` → `m3_ParseModule()` → `m3_LoadModule()`.
4. `wasm3_link_wasmos(module)` and `wasm3_link_env(module)` — bind all WASMOS
   host imports and WASI-compatible env imports.
5. `ipc_endpoint_create()` — allocate the driver's IPC endpoint.
6. Mark driver active, register in `g_wasm_driver_registry`.
7. `preempt_enable()`.

On teardown, `wasm_driver_stop()` calls `m3_FreeRuntime()`, `m3_FreeEnvironment()`,
unregisters from the registry, and resets the driver struct.

---

### WASMOS-APP Container Format

The WASMOS-APP file (`.wap`) is the deployment unit. Every driver, service, and
application is packed into this format by `make_wasmos_app`.

#### Format Versions

The parser in `wasmos_app.c` supports five header versions:

| Version     | Notes                                                                    |
|-------------|--------------------------------------------------------------------------|
| 1           | No entry-arg bindings, no driver matches table                           |
| 2           | Adds entry-arg bindings; single inline driver-match in header            |
| 3           | Adds `driver_match_count` and a separate per-match table                 |
| 4           | Replaces `reserved` with `compiled_size` for appended WARP AOT binaries  |
| 5 (current) | Adds an 8-byte subsystem tag for runtime dispatch and process reporting  |

Version is checked before any pointer arithmetic. Unknown versions are parse
errors.

#### v5 Header Layout

```c
typedef struct __attribute__((packed)) {
    char     magic[8];                  /* "WASMOSAP" */
    uint16_t version;                   /* 5 */
    uint16_t header_size;               /* sizeof(wasmos_app_header_v5_t) */
    uint32_t flags;                     /* WASMOS_APP_FLAG_* bitmask */
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;                 /* raw payload size in bytes */
    uint32_t req_ep_count;              /* 0 or 1 */
    uint32_t cap_count;                 /* up to 8 */
    uint32_t entry_arg_binding_count;   /* up to 4 */
    uint32_t mem_hint_count;            /* always 2: stack + heap */
    uint8_t  driver_match_class;        /* legacy; superseded by match table */
    uint8_t  driver_match_subclass;
    uint8_t  driver_match_prog_if;
    uint8_t  driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t driver_match_count;        /* entries in the variable match table */
    uint32_t compiled_size;             /* appended WARP AOT size; 0 if absent */
    char     subsystem_tag[8];          /* ASCII, NUL-padded runtime tag */
} wasmos_app_header_v5_t;
```

For compatibility, v1-v4 packages that do not carry a subsystem tag are mapped
to `NATIVE` when `WASMOS_APP_FLAG_NATIVE` is set and to the generic `WASM`
alias otherwise. The kernel resolves `WASM` to whichever built-in WASM backend
that kernel was compiled with.

#### Payload Layout (after header)

The parser walks the blob in this fixed order, same as the packer writes it:

```
[header]
[name bytes]
[entry export bytes]
[req_ep_count × (wasmos_req_endpoint_t + name bytes)]
[cap_count × (wasmos_cap_request_t + name bytes)]
[entry_arg_binding_count × (wasmos_entry_arg_binding_t + name bytes)]
[driver_match_count × wasmos_app_driver_match_t]
[mem_hint_count × wasmos_mem_hint_t]
[raw WASM or ELF bytes]
[compiled WARP AOT bytes if compiled_size > 0]
```

Variable-length sections are bounds-checked with 32-bit overflow-safe
arithmetic before any pointer access.

#### Flag Bits

| Flag                                | Value    | Meaning                                                        |
|-------------------------------------|----------|----------------------------------------------------------------|
| `WASMOS_APP_FLAG_DRIVER`            | `1 << 0` | Device driver; may hold hardware capabilities                  |
| `WASMOS_APP_FLAG_SERVICE`           | `1 << 1` | Kernel service; no hardware direct access                      |
| `WASMOS_APP_FLAG_APP`               | `1 << 2` | Unprivileged application                                       |
| `WASMOS_APP_FLAG_NEEDS_PRIV`        | `1 << 3` | Requests privileged spawn path                                 |
| `WASMOS_APP_FLAG_NATIVE`            | `1 << 4` | ELF payload; valid only with DRIVER or SERVICE                 |
| `WASMOS_APP_FLAG_STORAGE_BOOTSTRAP` | `1 << 5` | Must be initfs-resident; cannot be overridden by runtime rules |

`WASMOS_APP_FLAG_NATIVE` without `DRIVER` or `SERVICE` is a parse error.

#### Driver Match Record

```c
typedef struct {
    uint8_t  class_code;     /* 0xFF = any */
    uint8_t  subclass;       /* 0xFF = any */
    uint8_t  prog_if;        /* 0xFF = any */
    uint8_t  reserved0;
    uint16_t vendor_id;      /* 0xFFFF = any */
    uint16_t device_id;      /* 0xFFFF = any */
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint32_t priority;
} wasmos_app_driver_match_t;
```

Up to 8 match records per app (`WASMOS_APP_MAX_DRIVER_MATCHES`). The `device-manager`
evaluates all records; higher `priority` wins when multiple drivers match the
same device.

#### Memory Hints

Two fixed hint records are always written (kind=STACK, kind=HEAP):

```c
typedef struct __attribute__((packed)) {
    uint32_t kind;        /* WASMOS_APP_MEM_HINT_STACK=1, _HEAP=2 */
    uint32_t min_pages;   /* 4096 bytes per page */
    uint32_t max_pages;   /* reserved; parsed but not enforced */
} wasmos_mem_hint_t;
```

The kernel converts page counts to byte sizes: `pages * 4096`. A zero
`min_pages` for stack or heap causes the runtime to fall back to 64 KB.

#### Startup Contract (spawn-info)

Startup values are **no longer** passed through entry-arg registers. At spawn
time the process manager builds one `wasmos_spawn_info_t`
(`src/drivers/include/wasmos_spawn_info.h`) — a versioned header followed by the
argv blob — into a child-owned transfer buffer and records its `buffer_id` on
the process. The child retrieves it by execution model:

- **WASM** processes call the `wasmos_spawn_info_buffer()` hostcall to get the
  `buffer_id` (0 if none), then read the header + args with `xfer_buffer_read`.
- **Native** drivers/services call `api->spawn_info(&hdr, args_buf, args_cap)`
  (native ABI v7), which fills the header and copies the args directly.

The header carries `proc_endpoint`, `tty`, `module_count`, `module_index`, and
the argv offset/length; it is versioned (`magic`/`version`/`header_size`) so new
fields append without breaking older binaries. Service endpoints are **not**
carried here — a child resolves them with `svc_lookup` (e.g. `fs-fat` looks up
`"block"`). TTY allocation is requested with the `wants_tty` manifest key
(`WASMOS_APP_FLAG_WANTS_TTY`), which makes PM allocate a TTY and fill
`spawn_info.tty`.

The libc shims expose the header through `wasmos_startup_proc_endpoint()`,
`wasmos_startup_tty()`, `wasmos_startup_module_count()`,
`wasmos_startup_module_index()`, and `wasmos_startup_args()` (C; equivalent
`startup.*` accessors in Zig/AssemblyScript). `wasmos_startup_arg(0)` remains as
a compatibility alias for `proc.endpoint`.

> The legacy `entry_arg_bindings` manifest key is deprecated and ignored by the
> kernel; it is retained in the `.wap` format only for backward compatibility
> and will be removed.

#### Capability Names (fail-closed at pack time)

`make_wasmos_app` validates capability names against a static allowlist:

```
ipc.basic   io.port   irq.route   mmio.map   dma.buffer   system.control
```

Any other name is a pack error. Capability correctness is enforced at build
time, not discovered at runtime.

---

### make_wasmos_app Packaging Tool

The tool in `scripts/make_wasmos_app.c` has two invocation modes:

**Manifest mode (preferred):**
```
make_wasmos_app --manifest <path> --in <module.wasm|elf> --out <module.wap>
```

The manifest is a TOML-like file parsed from a `linker.metadata` file
colocated with each component's source directory. Sections:

```toml
version = 1

[package]
name     = "ata"
entry    = "initialize"
kind     = "driver"        # "driver" | "service" | "app"
subsystem = "WASM"         # optional; defaults to WASM or NATIVE
native   = false           # true for ELF native payloads
storage_bootstrap = true   # sets FLAG_STORAGE_BOOTSTRAP

[resources]
stack_pages = 16
heap_pages  = 16

[ipc]
required_endpoint_name    = "-"   # "-" means none
required_endpoint_rights  = 0
entry_arg_bindings        = ["proc.endpoint"]

[[capabilities]]           # zero or more; each is a separate table
name  = "io.port"
flags = 0

[[matches]]                # zero or more PCI match records
bus        = "pci"         # only "pci" is recognized
class      = 0x01
subclass   = "any"
prog_if    = "any"
vendor     = "any"
device     = "any"
io_port_min = 0x01F0
io_port_max = 0x03F7
priority   = 100
```

Subsystem tags are uppercase ASCII, at most 8 bytes, and are part of the
package ABI. Current in-tree tags:

- `WASM` — generic alias resolved to the kernel's built-in WASM backend
- `WASM3` — require the wasm3 backend
- `WARP` — require the WARP backend
- `WARP+JIT` — accepted as an alias for `WARP`
- `NATIVE` — native ELF payload

**Legacy positional mode:** still accepted for backward compatibility but not
used by the build system.

**Build targets:**
- `cmake --build build --target make_wasmos_app` builds the packer
- Each `CMakeLists.txt` component invokes `make_wasmos_app --manifest` as a
  post-build step to produce the `.wap` artifact

---

### Language ABI

The kernel's entry convention for WASM processes is a single export:

```
wasmos_main(arg0: i32, arg1: i32, arg2: i32, arg3: i32) -> i32
```

Each language shim exports `wasmos_main` and translates the four raw
`int32_t` arguments into the language's native call convention. The four
arguments are the resolved entry-arg binding values (endpoint IDs, module
counts, etc.); their meaning depends on the binding names declared in the
manifest.

| Language                                          | Export mechanism                                 | Native entry called  |
|---------------------------------------------------|--------------------------------------------------|----------------------|
| C (`libc/src/startup.c`)                          | `WASMOS_WASM_EXPORT int32_t wasmos_main(...)`    | `main(0, argv)`      |
| Rust (`libc/rust/wasmos.rs`)                      | `pub extern "C" fn wasmos_main(...)`             | `crate::main(&[])`   |
| Go (`libc/go/wasmos.go`)                          | `//export wasmos_main` + `func wasmos_main(...)` | `Main(emptyArgs)`    |
| Zig (`libc/zig/wasmos.zig`)                       | `pub export fn wasmos_main(...) callconv(.c)`    | `root.main()`        |
| AssemblyScript (`libc/assemblyscript/runtime.ts`) | `export function wasmos_main(...)`               | `runMain(main, ...)` |

All shims store the four arguments in a process-local array accessible through
`wasmos_startup_arg(index)` so the application can retrieve them after `main`
starts. The kernel ABI is stable; the language surface is what the developer
sees.

#### Driver and Service Entries

WASM drivers and services export `initialize` instead of `wasmos_main`:

```
initialize(arg0: i32, arg1: i32, arg2: i32, arg3: i32) -> i32
```

Native ELF drivers use the ELF `e_entry` address pointing at:

```c
int initialize(wasmos_driver_api_t *api, int arg1, int arg2, int arg3);
```

The `wasmos_driver_api_t` pointer is set to the kernel's native driver
function table; it is the only way native code reaches kernel internals.

---

### Runtime Load Path

When the process manager receives a spawn request for a WASMOS-APP blob, the
sequence is:

1. `wasmos_app_parse(blob, blob_size, &desc)` — validate and parse the container.
2. `wasmos_app_resolve_subsystem(&desc, &info)` maps the package tag onto a
   registered subsystem handler. The lookup is keyed directly by the 8-byte
   subsystem tag and returns a uniform result shape: handler kind
   (`BUILTIN`/future `BROKER`), resolved runtime tag, startup gating flags,
   and either an in-kernel ops table or the future external broker identity.
   Legacy packages without a tag are routed through compatibility aliases
   first. The current kernel populates that registry explicitly during early
   boot through `wasmos_app_init_subsystems()` +
   `wasmos_subsystem_register(...)`, using the shared uint32-keyed hashmap as a
   hash index and re-checking full tags inside each collision bucket.
3. Policy hooks set by `wasmos_app_set_policy_hooks()` resolve required endpoints
   and grant declared capabilities (callbacks into the process manager).
4. `wasmos_app_start(&instance, &desc, owner_context_id, init_argv, init_argc)`:
   - Uses the resolved subsystem handler returned by `wasmos_app_resolve_subsystem()`.
   - Calls `g_endpoint_resolver` for each entry in `desc.req_eps`.
   - Calls `g_capability_granter` for each capability in `desc.caps`.
   - Translates memory hints to byte sizes (`pages * 4096`, 64 KB floor).
   - Dispatches through the subsystem's `start` handler.
5. `wasmos_app_call_entry(&instance)` — dispatches through the subsystem's
   `call_entry` handler using the common instance-owned entry metadata.
6. `wasmos_app_stop(&instance)` dispatches through the subsystem's `stop`
   handler.

For WASM-backed built-in subsystems, the `start` handler calls
`wasm_driver_start()` and the entry handler calls `wasm_driver_call_unlocked()`.
For the native built-in subsystem, `start` calls `native_driver_start()` and
caches the result so the common entry path can still run through the same
`call_entry` contract. In the current kernel this registry is still built-in
and in-kernel; broker-backed registrations can now be represented in the
registry/result model, but `wasmos_app_start()` still rejects them until the
first IPC-routed subsystem broker lands.
Lookup no longer self-populates built-ins on first use; subsystem resolution is
read-only after boot-time registration succeeds.

Parse errors, failed endpoint resolution, and failed capability grants all
abort before any runtime state is created.

#### Executable Format Handlers

The subsystem registry now has a second registration surface alongside the
8-byte `.wap` subsystem-tag lookup: **broker-owned executable format
handlers**.

This is the intended path for additional executable formats that should remain
outside the kernel's built-in `.wap` container logic:

- `.jar` files owned by a JVM subsystem
- `.lua` files owned by a Lua subsystem
- shebang (`#!`) text files owned by a generic scripting subsystem

The current kernel keeps `.wap` as the only built-in executable package format.
Additional formats are represented as registry entries that attach a matcher
tree to a broker-backed subsystem registration.

Each handler currently records:

- handler name
- owning subsystem tag
- copied broker identity (`broker_name`, `broker_endpoint`)
- explicit priority
- `max_probe_bytes` budget for initial-byte probing
- a small matcher-node tree

The matcher tree is intentionally cheap and PM-friendly:

- `PREFIX(bytes...)`
- `EXTENSION(".lua")`
- `FILENAME("script")`
- `AND(left, right)`
- `OR(left, right)`
- `NOT(child)`

The registry validates matcher trees up front:

- bounded node count (`WASMOS_EXEC_MATCH_MAX_NODES`)
- acyclic structure
- valid child indexes
- non-empty leaf values
- prefix lengths that fit inside the handler's declared `max_probe_bytes`

Lookup evaluates all registered handlers against a lightweight probe
(`path`/`filename` + initial bytes) and returns the best match
deterministically:

1. higher `priority` wins
2. if priorities tie, lexicographically smaller handler name wins
3. if still tied, lexicographically smaller subsystem tag wins

PM-facing classification now exists as a separate helper:

- valid `.wap` headers are recognized first as the built-in executable format
- only non-`.wap` inputs fall through to broker-owned handler matching
- the helper exposes a single probe-byte budget that covers both `.wap`
  recognition and broker matchers

PM now has the first broker handoff contract as well:

- PM writes a `wasmos_broker_spawn_plan_request_t` into the tail of its loaded
  xfer buffer
- the guest blob stays at offset 0 in that same caller-owned xfer buffer
- PM currently lends that buffer to the broker read-only and sends
  `PROC_BROKER_IPC_SPAWN_PLAN_REQ`
- the broker currently replies with `PROC_BROKER_IPC_SPAWN_PLAN_RESP` pointing
  at a `wasmos_broker_spawn_plan_response_t` inside the broker's own xfer
  buffer
- PM borrows the broker buffer read-only, validates the returned plan against
  the matched handler identity, and only accepts a built-in `.wap` host-path
  plan kind for the current contract shape
- for that accepted plan kind, PM then reloads the returned host path through
  the ordinary path-spawn `.wap` flow and uses the broker-supplied host arg
  string as the final argv payload for the host workload

Architecturally, broker delegation should be understood in generic transfer-
buffer terms:

- request payloads are caller-owned transfer buffers lent to the broker with
  the required read access
- reply plans are transfer buffers lent to the broker with the required write
  access
- these are logically distinct borrows, even if an implementation chooses to
  optimize or stage them differently

The current PM/broker implementation still carries a narrower single-active-
borrow assumption in parts of the kernel path. That is an implementation
constraint under active correction, not the intended broker contract.

This is still a bounded delegation step. PM now classifies executable inputs,
validates a broker-owned spawn plan, and can execute the returned `.wap`
host-path plan through the existing built-in spawn machinery. More complex plan
kinds still remain future work.

The first real broker is `src/services/wasmos_script_broker`: it registers the
`WSCRIPT` subsystem plus a `PREFIX("#!")` executable handler and derives its
spawn plan from the guest file's `#!<interpreter>` line (data-driven, not a
hardcoded path). For the `wamos-script` interpreter it returns a `WAP_PATH` plan
that launches `src/services/wasmos_script` — a standalone executor that drives
the shared `wasmos_script` `.rc` engine (`src/libc/src/script.c`, the same one
the CLI embeds) over IPC — with the guest script path as argv. This replaces the
earlier hardcoded broker smoke fixture.

Broker/handler registration is now capability-gated and owner-scoped:

- `PROC_IPC_SUBSYSTEM_REGISTER_BROKER` and `PROC_IPC_EXEC_HANDLER_REGISTER`
  require the registering context to hold `CAP_SUBSYSTEM_REGISTER`
  (`subsystem.register` in the manifest); otherwise PM returns
  `PROC_PM_ERR_NOT_AUTHORIZED`.
- The registry records each entry's owner context and enforces per-owner and
  global caps on brokers and handlers.
- `wasmos_subsystem_registry_drop_owner()` runs from `process_reap`, so a broker
  that exits leaves no stale endpoint or matcher behind.

Runtime status: on wasm3 the delegated script runs end to end; on WARP ring-3 the
executor's first xfer-buffer read of the delegated argv is not yet coherent with
the module's user-VA view (see `docs/STATUS.md`).

#### Target Subsystem Delegation Model

The long-term direction is to make `NATIVE` the only kernel-built-in
user-space execution path. All other runtimes (`WASM3`, `WARP`, `JVM`, `LUA`,
`BEAM`, and similar environments) move out of the kernel and become native
ring-3 subsystem components.

That split is intentionally two-layered:

- **Subsystem broker**: parses a package format, validates it, applies
  subsystem-specific policy, and decides how the workload should be realised.
- **Execution engine**: the actual runtime host that executes the workload
  once PM has created the process.

For some formats the broker and engine may be the same native binary. For more
complex environments they are conceptually distinct even if the first
implementation combines them.

The preferred execution model is **one native host process per guest
workload**, not a multi-tenant runtime daemon. That keeps ring-3 isolation,
ownership, `wait`/`kill`, exit status, ready signaling, and accounting aligned
with the existing per-process model.

Even when a guest workload is realised through a native host binary, that host
is treated as the implementation detail of the child process rather than as a
second user-visible process. The logical process identity remains the guest
package:

- `ps` should show the guest app/service/driver name and resolved runtime tag
- parent/child relationships remain attached to the guest workload
- lifecycle operations (`wait`, `kill`, ready, exit status) target the guest
  identity, not the host binary filename

This means future PM delegation should be read as **"realise this child through
engine X"**, not as "spawn a separate helper next to the child".

Executable-format ownership should therefore be understood as a broker concern,
not a kernel runtime enum:

- `.wap` remains the built-in package/container path
- extra executable formats are broker-registered handlers
- PM should do only cheap matching plus spawn-plan validation/execution
- the owning broker must revalidate the full guest format before execution

---

### Filesystem Namespace

The bootstrap filesystem namespace used by the process manager and device manager:

| Path                        | Backend                                          |
|-----------------------------|--------------------------------------------------|
| `/`                         | Virtual root (no backing store)                  |
| `/boot`                     | `fs-fat` (active FAT partition endpoint)         |
| `/user`                     | Reserved for secondary FAT backend               |
| `/init/devmgr/rules`        | Device-manager rule root (initfs bootstrap)      |
| `/boot/system/devmgr/rules` | Device-manager rule root (runtime override, FAT) |

`fs-init` (`fs.init` endpoint) handles initfs listing. `fs-manager` routes
virtual-path requests to the appropriate backend endpoint. Bootstrap storage
drivers (`ata`, `fs-fat`) are always backed by initfs rules and cannot be
overridden by runtime FAT rules.

---

### Structural Invariants

1. **Parse before grant.** `wasmos_app_parse` runs to completion before any
   endpoint or capability action. A malformed container never partially grants
   resources.

2. **Capability names validated at pack time.** `make_wasmos_app` rejects
   unknown capability names. The kernel never sees a capability name it does
   not recognize.

3. **No shared runtime state.** Runtime instances are process-local. There is
   no global environment or cross-process module sharing regardless of backend.

4. **Preemption guarded around runtime calls.** Timer IRQs cannot interrupt
   wasm3 interpreter or WARP JIT state transitions.

5. **Native payloads require privilege.** `FLAG_NATIVE` without `FLAG_DRIVER`
   or `FLAG_SERVICE` is a parse error; the container is rejected before spawn.

---

### WARP JIT Runtime Integration

WARP (WebAssembly Resource-Efficient Processor, Apache-2.0 / BMW AG) is a
single-pass x86_64 JIT compiler that compiles WASM bytecode to native machine
code before first execution.  It is enabled with `-DWASMOS_WASM_RUNTIME_WARP=ON`
and lives in `libs/warp/` (git subtree).

#### Kernel porting layer

WARP is a C++14 hosted library.  The kernel provides a freestanding porting
layer in `src/kernel/warp/`:

| File                   | Purpose                                                                                                          |
|------------------------|------------------------------------------------------------------------------------------------------------------|
| `compat/`              | 30+ freestanding C++14 standard-library headers (type_traits, tuple, array, mutex, atomic, exception, …)         |
| `cxx_abi.cpp`          | Exception ABI: `__cxa_throw` longjmps to a per-CPU `__builtin_setjmp` checkpoint — no Dwarf/SJLJ unwinder needed |
| `link.cpp`             | ~50 `wasmos.*` V1 host-call wrappers (IPC, xfer buffers, block DMA, initfs, I/O ports, ACPI, scheduler, …)       |
| `shim.cpp`             | Two-tier kernel allocator (slab ≤ 112 bytes, page allocator for larger blocks), `operator new/delete`            |
| `mem_utils_kernel.cpp` | `vb::MemUtils` + `ExecutableMemory` backed by `pfa_alloc_pages` — no `<iostream>` or pthreads                    |
| `posix_kernel.c`       | `mmap`/`mprotect`/`munmap` → `pfa_alloc_pages` + higher-half mapping                                             |
| `linker_stubs.cpp`     | `malloc`, `memchr`, wasm3 symbol stubs, RTTI vtables                                                             |

`src/kernel/warp_driver.cpp` implements the full `wasm_driver_t` API using
`vb::WasmModule` as the backing runtime.

#### Exception boundary

WARP throws C++ exceptions internally.  These are contained within the
`warp_driver` layer via a per-CPU `__builtin_setjmp` checkpoint:

```c
WarpExceptionCheckpoint *ckpt = warp_exception_get_checkpoint();
ckpt->active = 1;
if (__builtin_setjmp(ckpt->jbuf)) {
    /* WARP threw — log and return error */
}
/* WARP API call */
ckpt->active = 0;
```

`__cxa_throw` checks `ckpt->active` and calls `__builtin_longjmp` if set.  No
exception ever propagates into C kernel code.

#### Memory model

WARP's global allocator is backed by the two-tier slab + page-allocator.  JIT
output pages are allocated below 512 MB (the kernel's higher-half identity
mapping window) so they are accessible at `phys | 0xFFFFFFFF80000000` and are
already mapped RWX by the initial page tables.

#### Host-call convention

Host functions follow WARP's V1 import convention — `ReturnType fn(Args..., void *ctx)` — where `ctx` is the `WarpCallContext*` that carries the `vb::WasmModule` pointer and calling PID.  Memory pointer arguments are raw i32 WASM offsets translated via `ctx->module->getLinearMemoryRegion(offset, size)`.

#### Known gaps

- `wasmos.console_read` is not implemented; the CLI traps on stdin reads (boot still succeeds).
- ~30 `wasmos.*` host-call TODOs remain in `src/kernel/warp/link.cpp` (shmem, IRQ routing, additional thread/sched ops).
- Multi-threaded WASM (`wasm_driver_spawn_vm_thread`) is not yet functional under WARP.
