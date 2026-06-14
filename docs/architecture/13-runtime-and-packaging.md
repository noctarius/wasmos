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

The parser in `wasmos_app.c` supports three header versions:

| Version     | Notes                                                         |
|-------------|---------------------------------------------------------------|
| 1           | No entry-arg bindings, no driver matches table                |
| 2           | Adds entry-arg bindings; single inline driver-match in header |
| 3 (current) | Adds `driver_match_count` and a separate per-match table      |

Version is checked before any pointer arithmetic. Unknown versions are parse
errors. The `reserved` field must be zero; a non-zero value is also a parse
error.

#### v3 Header Layout

```c
typedef struct __attribute__((packed)) {
    char     magic[8];                  /* "WASMOSAP" */
    uint16_t version;                   /* 3 */
    uint16_t header_size;               /* sizeof(wasmos_app_header_v3_t) */
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
    uint32_t reserved;                  /* must be zero */
} wasmos_app_header_v3_t;
```

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

#### Entry Arg Bindings

Up to 4 string names that map to runtime-supplied values at spawn time. The
process manager resolves each binding name to a `uint32_t` and passes the
resolved values as the four `wasmos_main` arguments. Common binding names:

- `proc.endpoint` — the process-manager's IPC endpoint ID
- `module.count` — number of WASMOS-APP modules loaded by the bootloader

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
2. Policy hooks set by `wasmos_app_set_policy_hooks()` resolve required endpoints
   and grant declared capabilities (callbacks into the process manager).
3. `wasmos_app_start(&instance, &desc, owner_context_id, init_argv, init_argc)`:
   - Calls `g_endpoint_resolver` for each entry in `desc.req_eps`.
   - Calls `g_capability_granter` for each capability in `desc.caps`.
   - Translates memory hints to byte sizes (`pages * 4096`, 64 KB floor).
   - Calls `wasm_driver_start()` which runs the startup sequence above.
4. `wasmos_app_call_entry(&instance)` — dispatches to `instance.entry` export
   (typically `initialize` or the resolved binding target).
5. The entry export runs the driver/service event loop via WASM host imports.

Parse errors, failed endpoint resolution, and failed capability grants all
abort before any runtime state is created.

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
| `link.cpp`             | ~50 `wasmos.*` V1 host-call wrappers (IPC, FS buffers, block DMA, initfs, I/O ports, ACPI, scheduler, …)         |
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
