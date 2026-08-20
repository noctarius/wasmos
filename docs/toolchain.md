# WASMOS Toolchain and SDK

> **Documentation status: reference for Stage 1, proposal beyond it.** The staged
> SDK, its sysroot, the `wasmos-clang` driver and the packaging step are
> implemented and covered by tests. compiler-rt, C++ and a native
> `wasm32-unknown-wasmos` LLVM triple are not.

The WASMOS SDK is the in-tree toolchain repackaged so that a developer outside
this repository can build a WASMOS application without knowing a target triple, a
sysroot, a linker flag, or the container format:

```bash
cmake --build build --target wasmos-sdk
export PATH=$PWD/build/wasmos-sdk/bin:$PATH

wasmos-clang hello.c -o hello        # -> hello.wap, runnable by WASMOS
```

---

## Stage 1 versus Stage 2

| | Stage 1 (current) | Stage 2 (not implemented) |
|---|---|---|
| Triple given to clang | `wasm32-unknown-unknown` | `wasm32-unknown-wasmos` |
| Triple the SDK reports | `wasm32-unknown-wasmos` | same |
| WASMOS knowledge lives in | the `wasmos-clang` driver | an LLVM/Clang toolchain class |
| LLVM fork required | no | yes |

Stage 1 deliberately requires no LLVM modification. `wasmos-clang
--print-target-triple` already answers `wasm32-unknown-wasmos`, so build systems
can key off the canonical name before LLVM knows it; the generated code is
identical either way, which is what makes the migration a non-event. The
`WASMOS_SDK_TARGET` / `WASMOS_SDK_TARGET_LLVM` split in
`cmake/WasmosSdk.cmake` is where the two names diverge.

---

## The ABI a compiled application must satisfy

`abi/hostcalls.yaml` is the single source of truth for the call surface and
`docs/architecture/13-runtime-and-packaging.md` for the entry and container
contract. What a toolchain must get right:

**Entry point.** There is no `_start` and no WASM start function. The kernel calls
an *export by name*:

| Package kind     | Export             | Supplied by                                        |
|------------------|--------------------|----------------------------------------------------|
| app              | `wasmos_main`      | `crt1.o` (built from `src/libc/src/startup.c`)     |
| driver / service | `initialize`       | the component itself; no crt is linked             |
| async service    | `async_initialize` | `src/libsys/wasm/service_async_entry_wasm.c`       |

The signature is `(i32, i32, i32, i32) -> i32` and all four arguments are always
zero. `crt1.o` calls `main()` and then `wasmos_proc_exit()` with its return value,
so an application never writes either.

**Startup values** are not in registers. They live in a spawn-info transfer
buffer: `wasmos_spawn_info_buffer()` returns a `buffer_id` whose contents are read
with `xfer_buffer_read`. Use the accessors in `wasmos/startup.h`
(`wasmos_startup_args`, `_proc_endpoint`, `_tty`, `_module_count`,
`_module_index`).

**Arguments** arrive as **one NUL-terminated string**, not an argv array.
`wasmos_startup_argv()` tokenizes it and `crt1.o` calls `main(argc, argv)` with the
result, so plain C works:

```c
int main(int argc, char **argv) {           /* argv[1] is the first argument */
    const char *path = argc > 1 ? argv[1] : "/boot/default";
```

Two properties of that argv are worth knowing:

- **`argv[0]` is an empty string, not the program name.** The contract carries only
  what followed the command name, and `wasmos_spawn_info_t` has no name field. The
  slot exists anyway so `argv[1]` is the first argument as the language says;
  filling it means appending a name to the spawn-info header, which is a `TODO` at
  the accessor.
- **An argument the buffer cannot hold whole is dropped, not truncated.** A
  silently shortened path or number is a failure nothing downstream can detect,
  while one fewer argument is visible in `argc`.

This is currently the **C path only**. The Rust, Go, Zig and AssemblyScript entry
shims still call their `main` with an empty argument list, so a guest in those
languages reads `wasmos_startup_args` and tokenizes it itself. Closing that is
tracked in `docs/TASKS.md`.

**Imports** are declared, never inferred. Every host call is an import of module
`wasmos` declared through `WASMOS_WASM_IMPORT` in `wasmos/api.h`, generated from
the IDL. The linker runs *without* `--allow-undefined`, so a symbol that is
neither defined nor a declared import is a link error rather than a silent import
that traps at the call site.

**Exports** are exactly `memory` and the entry symbol; `--strip-all` removes the
rest.

**Predefined macro.** `__wasmos__` is defined to 1. `__unix__`, `__linux__` and the
POSIX feature macros are not: WASMOS satisfies none of those contracts.

---

## SDK layout

```text
build/wasmos-sdk/
├── bin/            wasmos-clang, wasmos-clang++, wasmos-zig, wasmos-asc,
│                   wasmos-ld, wasmos-ar, wasmos-nm, wasmos-ranlib,
│                   wasmos-strip, wasmos-objdump, wasmos-pack, wasmos-inspect
├── libexec/wasmos/ make_wasmos_app, wasm_inspect.py, wasm_stack_check.py,
│                   as_coroutine_transform.mjs
├── sysroot/
│   ├── include/    libc headers, sys/, and wasmos/ (libc + libsys + libui),
│   │               with the generated ABI headers under wasmos/abi/
│   └── lib/wasm32-unknown-wasmos/  crt1.o, libc.a, libsys.a
├── share/
│   ├── cmake/WASMOS/  WASMOSToolchain.cmake, Platform/WASMOS.cmake
│   └── wasmos/        default-manifest.toml, zig/*.zig, assemblyscript/*.ts
└── wasmos-sdk.conf  resolved tool paths and version, sourced by the wrappers
```

The sysroot is relocatable: each wrapper resolves `bin/../sysroot` from its own
real path, so the SDK can be moved or extracted anywhere. Stage 1 borrows the host
LLVM rather than shipping one, so `wasmos-sdk.conf` records absolute clang paths;
that file is the only thing tying a staged SDK to the machine that built it.

### Why the sysroot rewrites one header

`src/libc/include/wasmos/api.h` includes the generated ABI headers by a
repo-relative path (`../../../../abi/generated/c/…`). That is deliberate in-tree:
the generated files live outside `src/` to stay out of format and lint scope, so
no `-Iabi/generated/c` has to be threaded through every compile that pulls in
libc. No sysroot can reproduce that depth, so `cmake/wasmos_sdk_stage.cmake`
installs the generated headers under `sysroot/include/wasmos/abi/` and rewrites
those two includes. The rewrite is asserted, not assumed — a silent miss would
produce a sysroot that cannot compile anything — and
`tests/test_sdk_abi.py` checks the installed result.

---

## Compiler usage

```bash
wasmos-clang hello.c -o hello              # module + .wap
wasmos-clang -O2 hello.c -o hello
wasmos-clang --emit-wasm hello.c -o hello.wasm     # stop at the module
wasmos-clang --wasmos-manifest=app.toml app.c -o app
wasmos-clang++ app.cpp -o app              # -fno-exceptions -fno-rtti
```

A `-c` invocation compiles only: no crt, no libraries, no packaging. That is the
shape a build system drives, compiling each source before linking the objects, and
it is what makes the CMake integration below work.

Configuration queries use the standard clang spellings where they exist:

```bash
wasmos-clang --version              # SDK version, then the underlying clang
wasmos-clang --print-sysroot
wasmos-clang --print-target-triple
wasmos-clang --print-manifest       # the manifest this invocation would use
```

An output name without `.wasm` produces a `.wap`; `--emit-wasm` or an explicit
`.wasm` output stops at the module. The `.wasm` is never hidden — the WebAssembly
pipeline stays independently testable.

## Zig

```bash
wasmos-zig app.zig -o app            # -> app.wap
wasmos-zig --emit-wasm app.zig -o app.wasm
```

The Zig driver hides more than the C one, and two of the things it hides are not
conveniences:

- **The 8 KiB shadow stack is mandatory.** Zig's default is 1 MB, which places the
  app's globals at ~1 MB — past the 64 KiB user-VA mirror region each process gets
  — and every host call that writes into WASM memory then rejects the pointer
  *silently*. The driver always passes `--stack`, and afterwards runs
  `wasm_stack_check` and **refuses to emit a module that violates the layout**,
  rather than leaving it to be discovered as a service that mysteriously fails to
  register.
- **The runtime shims are staged, not passed.** Zig resolves
  `@import("wasmos.zig")` beside the importing file, so the driver copies the app
  and `share/wasmos/zig/{wasmos,coroutine}.zig` into one directory and compiles
  there. That is why the shims live under `share/` rather than in the sysroot:
  they are source compiled with the app, not headers or archives.

A Zig guest's `main` returns `u8`, not `void` — the shim's `wasmos_main` export
casts it to the process exit status.

Additional `.zig` files passed on the command line are staged alongside. Extra C
objects (the coroutine and libui shims that `examples/zig/calculator` links) are
not wired into the driver yet; that app is still built by the in-tree helper.

## AssemblyScript

```bash
wasmos-asc app.ts -o app             # -> app.wap
wasmos-asc --emit-wasm app.ts -o app.wasm
```

What this driver hides follows from how `asc` resolves imports: it has no include
path and resolves every import relative to the entry file. So the whole AS runtime
is staged **flat** beside a copy of the app, and the entry is the runtime's own
`runtime.ts`, which imports `"./app"` — the developer's file is staged under that
name whatever it is called. The flat `"./name"` import convention across the AS
sources is a consequence of this, not a style choice.

Two fixed choices come with it. `--transform as_coroutine_transform.mjs` lowers
`@coroutine` functions into their state machines and is inert for a module that
uses none, so it applies unconditionally rather than being an opt-in to remember.
`--runtime stub` is the WASMOS choice: AS's incremental GC is not usable here.

An AS guest's `main` takes the args array — `export function main(args:
Array<string>): i32` — and omitting the parameter does not compile.

`[link] initial_memory` is in bytes like every other language; `asc` takes pages,
and a value that is not a whole number of 64 KiB pages is refused rather than
rounded.

## Linker behavior

The driver passes, and a developer does not:

| Concern | Default |
|---|---|
| Entry | `--no-entry` plus `--export=<manifest entry>` |
| Undefined symbols | an error; `--allow-undefined` is **not** passed |
| Stack | `-z stack-size=` from the manifest's `[link]` |
| Initial / max memory | from `[link]`; equal unless the app declares growth |
| Symbols | `--strip-all` |
| Runtime libraries | `crt1.o`, `-lc`, `-lsys` |

## C++

`wasmos-clang++` works for freestanding C++: classes, templates, and anything that
needs no runtime support. It passes `-fno-exceptions -fno-rtti
-fno-threadsafe-statics -fno-use-cxa-atexit`, and there is no wasm32 `libc++`, so
the standard library is not available — `<vector>` and `<string>` are a later
milestone.

One difference from the C path is load-bearing and worth knowing before it bites:
**the C++ compile does not pass `-ffreestanding`.** Under `-ffreestanding` the
standard gives `main` no special status, so clang mangles it to `_Z4mainv` and
`crt1.o`'s `extern int main(int, char **)` goes unresolved — the failure surfaces
as `undefined symbol: main` from a program that plainly defines one. Dropping the
flag restores `main` as the program entry and costs nothing else: `-nostdlib`
still links no host library, the sysroot headers still satisfy every include,
`-fno-builtin` still blocks libc-shaped lowering, and the `__main_void` /
`__original_main` adapters clang then emits are unreferenced and collected by the
linker. A native `wasm32-unknown-wasmos` target would state the entry contract
directly, which is the Stage 2 fix; until then this asymmetry is deliberate and
lives in one place, `scripts/sdk/wasmos-clang`.

## Runtime libraries

| Library | Contents |
|---|---|
| `crt1.o` | the `wasmos_main` export and the bridge to `main()` |
| `libc.a` | `string`, `stdio`, `stdlib`, `ctype`, `math`, `unistd`, `spawn_info`, `ipc_managed`, `script` |
| `libsys.a` | coroutines, IPC futures, the service runtime |

`stdint.h`, `stddef.h`, `stdbool.h` and `stdarg.h` come from clang's freestanding
resource headers and are not shipped in the sysroot.

There is no `errno.h`, and there will not be one: WASMOS reports failure through
the packed `(domain, code)` values single-sourced in `abi/errors.yaml` (see
`skills/wasmos-add-error`), and a parallel POSIX `errno` channel would reintroduce
exactly the ambiguity that model removes. `wasmos/abi/wasmos_status.h` is the
header that answers "what went wrong".

There is also no `libclang_rt.builtins-wasm32.a`, and the gap is narrower than a
freestanding target usually implies. WebAssembly has native i64 divide, remainder
and multiply and native float conversions, so **64-bit C arithmetic needs no
compiler-rt at all**. The only shape that does is `__int128`, which reaches exactly
eight symbols:

```
__multi3  __udivti3  __divti3  __umodti3  __modti3     (integer)
__fixdfti  __fixunsdfti  __floatuntidf                 (double <-> i128)
```

Nothing in the tree uses `__int128`, and because the C link does not pass
`--allow-undefined`, a guest that reaches one gets a link error naming the symbol
rather than a module that loads and traps. `tests/test_sdk_arithmetic.py` pins both
halves of that boundary — 64-bit code links, `__int128` fails by name — so building
compiler-rt later is a change that test will notice.

---

## Manifest

The manifest is the `.wap` container's input and the driver's configuration. The
grammar is the TOML subset `scripts/make_wasmos_app.c` accepts. `[link]` is read
by `wasmos-clang` and ignored by the packer:

```toml
version = 1

[package]
name = "hello"
entry = "wasmos_main"
kind = "app"            # "app" | "driver" | "service"
native = false

[resources]              # kernel runtime hints, in 4 KiB pages
stack_pages = 16
heap_pages = 16

[link]                   # link-time sizes in bytes, read by wasmos-clang
stack_size = 4096
initial_memory = 65536
max_memory = 65536

[ipc]
required_endpoint_name = "-"

[[capabilities]]
name = "ipc.basic"
flags = 0
```

`[resources]` and `[link]` are different things with confusingly similar names:
`[resources]` sizes the *kernel's* runtime allocations for the process, `[link]`
sizes the *module's* own linear memory and shadow stack.

`[link]` is where an app's memory lives for the in-tree build too, not only for the
SDK: `wasmos_add_wasm_c_app_target` reads it at configure time and passing
`STACK_SIZE`/`INITIAL_MEMORY`/`MAX_MEMORY` to the helper is now a configure error
naming the manifest to move them to. Omit a key to take the default; writing a
default out explicitly is noise a test rejects, so a value present in a manifest is
one somebody chose. `tests/test_link_memory_manifest.py` checks that what a
manifest asks for is what the module declares — the failure it exists for is a
module that links fine and is sized wrong, which does not surface at the link step
but later, inside a host call whose window did not fit.

All four toolchains read `[link]` — C, Zig, AssemblyScript and Rust — so an app is
sized in one file whatever it is written in. The section is in **bytes** in every
language; `asc` wants pages, and the conversion happens in one place
(`wasmos_manifest_link_pages`), where a value that is not a whole number of 64 KiB
pages is an error rather than a silent round. Each helper keeps its own defaults for
keys a manifest omits — the C one pins a maximum, the Zig and AssemblyScript ones
leave the module without any — so only declared keys are checked.

With no `--wasmos-manifest=`, the driver falls back to
`share/wasmos/default-manifest.toml` and substitutes the output basename for the
package name, leaving the substituted copy beside the output as
`<name>.manifest.toml` — so what was packed is inspectable rather than implied. Those defaults describe a one-page application and are only right
for trivial programs: an application that maps shared memory, a framebuffer, or
socket rings must reserve linear memory up front, because those windows are placed
above a 2 MiB floor and above live data. Every non-trivial app therefore ships its
own manifest.

Names destined for the ESP must survive FAT 8.3, which is why the SDK's own smoke
app is staged as `sdkhello.wap`.

---

## One toolchain, not two

The in-tree build and the SDK are the same toolchain: `wasmos_add_wasm_c_app_target`
links `crt1.o`, `libc.a` and `libsys.a` out of the staged sysroot rather than
recompiling libc into each module. So there is one libc build instead of one per
module, and one link line to keep correct instead of two that can drift — a change
to a libc source or a linker default reaches every guest and the SDK at once.

Two consequences worth knowing:

- **`llvm-ar` is required to build anything.** It builds the archives every WASM
  target links against. It ships with the same LLVM install that provides the
  clang, lld and `llvm-objcopy` this build already needs.
- **A symbol defined by both an application and libc is no longer a duplicate
  symbol error.** Archive members are pulled only to resolve something undefined,
  so the application's definition silently wins. That was a link failure when libc
  was compiled into every module.

Only *entry* shims are still compiled per target: an entry symbol has to be
present whether or not anything references it, which is exactly what an archive
will not guarantee. `crt1.o` is named on the link line for the same reason.

## Building the SDK

```bash
cmake -S . -B build
cmake --build build --target wasmos-sdk
```

The `wasmos-sdk` target stages the SDK tree; the archives and `crt1.o` inside it
are built by the ordinary build, because every WASM target already depends on them.

`cmake/WasmosSdk.cmake` compiles the sysroot objects with the same flags
`wasmos-clang` passes for application sources — they must match, or an archive
object and its caller would disagree about, for example, `WASMOS_TRACE` — archives
them with `llvm-ar`, and runs `cmake/wasmos_sdk_stage.cmake` to populate the
header tree, the wrappers and the CMake integration. The target is skipped when
`llvm-ar` is absent.

## CMake integration

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=$WASMOS_SDK/share/cmake/WASMOS/WASMOSToolchain.cmake ..
```

`CMAKE_SYSTEM_NAME` is `WASMOS` and the platform module beside the toolchain file
describes it. It is deliberately not `Linux`. Because the driver's output is a
WebAssembly module rather than a host executable, the toolchain file sets
`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` so CMake's compiler probe does not
try to run what it builds.

---

## Adding a libc function

1. Declare it in the appropriate `src/libc/include` header and implement it in the
   matching `src/libc/src/*.c`. Do not add a POSIX surface WASMOS cannot honour.
2. If the file is new, add it to `WASMOS_SDK_LIBC_SOURCES` in
   `cmake/WasmosSdk.cmake` — the archive is now the only place libc is built, so
   that is the only list. Archive membership costs nothing at link time: an object
   no module references is not pulled in. `WASMOS_LIBC_C_SOURCES` in the root
   `CMakeLists.txt` is for entry shims only, and a libc source added there would be
   compiled into every module again.
3. Cover it with a host unit test under `tests/unit/` (`test_libc_*.c` are the
   precedents) and keep the other language bindings in sync where the API is shared
   (repo rule: libc and its wrappers change together).

## Debugging linking problems

- **`undefined symbol: X`** — with `--allow-undefined` gone this is the expected
  failure for a missing source file or a stub that never got linked. Find which
  object defines `X` and add it to the link, rather than reaching for
  `--allow-undefined`.
- **The module loads but traps at a call** — an import the runtime does not
  provide. `wasmos-inspect` the module and compare its import list against
  `abi/generated/docs/hostcalls.md`.
- **Host calls fail silently at runtime** — a pointer above the process's user-VA
  mirror region (16 pages, 64 KiB; `src/kernel/memory.c`). Check where the module
  places its globals: a large shadow stack pushes them out of range. This is why
  Zig apps build with `--stack 8192` and are verified by
  `scripts/wasm_stack_check.py`.
- **A WARP AOT payload does not load** after a host-call change — rebuild
  `warp_aot_tool` and clear `.cache/warp_aot`.

## Inspecting generated WebAssembly

```bash
wasmos-inspect hello.wasm       # imports, exports, section shape
wasmos-inspect hello.wap        # container header, then the module
```

`tests/test_sdk_abi.py` turns this into an assertion: every import must be a
`wasmos.*` name declared in `abi/hostcalls.yaml`, exports must be exactly `memory`
plus the entry, and a C module must reach no WASI import. The allowlist is
deliberate rather than a blanket "no WASI" rule — `abi/hostcalls.yaml` declares two
`wasi_snapshot_preview1` calls on purpose, WARP-only — so the property worth
guarding is that nothing *outside* the declared ABI appears.
