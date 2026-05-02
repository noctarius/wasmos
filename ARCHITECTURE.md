# Architecture Notes

This document is the repository's technical design baseline. It describes the
boot contract, the microkernel split, the current implementation state, and the
next layers that still need to be built.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.
Threading design details are maintained in `THREADING.md`.
Full ring-3 isolation execution planning is tracked in
`RING3_ISOLATION_PLAN.md`.

## Goals
- Boot an x86_64 kernel through UEFI with a deterministic, auditable handoff.
- Keep the kernel small: scheduling, IPC, memory, interrupts, and runtime
  hosting are kernel responsibilities; policy lives in WASM services.
- Treat services and applications as isolated WASM programs and allow selected
  drivers to run as native ELF payloads when needed, all behind explicit IPC
  contracts instead of implicit in-kernel calls.
- Preserve a stable boot and process ABI while still allowing the system to
  evolve incrementally.

## Current System Summary
The current tree already boots into a usable user-space stack:
- `BOOTX64.EFI` loads `kernel.elf` plus a single `initfs.img`, gathers the
  memory map, materializes bootstrap boot modules from the initfs, and exits
  boot services.
- The kernel initializes paging, physical memory management, exceptions, the
  timer, IPC, the scheduler, and the process manager.
- A kernel-owned `init` process starts `hw-discovery`, waits for `fs-fat` to
  become ready, and then asks the process manager to load `sysinit` from the
  FAT filesystem. `hw-discovery` now limits the pre-FAT bootstrap to storage,
  then starts display/input drivers from FAT by name once `fs-fat` can serve
  process-manager reads; the kernel early framebuffer is the only display path
  needed before FAT.
- `sysinit` is intentionally small and starts the configured post-FAT services
  and user processes listed in the generated boot-config blob.
- The initfs also carries a generated binary boot-config blob derived from
  `scripts/initfs.toml` for config-driven startup. Native framebuffer, serial,
  keyboard, VT, and CLI payloads stay on the FAT image and are loaded by name
  after `fs-fat` is available; hardware drivers are still owned by
  `hw-discovery`, while `sysinit` starts higher-level services/apps.
- `fs-fat` currently provides read-only open/read/seek/stat primitives for the
  shared libc layer and the language-native shims.
- `fs-fat` also supports overwrite-only writes to existing files through the C
  libc `open/write` path, plus `O_TRUNC` size updates, `O_APPEND` writes for
  existing files within their current cluster chain, and `O_CREAT` for
  zero-length 8.3 files in existing directories; FAT12/16 cluster allocation
  now grows files and newly created files, long-filename creates now emit LFN
  entries plus a generated short alias, regular-file unlink now reclaims the
  cluster chain and tombstones the short+LFN dir entries, empty-directory
  create/remove now allocates a directory cluster plus `.`/`..` entries, and
  the C stdio shim now exposes `fopen`/`fwrite` write and append modes. The C
  libc now exposes `unlink`, `mkdir`, and `rmdir`, and the Rust, Zig, Go, and
  AssemblyScript shims expose matching create/write/append/unlink/mkdir/rmdir
  helpers. Update modes such as `r+`/`w+`/`a+` and non-ASCII LFN creation
  remain future work.
- The runtime host uses `wasm3`, not WAMR.
- The process manager also supports native ELF drivers wrapped in WASMOS-APP as
  `FLAG_DRIVER|FLAG_NATIVE`, loaded directly into a process context and called
  through a kernel-provided function-table ABI.
- Native framebuffer startup now registers its text-control IPC endpoint back
  into process-manager state, so downstream VT instances receive a concrete
  framebuffer endpoint for switch clear/replay control instead of degrading to
  logical-only tty switching. The driver maps only the framebuffer byte range
  captured by the bootloader and keeps the boot-provided geometry when Bochs
  VBE reports a larger post-boot mode, preserving the kernel's framebuffer
  mapping contract until explicit native-driver mode setting is introduced.
  It is launched from FAT by `hw-discovery` after the storage bootstrap
  completes, with the kernel early framebuffer covering pre-FAT diagnostics and
  panic rendering.
- Serial-to-framebuffer text handoff now uses a kernel-created shared-memory
  console ring (1 page). `serial_write` appends bytes into this ring, and the
  native framebuffer driver maps and drains it, removing the previous
  serial→framebuffer text IPC message path.
- Shared-memory primitives now exist for both native-driver ABI
  (`shmem_create/map/unmap`, `console_ring_id`) and WASM syscalls
  (`wasmos_shmem_create/map/unmap`) backed by the same kernel shared-memory
  registry.
- `wasmos_console_read` now mirrors successful user-copy input bytes into the
  immediate wasm host-pointer view (in addition to `mm_copy_to_user`) to keep
  serial-input consumers aligned during ring3 copy-path migration.
- The VT WASM service now maintains explicit per-TTY state (4 tty slots),
  supports active-tty selection (`VT_IPC_SWITCH_TTY`), and stores per-tty
  attributes (`VT_IPC_SET_ATTR_REQ`) while output remains routed through
  `wasmos_console_write` into the serial/console-ring path.
- VT tty roles are now split intentionally: `tty0` reflects the system
  serial/console-ring output, while `tty1+` are VT-managed framebuffers.
  Framebuffer control IPC now includes a console-mode toggle so VT can disable
  console-ring drain when non-zero ttys are active and restore it on `tty0`.
  tty switches clear the framebuffer before replaying the selected tty buffer,
  and switch-time clear/replay now uses a higher-reliability IPC send path so
  redraw is not skipped under transient framebuffer queue backpressure; VT now
  fails switch requests when switch control-plane IPC (console-mode/clear) cannot
  be delivered so clients do not receive false-positive switch success. VT switch
  state commit is atomic with successful switch control operations (generation +
  active tty update only after control success), failed switches restore the
  prior framebuffer console mode to avoid half-switched states, and replay is
  intentionally best-effort under sustained queue pressure to avoid repeated
  switch abort loops. The native framebuffer driver now services control IPC
  before draining console ring backlog and now drains ring bytes in bounded
  chunks so switch clear/replay commands are not starved by heavy log traffic.
  When console mode is re-enabled for `tty0`, the framebuffer now drops stale
  ring backlog and resumes in live-tail mode, preventing large catch-up floods
  from monopolizing the display path after long `tty1+` sessions.
- CLI now receives the VT endpoint from process-manager wiring, switches to
  `tty1` at startup, and sends terminal output through `VT_IPC_WRITE_REQ`
  rather than direct console writes. VT write retry budgets were raised to
  reduce dropped output chunks under queue pressure during larger command
  bursts. Filesystem `ls`/`cat` output now returns from `fs-fat` over a
  requester-scoped IPC stream and is rendered by the requesting CLI instance,
  so multi-tty sessions keep file listings/content on the active tty rather
  than global console-only output.
- CLI now exposes VT switch failure codes in `tty` command errors so switch
  failures can be mapped to explicit VT control-plane failure paths.
- CLI `cd` path tracking now keeps standard dot-segment semantics (`.` stays in
  place, `..` resolves to the parent) instead of collapsing both to `/`.
- Process-manager assigns CLI home ttys and sysinit launches configured CLI
  targets with a cap of three concurrent instances (`tty1..tty3`). CLI input
  still gates on VT foreground selection.
- VT now owns keyboard input routing end-to-end and delivers per-tty raw key
  input over `VT_IPC_READ_REQ`. CLI remains the owner of line editing/echo;
  raw mode now emits extended arrows plus nav/edit keys as ANSI escape bytes
  (`ESC[A/B/C/D`, `ESC[H/F`, `ESC[5~/6~`, `ESC[2~/3~`), CLI now consumes
  `ESC[A`/`ESC[B` for shell history navigation in raw mode, raw printable keys
  now honor Shift-modified ASCII symbols (Set-1 map), and serial console
  reads are retained as fallback for headless/automated test flows.
- VT now applies a core CSI/SGR subset per tty state (`A/B/C/D/H/f`, `s/u`,
  `J`, `K`, private `?25h/l`, `m` with 16-color mapping), so replayed tty
  buffers preserve cursor/erase/color effects across switches.
- VT now queries framebuffer text geometry during startup and sizes tty state
  to that runtime grid (with fixed upper bounds) rather than hardcoding 80x25,
  preventing premature scroll on larger framebuffer text layouts. Per-tty cell
  storage is now allocated dynamically from WASM linear memory at startup
  (with explicit memory growth when needed), and VT falls back to default
  geometry if larger-grid allocation cannot be satisfied.
- VT write routing now uses caller endpoint ownership to target the correct tty
  buffer; non-foreground tty writes are buffered without rendering over the
  active framebuffer.
- Kernel-hosted WASM `console_write` now also mirrors output into VT as
  kernel-origin write chunks targeted at the active tty (best-effort, no
  writer registration/generation token), while still emitting serial output for
  headless logging and existing diagnostics/tests.
- VT now accepts `VT_IPC_SET_MODE_REQ` so clients can select per-tty input
  handling (`raw`, `canonical`, `echo`) through the same owned endpoint used
  for VT writes/reads.
- VT canonical input handling now includes baseline in-service line discipline
  controls (`Backspace`, `Ctrl+U`, `Ctrl+C`) plus per-tty history navigation
  (`Up/Down` arrows with `Ctrl+P`/`Ctrl+N` fallback), so cooked-mode consumers
  can rely on VT-side editing/interrupt delivery semantics.
- VT enforces explicit writer registration (`VT_IPC_REGISTER_WRITER`) and
  switch-generation write tokens: writes tagged with older generations are
  dropped after tty switches, and switch replay runs behind a temporary render
  barrier to avoid in-flight foreground repaint races. VT reply paths now also
  use bounded retry/yield behavior under queue pressure, reducing lost
  response windows that previously surfaced as CLI-side switch timeouts.
- VT can emit compact switch/ownership/drop telemetry through
  `wasmos_debug_mark` into the kernel's global trace stream when
  `WASMOS_TRACE=1`, so race analysis uses existing tracing infrastructure.
- Known deferred VT issue: an intermittent framebuffer-only prompt
  duplication/misalignment artifact during rapid `Ctrl+Shift+Fn` switching was
  observed earlier. It is currently not reproducible in recent runs, so the
  issue is deferred until a stable repro path is available.
- VT keyboard hotkeys support `Ctrl+Shift+F1..F4` to switch directly to
  `tty0..tty3`. While active tty is `tty0`, plain `F2/F3/F4` also switches
  directly to `tty1..tty3` as a recovery path when modifier tracking is not
  reliable.
- Entering `tty0` now renders a short read-only hint line so a blank
  live-tail console state is visibly intentional and provides immediate switch
  guidance.
- Keyboard event delivery into VT is now explicit fire-and-forget
  (`KBD_IPC_KEY_NOTIFY` with `request_id = 0`), and VT/CLI output transport
  loops now use bounded `IPC_ERR_FULL` retries so queue backpressure degrades
  output before it can freeze the interactive path.
- Fatal CPU exceptions still log to serial and now also render an in-kernel
  framebuffer panic screen (black background) with key crash diagnostics
  including exception registers, process identity, stack bounds, CR3/kernel
  text range, and framebuffer geometry/base.
- x86 syscall boundary groundwork now exists via a DPL3-callable `int 0x80`
  gate and a minimal syscall dispatcher (`nop`, `getpid`, `exit`, `yield`,
  `wait`, `ipc_notify`, `ipc_call`) so ring3 transition work can pivot from
  hostcall-only assumptions to an explicit kernel entry. Syscall argument
  handling now enforces 32-bit cleanliness for current 32-bit field-based
  calls (`wait`, `ipc_notify`, `ipc_call`) before dispatch.
- The libc include tree now exposes a native-only syscall helper header at
  `lib/libc/include/wasmos/syscall_x86_64.h` that mirrors the current register
  ABI for non-WASM x86_64 userland experiments (`int 0x80` wrappers with
  primary return in `RAX` and optional secondary return in `RDX` for
  `ipc_call`; current `ipc_call` semantics are `RAX=status`, `RDX=reply arg0`
  on success, `RDX=0` on error, and blocking wait for matching `request_id`).
- Scheduler context state now tracks privilege-return metadata (`cs`, `ss`,
  `user_rsp`) and context-switch restore now branches to `iretq` for ring3
  contexts while preserving `ret` for ring0 contexts.
- Scheduler now updates TSS `rsp0` per selected process before context switch,
  establishing the kernel-stack landing point used by user-mode trap/syscall
  entry.
- Kernel startup now provisions a dedicated ring3 smoke task (`ring3-smoke`):
  it copies a tiny user-mode `int 0x80` loop into the process linear region,
  marks that region executable, and flips the process into CPL3 via
  `process_set_user_entry`. The syscall handler logs `[test] ring3 syscall ok`
  on the first CPL3 `getpid` call as an end-to-end transition checkpoint; it
  now also probes `ipc_notify` deny/allow paths and logs
  `[test] ring3 ipc syscall deny ok` and `[test] ring3 ipc syscall ok` from the
  syscall layer when those outcomes are observed. It also probes `ipc_call`
  invalid/permission-denied/allow behavior via invalid-endpoint rejection, a
  kernel-owned permission-denied endpoint, and a kernel echo endpoint,
  logging `[test] ring3 ipc call deny ok`,
  `[test] ring3 ipc call err rdx zero ok` (error-path secondary return
  contract), 
  `[test] ring3 ipc call perm deny ok`, and `[test] ring3 ipc call ok` when
  observed. It also issues an explicit CPL3 `yield` syscall and logs
  `[test] ring3 yield syscall ok` when observed. The smoke loop then performs 4096
  CPL3 `getpid` syscalls before issuing a CPL3 `exit`, and the kernel logs
  `[test] ring3 preempt stress ok` when that loop completes to validate
  timer-preemption trampoline behavior under sustained user-mode syscall
  traffic. Ring3 smoke mode also spawns a second compiled native probe process
  (`ring3-native`) built from C using
  `lib/libc/include/wasmos/syscall_x86_64.h`; the syscall layer logs
  `[test] ring3 native abi ok` on first native CPL3 `getpid`. The
  `run-qemu-ring3-test` harness now also requires `native-call-smoke: ipc-call ok`
  and `[test] ring3 native abi ok` so both native IPC-call and native syscall
  header paths are asserted in the same run; it now also asserts the boot-time
  marker `[mode] strict-ring3=1`. Default smoke spawn remains
  disabled while this path is still being soak-tested. Ring3 smoke endpoint
  immediates are patched in a kernel-local code buffer before upload, and
  ring3 bootstrap copying
  now writes through the target context's user virtual mapping (temporary RW
  mapping then RX remap) instead of writing code directly via physical aliases.
- Native driver ELF PT_LOAD segment population now uses `mm_copy_to_user`
  (bytes + zero-fill) into the target context rather than direct dereference
  of mapped segment virtual addresses.
- Regression coverage now includes `tests/test_ring3_smoke_target.py`, which
  executes `cmake --build build --target run-qemu-ring3-test` to keep ring3
  marker assertions in the standard automated test suite, including structured
  user fault reason telemetry.
  Staged-default policy keeps ring3 smoke OFF in normal boot configs and ON
  in the dedicated ring3 smoke test target.
- Timer IRQ preemption now performs a ring3-safe trampoline rewrite for CPL3
  frames: return RIP is redirected to the scheduler preempt trampoline and CS
  is rewritten to kernel code selector so `iretq` re-enters ring0 cleanly
  before the scheduler context switch runs.
- Memory-region policy now carries an explicit user-access flag into paging
  mappings (including intermediate page-table entries), so user-accessible
  pages are tracked by intent rather than implicit convention. Paging now also
  rejects user mappings outside the designated user virtual-address slot and
  enforces user W^X (`WRITE+EXEC` denied); ring3 smoke/native code regions are
  now marked RX after bootstrap copy. Child address spaces now share a reduced
  higher-half kernel alias window (1 GiB) to tighten default kernel mapping
  exposure under user CR3 roots.
- A baseline user-pointer copy layer now exists in kernel memory management
  (`mm_copy_from_user` / `mm_copy_to_user`) with user-range permission checks,
  pre-mapping of touched pages, and temporary CR3 switch/restore around the
  actual copy. Copy helpers now use a fixed bounce buffer per chunk so
  kernel-side source/destination accesses happen under kernel CR3 while
  user-side memory dereferences happen only under target user CR3. Hostcall
  migration has started with `wasmos_framebuffer_info` using
  `mm_copy_to_user`; `wasmos_boot_config_copy` is now on a staged
  `mm_copy_to_user`-first path with a temporary
  host-pointer fallback during non-strict compatibility soak.
  Copy helpers now also emit trace-gated (`WASMOS_TRACE`) failure-stage
  diagnostics with op/stage/context/user range/expected-vs-current-root/chunk
  metadata for focused ring3 copy-path regression triage.
  A non-copy validator API
  (`mm_user_range_permitted`) is now available for phased hostcall guard
  rollouts. The framebuffer and shared-memory map hostcalls now use an
  explicit WASM-offset to user-VA resolver before remapping linear pages.
  Pointer-bearing hostcalls now broadly use a host-pointer bridge to resolve
  wasm3 host pointers back into user VA for explicit `mm_user_range_permitted`
  preflight (READ/WRITE by direction), including boot/module/process metadata,
  console I/O, ACPI info output, strlen input, block/fs buffer transfer paths,
  and early-log/boot-config copy paths; `wasmos_early_log_copy` now transfers
  data through `mm_copy_to_user` in bounded chunks instead of direct writes
  through wasm host pointers. Block-buffer copy/write hostcalls now
  also enforce non-overflowing in-range `phys+offset+len` arithmetic.
  A small set of early-boot output paths now use a compatibility dual-write
  helper (`mm_copy_to_user` plus host-pointer mirror) to preserve current
  non-strict behavior while keeping validated user-VA writes active during
  staged ring3 boundary hardening (`boot_config_copy`, `acpi_rsdp_info`,
  `boot_module_name`).
- Unrecoverable user-mode page faults now use process-local failure semantics:
  the kernel marks only the faulting process exited (`-11`) and continues
  scheduling remaining work; unhandled kernel-mode faults remain fatal.
  Ring3 smoke now includes a dedicated `ring3-fault` injector process and
  asserts `[test] ring3 fault isolate ok` when the fault is contained; it now
  also includes explicit write/exec fault injectors and assertions
  (`[test] ring3 fault write reason ok`,
  `[test] ring3 fault exec reason ok`).
  User-mode fault logs now include structured reason classification
  (`unmapped`, `write_violation`, `exec_violation`, `user_to_kernel`,
  `protection`) together with pid/error/address/rip for deterministic triage.
- Ring3 smoke now additionally verifies syscall ABI width validation for IPC:
  a CPL3 `IPC_NOTIFY` with a >32-bit endpoint is rejected by syscall argument
  parsing and emits `[test] ring3 ipc syscall arg width deny ok`.
- Ring3 fault policy is now asserted explicitly: a kernel-side watcher process
  verifies that `ring3-fault`, `ring3-fault-write`, and `ring3-fault-exec`
  each terminate with exit status `-11`, emitting dedicated
  `[test] ring3 fault* exit status ok` markers.
  Current `ring3-fault-exec` reason-marker acceptance includes
  `exec_violation`, `user_to_kernel`, and `unmapped` on existing QEMU/CPU
  paths until NX instruction-fetch faults classify consistently as
  `exec_violation`.
- Capability metadata now feeds a per-context resource-capability registry
  (`io.port`, `irq.route`, `mmio.map`, `dma.buffer`); WASM I/O hostcalls now
  enforce `io.port` when explicit capability policy is configured for the
  calling context, while unconfigured contexts remain in compatibility mode.
- A minimal fixed-size slab allocator scaffold (`kalloc_small`/`kfree_small`)
  is now available as an optional kernel allocator path for incremental
  migration off ad-hoc static/object-specific allocation patterns.
- The CMake-only `kernel_ide` aggregation target indexes kernel sources plus
  selected WASM user-space sources, so it must mirror the libc include root
  used by those components for editor diagnostics.
- The top-level documentation now uses repo-local mascot and wordmark assets in
  `README.md`; this is documentation-only branding and does not affect boot or
  runtime behavior.

## Architectural Direction

### Microkernel Split
Kernel mechanisms:
- Boot-time platform handoff.
- Physical and virtual memory management primitives.
- Preemptive scheduling and process lifecycle control.
- IPC transport, endpoint ownership, and wakeup rules.
- Interrupt handling and timer-driven preemption.
- WASM runtime hosting plus native-driver ELF loading via WASMOS-APP hooks.

User-space policy:
- Driver startup order and long-running driver logic.
- Filesystem semantics.
- Process startup policy.
- Hardware discovery policy above the raw ACPI data scan.
- Future service registry, supervision, and namespace management.

### Privilege Model
- Today all processes still execute in ring 0 with per-process kernel data
  structures and separate runtime contexts.
- The architecture now includes a minimal syscall entry primitive (`int 0x80`)
  plus ring3-capable context-restore primitives (`iretq` path + per-process
  `rsp0` setup) and user-page mapping flag plumbing, but full user-mode
  process rollout and user-space pager ownership are still pending.
- Drivers are treated as privileged by policy.
- Services are intended to become least-privileged first once ring 3 support,
  page-table separation, and syscall entry are in place.
- Applications already carry the weakest semantic role in the container format,
  even if CPU privilege separation is not implemented yet.

## Boot Contract

### Bootloader Responsibilities
Intent: small, deterministic, and policy-light.

The bootloader must:
- Locate and read `kernel.elf` from the EFI System Partition.
- Locate and read `initfs.img` from the EFI System Partition.
- Validate the ELF header and load all `PT_LOAD` segments, including
  misaligned physical addresses and overlapping segment reuse.
- Collect the UEFI memory map and copy it into kernel-owned pages before
  `ExitBootServices()`.
- Fill a versioned `boot_info_t`.
- Validate the initfs header and entry table, then copy the initfs blob into
  boot handoff memory.
- Synthesize `boot_module_t` records for bootstrap-marked initfs WASMOS apps so
  the existing early-kernel bootstrap path can stay unchanged.
- Transfer control to the kernel entry point without embedding higher-level OS
  policy.

`BOOTX64.EFI` now also captures the GOP framebuffer when available. When the
GOP handles are absent, it scans VGA PCI BARs for a framebuffer, logs the
cartographic results, and pipes the discovered base, size, resolution, stride,
and `BOOT_INFO_FLAG_GOP_PRESENT` into `boot_info_t` so lower-privilege drivers
can map the framebuffer themselves.

### Kernel Entry Responsibilities
The architecture-specific entry path must:
- Establish a known stack.
- Clear `.bss`.
- Preserve the incoming `boot_info_t *` passed in `RCX` under the Microsoft x64
  UEFI calling convention.
- Call `kmain(boot_info_t *)`.

### `boot_info_t` Rules
`boot_info_t` is append-only and versioned. New fields go at the end.

Current required fields:
- `version`, `size`, `flags`
- `memory_map`, `memory_map_size`, `memory_desc_size`, `memory_desc_version`
- `modules`, `module_count`, `module_entry_size`
- `rsdp`, `rsdp_length`
- `initfs`, `initfs_size`
- `boot_config`, `boot_config_size`

Current behavior:
- The bootloader fills the structure and the kernel validates the size/version
  before using it.
- Boot modules are still used as the early bootstrap channel, but they are now
  derived from the initfs instead of being loaded one-by-one in the bootloader.

### Initfs Layout
The bootstrap image format is intentionally small and append-only enough for
boot handoff use:
- header with magic `WMINITFS`, version, table sizing, and total image size
- fixed-size entries with type, flags, payload offset/size, and logical path
- raw payload data packed directly behind the table

Current entry types:
- WASMOS app payload
- config payload
- generic data payload

Current bootstrap use:
- bootstrap-marked WASMOS app entries are exposed as `boot_module_t` records
- the first config entry is exposed through `boot_info_t.boot_config`
- the full blob is retained so later code can add a proper initfs reader

## Boot Flow

### High-Level Sequence
1. UEFI loads `BOOTX64.EFI`.
2. The bootloader loads `kernel.elf` and `initfs.img`.
3. The bootloader exits boot services and jumps to kernel entry.
4. The kernel initializes core subsystems and spawns the kernel `init` task.
5. `init` starts `hw-discovery` from the bootstrap module set exposed by initfs.
6. `hw-discovery` starts `ata` and `fs-fat`.
7. `init` waits for FAT readiness, then loads `sysinit` from disk through the
   process manager.
8. `sysinit` reads the boot config and starts the configured late user
   processes.
9. The CLI becomes the visible interactive shell.

A minimal COM1-based serial stub keeps the console alive during the steps above.
The AssemblyScript `serial` driver now loads via `hw-discovery` and invokes
`serial_register()` so console output can switch over from the stub to the new
service as soon as the driver is available.

- `hw-discovery` merely starts the keyboard WASMOS app alongside the other
  bootstrap drivers; the AssemblyScript driver now polls the PS/2 controller for
  scancodes itself so keyboard presence remains a user-space concern instead of
  spinning kernel knowledge into the microkernel core.

### Practical Boot Ownership
- Bootloader owns UEFI interaction and boot-time file I/O.
- Kernel owns core mechanisms and early bootstrap orchestration.
- `init` owns system bootstrap sequencing once the kernel is alive.
- `sysinit` owns late user process startup policy from boot config only.

This split is intentional: it keeps bootloader policy minimal and prevents
`sysinit` from becoming a second bootstrap coordinator.

## Scheduling and Preemption

### Current Design
The scheduler is fully preemptive round-robin:
- PIT IRQ0 drives time-slice accounting.
- The default PIT rate is 250 Hz.
- The ready queue is FIFO.
- Each runnable process gets a fixed quantum
  (`PROCESS_DEFAULT_SLICE_TICKS` in `src/kernel/include/process.h`).
- An explicit idle task runs `hlt` whenever no process is ready.

### Implemented Preemption Path
The preemptive scheduling design that previously lived in a separate draft is
now the baseline architecture:
- `src/kernel/timer.c` programs PIT channel 0 and tracks global ticks.
- The IRQ0 handler increments tick accounting and triggers preemption logic.
- The kernel does not perform a full scheduler run inside the ISR.
  Instead, timer preemption rewrites the interrupted RIP to
  `process_preempt_trampoline`, which returns into the normal scheduler path.
- Per-process state includes saved register context, total tick accounting, and
  remaining time slice.
- Context switching is implemented in
  `src/kernel/arch/x86_64/context_switch.S`.
- Spinlocks disable preemption while held to keep critical regions short and
  consistent.

### Preemption Safety Rules
- Never preempt while a spinlock is held.
- Never perform heavy scheduling work directly in the interrupt handler.
- IPC queue mutations must remain atomic under preemption.
- Long host calls must mark themselves as non-preemptible if interrupting them
  would break wakeup or ownership invariants.

### Current Safe Points and Special Cases
- IPC receive host calls mark the current process as inside a host call so the
  empty-to-block transition cannot race against wakeups.
- If a blocked process is woken during that transition, the scheduler preserves
  the wakeup instead of forcing the process back to `BLOCKED`.
- The CLI calls `sched_yield` while polling for user input so other processes
  continue to make progress even when the shell is idle.

### What Is Still Missing
The preemptive core is implemented, but the following are still future work:
- Priorities or budgets.
- Per-CPU scheduling.
- User-mode context switching with kernel/user stack separation.
- Richer scheduling metrics and latency instrumentation.

## Process Model

### Process Lifecycle
The implemented process states are:
- `READY`
- `RUNNING`
- `BLOCKED`
- `ZOMBIE`

Typical transitions:
- Spawn: `READY`
- Dispatch: `READY -> RUNNING`
- Time slice expiration: `RUNNING -> READY`
- IPC wait or explicit block: `RUNNING -> BLOCKED`
- Wakeup: `BLOCKED -> READY`
- Exit: `RUNNING -> ZOMBIE`
- Reap: `ZOMBIE -> UNUSED`

### Process Ownership
- The kernel-owned `init` task is the root parent for kernel-spawned processes.
- The process manager owns the `proc` IPC endpoint and mediates spawn/wait/kill/status.
- PM-created processes get their own runtime context and stack/heap sizing from
  WASMOS-APP metadata.

### Runtime Contexts
Each process is associated with a runtime context that tracks:
- linear memory
- stack
- heap
- IPC region placeholders
- device region placeholders

This is the structural precursor to full address-space separation.

## IPC Model

### Core Message Format
All IPC messages share the same fixed register-sized layout:

```c
type
source
destination
request_id
arg0
arg1
arg2
arg3
```

Small control traffic stays in-message. Bulk payloads are expected to move to
shared buffers plus synchronization messages.

### Implemented Rules
- Endpoints have an owning context.
- `ipc_send_from` requires a non-kernel sender to own its source endpoint.
- `ipc_recv_for` requires a non-kernel receiver to own the destination endpoint.
- Enqueueing a message can wake a process blocked on the destination endpoint.
- Message queues are bounded and protected by spinlocks.
- Endpoint table capacity is currently 128 and endpoints owned by a process
  context are released when that process is reaped, preventing table exhaustion
  across repeated short-lived app runs.

### Error Model
Current IPC status codes:
- `IPC_OK`
- `IPC_EMPTY`
- `IPC_ERR_INVALID`
- `IPC_ERR_PERM`
- `IPC_ERR_FULL`

### Direction of Future Growth
The current transport is intentionally small. The architecture still needs:
- notification objects distinct from synchronous request/reply IPC
- shared-memory bulk transfer paths
- service-level allowlists / badges
- async server helpers for multi-hop service stacks
- richer endpoint naming / registry rules

## Interrupts and Timer Integration
- The kernel remaps the legacy PIC and installs exception plus IRQ stubs.
- PIT IRQ0 is the active scheduler clock.
- The timer code emits a one-time visible initialization message
  (`[timer] pit init`).
- Periodic timer tick progress markers are now trace-only and hidden when
  `WASMOS_TRACE=OFF`.

The current interrupt model is still PIC-based. APIC/IOAPIC support remains open.

## Memory Management

### Current State
Implemented:
- physical frame allocator from the UEFI memory map
- freeing of physical pages
- kernel-owned x86_64 page tables
- higher-half kernel alias mapping at `0xFFFFFFFF80000000`
- root context creation
- per-process context creation
- per-process root page tables cloned from the kernel mappings
- CR3 switching on scheduler dispatch/return
- fault-driven mapping of process-owned virtual regions into a private user slot
- guard pages around process stacks
- stack canaries for overflow diagnostics

### Current Constraints
- Shared-memory primitives are still mostly architectural intent.
- Page faults are handled through a kernel-hosted memory service scaffold rather
  than a real user-space pager.
- All tasks still execute in ring 0, so address-space separation is not yet a
  security boundary.
- Process runtime stacks still rely on shared low kernel mappings rather than a
  dedicated kernel-stack virtual range per process.

The native-driver loader maps requested physical device memory (for example, the
GOP framebuffer) into the driver process context at a fixed device virtual base
for direct native access after validation.

### Direction
The desired endpoint is:
- shared kernel higher-half mappings
- per-process user mappings with ring 3 execution
- explicit shared regions for bulk IPC
- user-space memory policy
- user-mode page-fault handling

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

### Language ABI Strategy
Applications no longer need to implement the raw startup ABI directly:
- the C shim exports `wasmos_main` and calls `main(int argc, char **argv)`
- the Rust shim exports `wasmos_main` and calls `main(args: &[&str])`
- the Go shim exports `wasmos_main` and calls `Main(args []string) int32`
- the AssemblyScript toolchain-owned root module exports `wasmos_main` and
  delegates to `main(args: Array<string>): i32`
- the Zig shim exports `wasmos_main` and keeps a Zig-native `main`

This keeps the external ABI stable while presenting language-native entrypoints.

## Drivers and Services

### Implemented Drivers
- `ata`
  - PIO ATA block driver
  - owns the `block` endpoint
  - supports identify and read operations
- `fs-fat`
  - FAT12/16/32 filesystem driver
  - consumes the `block` endpoint
  - owns the `fs` endpoint
  - supports root/subdirectory listing, `cat`, `cd`, PM app loading, and the
    minimal shared libc read-only file API
  - follows FAT12/16 cluster chains for multi-cluster file reads on the
    current ESP baseline
- `chardev`
  - IPC-backed console/character device service
- `framebuffer`
  - optional native C driver packed as `FLAG_DRIVER|FLAG_NATIVE`
  - probes the kernel framebuffer APIs exposed via GOP
  - validates geometry and maps framebuffer pages into a fixed driver device
    virtual region through the native-driver API
  - paints a gradient on the standard QEMU VGA framebuffer when the device is present

### Implemented Services
- `process-manager`
  - validates WASMOS-APP containers
  - creates process/runtime state
  - resolves required endpoints
  - starts entries
  - exposes `spawn`, `spawn by name`, `wait`, `kill`, and `status`
- `hw-discovery`
  - scans ACPI RSDP data
  - starts the early storage driver chain
  - starts post-FAT display/input drivers by name from disk
- `sysinit`
  - intentionally narrow
  - starts post-FAT services and late user processes from the generated boot
    config
- `cli`
  - interactive shell over `proc` and `fs`

### Driver and Service Startup Chain
Current startup chain:
1. bootloader loads `initfs.img`
2. initfs contributes bootstrap `boot_module_t` entries for `hw-discovery`,
   `ata`, `fs-fat`, and the current smoke/bootstrap apps
3. kernel `init` spawns `hw-discovery`
4. `hw-discovery` starts the storage chain: `ata` and `fs-fat`
5. `hw-discovery` starts post-FAT hardware drivers by name: `serial`,
   `keyboard`, and `framebuffer`
6. kernel `init` waits for a successful FAT readiness probe
7. kernel `init` loads `sysinit` from disk via PM
8. `sysinit` loads the configured `sysinit.spawn` services/processes from disk,
   including `vt` and `cli`

This is the current stable bootstrap baseline.

### Boot Config
The initial config channel is a simple binary blob generated from TOML at build
time. The current generator reads `scripts/initfs.toml` and emits both the
initfs image and a compact `bootcfg.bin` payload.

Current config format:
- magic `WCFG0001`
- version
- bootstrap-module count
- sysinit-spawn count
- string-table size
- offset arrays for each string list
- NUL-terminated ASCII string table

Current `sysinit.spawn` validation:
- at least one late-start process must be configured
- process names must be unique
- process names must fit the current 16-byte PM by-name spawn ABI

Current use:
- the blob is carried in initfs for stable packaging
- the bootloader exposes the blob through `boot_info_t`
- wasm processes can read it through `wasmos_boot_config_size()` and
  `wasmos_boot_config_copy()`
- `sysinit` validates and consumes the `sysinit.spawn` string list for its
  late-start process policy and halts that policy path if the config is
  malformed

### What Is Still Missing
- driver-manager
- full PCI enumeration
- richer ACPI-based device inventory publication
- hotplug handling
- capability-based MMIO/PIO/DMA/IRQ grants

## CLI and User-Space Baseline
The CLI intentionally stays small and testable.

Supported commands:
- `help`
- `ps`
- `ls`
- `cat <path>`
- `cd <path>`
- `exec <app>`
- `halt`
- `reboot`

The CLI is also part of the scheduler regression story because it yields while
idle instead of monopolizing CPU time in a polling loop.

## Diagnostics and Tracing

### Visible by Default
These remain visible even with tracing disabled:
- normal boot logs
- fatal diagnostics
- runtime failure diagnostics
- process listings and CLI-visible service output

### Visible Only With `WASMOS_TRACE=ON`
- init / PM / scheduler trace lines
- `debug_mark(tag)`
- periodic timer progress markers (`[timer] ticks`)
- verbose scheduling and runtime transition traces

### Existing Debug Hooks
- `g_skip_wasm_boot` can isolate runtime bring-up in the kernel init path
- GP fault reporting includes PID, name, stack bounds, RIP, CS, and RFLAGS
- CLI tests and smoke apps provide functional regression coverage for IPC,
  preemption, filesystem access, and language shims

## Current Status by Area

### Done
- boot contract versioning and validation
- ELF loading with aligned/overlap-safe segment handling
- single-image initfs bootstrap packaging
- serial-first early boot diagnostics
- physical memory allocator
- basic paging scaffold
- process contexts and stacks from physical memory
- preemptive round-robin scheduler
- timer-driven context switching
- IPC endpoint ownership enforcement
- process manager and WASMOS-APP loader
- bootstrap split between kernel `init` and user-space `sysinit`
- FAT-backed loading of `sysinit`, `cli`, and normal applications
- generated boot-config blob exposed to user space
- shared read-only file API in userland libc
- language-native application entrypoint shims

### Partially Done
- filesystem support
  - works for current boot and small-file scenarios
  - still limited in read breadth and filesystem operations
- hardware discovery
  - enough for ACPI RSDP discovery and storage bootstrap
  - not yet a general device manager
- memory service
  - kernel-hosted scaffold exists
  - real user-space pager does not

### Not Done
- ring 3 execution
- syscall ABI
- APIC / SMP
- shared-memory IPC fast paths
- service registry
- supervision / restart policy
- capability-granted device access
- broader config-driven startup policy beyond the current `sysinit.spawn` list

Open implementation work is tracked in `TASKS.md`.

## Repository Map
- `src/boot/`: UEFI loader
- `src/kernel/`: kernel core, runtime hosting, scheduler, IPC, memory
- `src/drivers/`: WASM and native drivers
- `src/services/`: WASM services
- `lib/libc/`: shared user-space libc surface and language shims
- `examples/`: application examples and smoke apps
- `tests/`: QEMU-driven integration and regression tests

## Validation Baseline
Every architecture-affecting change is expected to keep these green:
- `cmake --build build --target run-qemu-test`
- `cmake --build build --target run-qemu-cli-test`
- `cmake --build build --target strict-ring3`

QEMU backend caveat:
- the CLI write smoke keeps truncate/append/create plus nested unlink/rmdir
  checks, but avoids one top-level grown-file unlink sequence that can trigger
  a known `vvfat` host assertion on some QEMU builds

The architecture is only considered stable when non-interactive boot, CLI
integration, and strict-ring3 gate checks all pass.
