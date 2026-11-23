---
name: wasmos-wasm-driver
description: Create, wire, and validate a new WASMOS device driver (WASM or native) — the `.wap` package model: an `initialize` entry that owns an event loop, service/class registration, capabilities declared in linker.metadata, build via a per-directory BUILD.py manifest (guest()/native() + install()), and boot-time bring-up through device-manager rules. Use when adding a driver under src/drivers/.
---

# WASMOS Device Driver

## Overview

A driver is a `.wap` package under `src/drivers/<name>/` that the **device
manager** spawns from disk at boot. It exports a single `initialize` entry that
creates an endpoint, registers a service, and runs its own event loop. WASM
drivers (C/C++/Rust/Zig/AssemblyScript, `native = false`) run ring-3 through
wasm3 or WARP; native drivers (C/Zig, `native = true`) run against the kernel
`wasmos_driver_api_t` table.

Legacy patterns that are GONE — do not use them:
- kernel-embedded blobs (`WASMOS_KERNEL_BLOBS`, a hand-written
  `src/kernel/wasm_foo.c` wrapper, `wasm_driver_start`);
- `foo_init` + `foo_ipc_dispatch` exports;
- `wasmos_endpoint_resolve` kernel wiring;
- hand-written wasm3 signature strings.

## Workflow

1. Choose runtime (WASM vs native) and create the source under `src/drivers/<name>/`.
2. Export `initialize`; register a service; run a blocking/event-loop.
3. Declare `linker.metadata` (package + capabilities + optional PCI match).
4. Add a `BUILD.py` (`guest(...)`/`native(...)` + `install(...)`); list the dir in `src/drivers/BUILD.py`.
5. Add a device-manager rule that spawns the `.wap`.
6. Build and boot in QEMU.

## Step 1: Source + runtime

All drivers live under `src/drivers/<name>/`. Representative examples:
`src/drivers/virtio_rng/` (WASM/C, DMA + PCI-matched), `src/drivers/chardev/`
(WASM/C, trivial), `src/drivers/uart16550/` and `src/drivers/pl011/`
(AssemblyScript: two chip backends sharing `src/drivers/serial/serial_core.ts`), and
`src/drivers/framebuffer_pci/` (native/C).

- WASM includes: `wasmos/api.h`, `wasmos/ipc.h`, `wasmos/libsys.h`,
  `wasmos/startup.h`, plus `wasmos_driver_abi.h` for opcodes (DMA adds
  `wasmos/vring.h`).
- Native includes: `wasmos_driver_abi.h` + `wasmos_native_driver.h` (the
  `wasmos_driver_api_t` table, ABI version at `wasmos_native_driver.h:171`).

## Step 2: The `initialize` entry + event loop

Export exactly one entry with `WASMOS_WASM_EXPORT`
(`src/libc/include/wasmos/imports.h:13`); it owns the driver's main loop and does
not return.

- WASM entry: `WASMOS_WASM_EXPORT int32_t initialize(void)`
  (`src/drivers/virtio_rng/virtio_rng.c:536`). It takes NO parameters: the
  process manager calls every entry with an argument count of zero, and wasm3
  refuses a call whose argument count does not match the declared signature
  ("argument count mismatch"), so a parameterised entry kills the driver at
  spawn under wasm3 while running fine under WARP. Fetch the process-manager
  endpoint via `wasmos_startup_proc_endpoint()` (spawn-info contract).
- Native entry: `int initialize(wasmos_driver_api_t* api, int module_count, int, int)`
  (`src/drivers/framebuffer_pci/framebuffer_pci_native.c:252`).

Inside `initialize`:
1. `ep = wasmos_ipc_create_endpoint()`.
2. Register the service:
   - plain name: `wasmos_svc_register(proc_ep, ep, "chardev", 1)`
     (`chardev_server.c:44`); or
   - **class** (preferred for backend-neutral drivers): `wasmos_svc_register_class(proc_ep, ep, "virtio-rng", "hrng", 0, 1)` — concrete name + virtual class (`virtio_rng.c:411`; needs the `svc.class` capability).
3. `wasmos_sys_notify_ready(proc_ep, ep)`.
4. Event loop that **blocks at idle** (hard rule: never busy-spin —
   `[[feedback_no_busy_spin_prefer_event_loops]]`): drain with
   `wasmos_ipc_drain(ep)` + `wasmos_ipc_message_read_last(&msg)`, dispatch, then
   `wasmos_ipc_select_wait_timeout(sel, ms)` (`virtio_rng.c:417-431`); or the
   simpler blocking `wasmos_ipc_select_one(ep)` (`chardev_server.c:50`).

Reply with `wasmos_ipc_send(dest, ep, type, req_id, a0..a3)` /
`wasmos_ipc_reply(...)`. Move bulk data through **borrowed transfer buffers**
(`wasmos_sys_buffer_write(buffer_id, ptr, n, 0)`), never inline; DMA drivers pin
device memory with `wasmos_region_alloc(pages, WASMOS_REGION_CACHE_WB, &phys)` +
`vring.h`. (AssemblyScript declares imports directly with
`@external("wasmos", ...)` — `serial.ts:34-47` — same wire protocol.)

## Step 3: `linker.metadata`

A TOML sidecar is the source of truth for identity + capabilities (not the code).
Real example (`src/drivers/virtio_rng/linker.metadata`):

```toml
version = 1
aot = true
[package]
name = "virtio-rng"
entry = "initialize"
kind = "driver"
native = false          # true selects the native ELF toolchain/runtime
[resources]
stack_pages = 16
heap_pages = 512        # native drivers use 0/0 and get heap via vm_map
[deps]                  # peers by name or "class:<name>"; data only, never gates start
needs = ["pci"]         # hard (cycle-checked at build time); also wants / provides
provides = ["hrng"]
[[capabilities]]        # one block per capability the driver needs
name = "io.port"
flags = 0
[[capabilities]]
name = "dma.buffer"
flags = 4               # DMA budget in pages
[[capabilities]]
name = "svc.class"
flags = 0
[[regions]]             # optional: register windows, in the order the driver
kind = "io"             # addresses them; declaration order IS the region index
first = 0x01F0
last = 0x03F7
[[regions]]
kind = "bar"            # a BAR of the bound device: I/O or memory-mapped by the
index = 4               # BAR's own kind (a memory BAR also needs mmio.map)
```

The driver reaches every region with `wasmos_io_region_in*/out*(region,
offset, ...)`, whichever its kind; an access to a memory-mapped region must be
naturally aligned. A region the device cannot supply keeps the driver from
being spawned for that device at all.

A manifest declares what the driver needs, never which device it gets. There is
no match table in the package: the device it binds to is decided by a rule (Step
5), so that binding lives in one place and can be changed without repacking.

Capability names in the tree: `io.port`, `irq.route`, `mmio.map`, `dma.buffer`,
`system.control`, `subsystem.register`, `svc.class`, `ipc.basic`.

## Step 4: Build (BUILD.py)

Each driver directory has a `BUILD.py` manifest whose `build(b)` uses the
helpers in `scripts/buildgen/wasmos_rules.py` (API reference:
`docs/BUILD_SYSTEM.md` §12):
- WASM (C, C++, Zig, Rust, Go, AssemblyScript): `guest(b, name, sources,
  lang="c")` compiles through the staged SDK driver for the language
  (`wasmos-clang`, `wasmos-zig`, `wasmos-asc`, …) and packs `<name>.wap` with
  `make_wasmos_app`. The entry export comes from `[package] entry` in
  `linker.metadata` (the default manifest), and the memory layout from its
  `[link]` section (`stack_size`, `initial_memory`, `max_memory`). Under WARP, a
  manifest with `aot = true` is AOT-compiled by `warp_aot` and bundled. C sources
  link in the listed order. `cxx=True` selects C++. Examples:
  `src/drivers/virtio_rng/BUILD.py` (C), `src/drivers/pl011/BUILD.py`
  (`lang="as"`, with sources from a shared directory).
- Native C/Zig: compile with `b.toolchain("native").compile(...)` (and
  `zig_object(...)` for a Zig root module), then `native(b, name, objs,
  entry="initialize", image_base=...)` links a static ELF and packs it without
  AOT (`src/drivers/framebuffer_pci/BUILD.py`).
- `install(b, "system/drivers/<name>.wap", wap)` stages the package on the ESP
  (`out/<dir>/esp/system/drivers/<name>.wap`). An initfs payload is not
  installed; `scripts/initfs.<arch>.toml` names it and the initfs step finds it by name
  in the build directory.
- Add the directory name to `b.subdir(...)` in `src/drivers/BUILD.py`.
  `build.ninja` regenerates on the next `ninja` invocation; no re-configure is
  needed.

Minimal manifest (`src/drivers/ata/BUILD.py`):

```python
from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "ata", [b.src("ata.c")])
    install(b, "system/drivers/ata.wap", wap)
```

## Step 5: Boot wiring — device-manager rules

The device manager spawns drivers from udev-style rules, two locations:
`scripts/initfs/devmgr/rules/default.<arch>.rules` (initfs bootstrap, boot-critical) and
`scripts/system/devmgr/rules/default.<arch>.rules` (boot-FAT, after storage is online).

```
SUBSYSTEM=="boot", RUN+="system/drivers/chardev_server.wap"
SUBSYSTEM=="acpi", ATTR{class}=="0x09", ATTR{subclass}=="0x00", RUN+="system/drivers/keyboard.wap"
SUBSYSTEM=="pci",  ATTR{vendor}=="0x1AF4", ATTR{device}=="0x1005", RUN+="system/drivers/virtio_rng.wap"
SUBSYSTEM=="block", ATTR{unit}=="1", ENV{MOUNT}="/user", RUN+="system/drivers/fs_fat.wap"
```

Rules are parsed by `src/services/device_manager/device_manager_rules.c`
(`always_spawn` / `block_fs` / `pci_match` / `acpi_match`) and applied as PCI/ACPI
inventory arrives (`device_manager.c` `queue_*_rule_spawns`). A PCI or ACPI
driver is spawned by path with `PROC_IPC_SPAWN_WATCHED` (`dm_watch_spawn_send`):
its startup args carry the device's identity (BAR/IRQ), which a PCI driver
parses (e.g. `virtio_rng.c` `probe_virtio_rng_from_startup_args`), and its
capability descriptor carries every declared region and the PCI function. The
rule is retired when the driver latches READY, so a driver must reach
`wasmos_sys_notify_ready` (or its language binding) within 5 s or its
device is given up. A driver the boot depends on must be in the initfs.

## Step 6: Verify

```sh
./configure out/wasm3-smp configs/x86_64_wasm3_smp_defconfig && \
  ninja -C out/wasm3-smp run-qemu-test                  # wasm3
./configure out/warp-smp configs/x86_64_warp_smp_defconfig && \
  ninja -C out/warp-smp run-qemu-test                   # WARP
```

Each configuration has its own build directory; the runtime is the
`WASMOS_WASM_RUNTIME_*` choice in that directory's `.config`.

Confirm the driver's ready log and exercise its IPC from a client. Remove debug
markers before committing.

## Guardrails
- Keep `libc`/`libsys` and their per-runtime wrappers in sync (repo rule).
- Never modify `libs/warp` or `libs/wasm3`.
- Never busy-spin; block on a select set at idle.
- Prefer `svc.class` registration + class discovery over hardwired names.
