## Runtime Hosting and WASMOS-APP Format

### Runtime Choice
The supported in-tree runtime is `wasm3`.

Current wasm3 integration guarantees:
- runtime instances are process-local
- runtime allocation uses a kernel-owned per-process chunked bump allocator
- runtime heaps grow incrementally and are capped at 2 GiB per process
- runtime create/load/call/free operations execute with preemption disabled so
  timer IRQs cannot interrupt runtime mutation

Current heap behavior:
- each process starts with a preferred heap chunk size derived from the loader
  manifest, with a practical default still centered around the old 4 MiB arena
- additional chunks are allocated on demand instead of requiring a single large
  contiguous reservation
- freeing and in-place `realloc` are still optimized for tail allocations only
- WASMOS-APP heap `max_pages` metadata is parsed but not enforced yet

### Historical WAMR Note
Earlier experiments with WAMR on a preemptive branch showed the interpreter
stalling at the glue-frame/IMPDEP handoff before reaching native imports.
Those notes were useful as a debugging record, but WAMR is not the supported
runtime in this tree and the repository policy remains: do not carry routine
instrumentation inside vendored runtime code. If alternate runtime work resumes,
compare against a non-preemptive baseline and keep debug instrumentation out of
the vendored subtree when possible.

### WASMOS-APP Container
WASMOS-APP exists to make boot and PM loading deterministic:
- fixed header
- explicit app name
- explicit entry export
- endpoint requirements
- capability requests
- memory hints
- raw payload bytes (WASM module or native ELF)

Current flag roles:
- driver
- service
- normal application
- privileged request
- native payload (valid only in combination with `driver`)

Current memory-hint behavior:
- stack `min_pages` affects runtime stack sizing
- heap `min_pages` affects the preferred initial runtime heap chunk size
- heap `max_pages` is reserved metadata for future enforcement

Current entry expectations:
- applications export `wasmos_main` through a language shim
- drivers and services export `initialize`
- native drivers use ELF `e_entry` to point at `initialize(wasmos_driver_api_t *, int, int, int)`

Current packaging input:
- all in-tree app/driver/service modules define metadata in colocated TOML
  `linker.metadata` files
- `scripts/make_wasmos_app.c` consumes those manifests via
  `--manifest <file> --in <module> --out <app>`
- driver manifests can carry zero or more `[[matches]]` records so one driver
  can advertise multiple valid PCI identification mappings
- process-manager supports initfs metadata lookup by module path via
  `PROC_IPC_MODULE_META_PATH` (returns sanitized metadata fields to user-space
  callers and keeps PM-only loader internals private)

### Language ABI Strategy
Applications no longer need to implement the raw startup ABI directly:
- the C shim exports `wasmos_main` and calls `main(int argc, char **argv)`
- the Rust shim exports `wasmos_main` and calls `main(args: &[&str])`
- the Go shim exports `wasmos_main` and calls `Main(args []string) int32`
- the AssemblyScript toolchain-owned root module exports `wasmos_main` and
  delegates to `main(args: Array<string>): i32`
- the Zig shim exports `wasmos_main` and keeps a Zig-native `main`

This keeps the external ABI stable while presenting language-native entrypoints.
