# Current Status

- This status file is a snapshot, not a release changelog.
- Current SMP crash instrumentation now also watches the live main
  `thread->ctx` for `process-manager` / `native-call-min` instead of the stale
  mirrored `proc->ctx`, and AP bring-up no longer clears the global watch
  state after the BSP arms it. The shared ready queue now additionally halts
  immediately if any caller tries to enqueue a `THREAD_STATE_RUNNING` thread,
  tightening the current race hunt around duplicate-run versus post-save
  context stomp failures.
- Scheduler/context-switch hardening now validates the live `thread->ctx`
  record at dispatch and user-preempt save points instead of relying on the
  legacy `proc->ctx` mirror, so corrupt thread RIP/RSP state trips
  immediately with the owning PID/TID in the panic path. The x86_64
  `context_switch.S` restore path also no longer reuses restored `%rdi`,
  `%rsi`, `%r8`, or `%r10` as scratch registers while staging `ret`/`iretq`,
  which previously clobbered resumed thread register state during both kernel
  and ring-3 resumes. The shared ready queue now also rejects duplicate
  `thread_t` inserts under its lock, closing an SMP wake/block race where a
  thread could be concurrently re-enqueued twice during blocked-yield
  completion and remote wake-up. Late remote wakes now also refuse to
  re-enqueue threads that have already returned to `RUNNING`, so stale event
  delivery cannot place a live thread back on the ready list from another CPU.
  The wake path now uses the thread-table's atomic `BLOCKED -> READY`
  transition helper instead of raw unlocked state stores, removing another SMP
  window where concurrent CPUs could race on the same thread state. wasm3
  runtime entry is now also serialized globally across CPUs, so different
  processes/drivers can no longer race through shared wasm3 internals while
  merely relying on per-process runtime ownership. Nonblocking IPC/notify
  polls no longer register scheduler-event waiters on `IPC_EMPTY`, which
  removes an SMP bug where a sender could "wake" a thread that never blocked
  and requeue a still-running thread on another CPU.
- User-space threading helpers now include a process-local reentrant mutex
  surface across WASM libc/libsys, native ring3 libc, and the native
  driver/service ABI. The mutex state lives in user memory (`owner_tid` +
  `recursion_depth`), while the kernel serializes `try_lock` / `unlock`
  transitions so the runtime does not depend on unsupported WASM atomic
  instructions. Current contention handling is cooperative (`thread_yield` /
  `sched_yield` retry) rather than a futex-style sleep queue.
- SMP infrastructure (Phases 0–9) is complete. `WASMOS_SMP` Kconfig bool
  (depends on `WASMOS_IRQ_IOAPIC`, default off) gates all multi-core code.
  Per-CPU data structure (`cpu_local_t`, `g_cpus[16]`) and `cpu_local()`
  accessor are live; the BSP fully uses `g_cpus[0]` for its GDT, TSS, and
  scheduler state. GS base MSR is set at the end of `x86_cpu_init()`.
  `process.c` scheduler globals (`current_process`, `current_thread`,
  `preempt_disable_count`, `in_scheduler`) now live in `cpu_local_t`.
  Spinlock per-CPU IRQ-disable state (`irq_disable_depth`, `irq_saved_flags`)
  moved from file-static globals to `cpu_local_t`; ready-queue spinlock added.
  MADT type-0 (Processor Local APIC) CPU discovery in `ioapic.c` populates
  `g_cpus[1..N-1]` with AP APIC IDs when `WASMOS_SMP=1`. LAPIC ICR helpers
  (`lapic_read_id`, `lapic_send_init_ipi`, `lapic_send_sipi`, `lapic_ap_enable`)
  added to `lapic.c`. AP trampoline in `smp_trampoline.S` (physical 0x1000)
  transitions 16-bit real → 32-bit PM → 64-bit LM and calls `smp_ap_c_entry`.
  `smp_cpus_up()` performs INIT-SIPI-SIPI per AP, waits on `cpu->started`;
  the trampoline code page is identity-mapped executable while the low data
  slot page remains NX so AP startup can fetch from `0x1000` without opening
  execute permission on the `0x0000` slot page. The fixed trampoline page at
  physical `0x1000` is also reserved from the general page-frame allocator so
  shared-memory/kernel allocations cannot be clobbered during AP bring-up.
  Service/driver children that require `notify_ready` are now also marked
  ready-gated before they enter the run queue so SMP cannot let them auto-mark
  ready via an early IPC block before process-manager arms the sync wait.
  `smp_ap_c_entry()` loads per-CPU GDT/TSS/IDT/GS, enables AP LAPIC timer,
  sets `started=1`. AP CPU init now also normalizes CR0/CR4 SIMD/FPU state
  (including `OSFXSR` / `OSXMMEXCPT`, `fninit`, and default `MXCSR`) before
  scheduled C code runs on that CPU. The global ready queue now uses a single
  shared spinlock instead of per-CPU queue locks, and array-chunk list
  elements are returned 8-byte aligned so embedded atomic fields such as IPC
  endpoint spinlocks are SMP-safe. No behavioral change at `WASMOS_SMP=0`.
  Full design in
  `docs/architecture/28-smp.md`.
- SMP runtime hardening now also covers several real shared-state failures
  exposed after APs joined the scheduler loop. The current-thread identity is
  fully per-CPU (`thread_current_tid()` now reads `cpu_local()->current_thread`
  instead of a shared global), so IPC waiter registration, wake targeting,
  syscall self-thread queries, and WASM hostcalls no longer race across CPUs.
  Nonblocking IPC pollers now use a separate `ipc_try_recv_for()` path that
  does not arm `waiter_tid` on `IPC_EMPTY`, preventing senders from waking a
  still-running poller thread and re-enqueueing it in `THREAD_STATE_RUNNING`.
  Blocking kernel-side receive loops still share `ipc_recv_blocking_for()`,
  which retries after `process_block_on_ipc()` and now leaves externally woken
  `READY` state intact instead of forcing `RUNNING` back into the thread slot.
  The scheduler’s low-stack
  trampoline path now also uses a per-CPU scheduler stack instead of a shared
  global buffer, removing cross-CPU corruption of saved scheduler frames during
  concurrent low-stack entries. The equivalent WASM hostcall path
  (`wasmos_ipc_recv`) still restores the current thread state on the same race.
- Shared kernel allocators and registries now also serialize their mutable SMP
  state: the physical page-frame allocator protects its free-range and
  refcount arrays with a spinlock; process/thread slot allocation and PID/TID
  assignment are serialized; MM context/shared-region registries are serialized;
  and the early list fallback arena uses an atomic bump offset so bootstrap
  allocations cannot overlap across CPUs. `ipc_recv_blocking_for()` now only
  restores `RUNNING` state when the caller is still locally blocked, avoiding a
  READY-to-RUNNING stomp after a remote wake-up. One known scheduler gap
  remains: `g_in_context_switch` and the related context-switch diagnostics are
  still global until the assembly path is converted to per-CPU storage.
- SMP late-boot storage/runtime fixes now also include correct higher-half
  aliasing for wasm block-buffer copy/write hostcalls, so ATA/FAT service
  traffic no longer dereferences raw physical addresses once SMP bring-up
  reaches filesystem activity. With the current SMP fixes in place, the default
  `run-qemu-test` boot now reaches framebuffer, input, font, compositor, and
  CLI startup and lands at the interactive prompt instead of faulting during
  `device-manager`/storage bring-up.
- SMP native-driver and sync-spawn hardening now fixes two additional races
  exposed on 4-CPU boots. First, `native_driver_start` now switches CR3 to the
  driver's own page table (`ctx->root_table`) before calling the ELF entry
  function and restores the kernel CR3 on return; previously the entry ran with
  the kernel's PML4, executing wrong physical pages because native driver ELFs
  are linked at `IMAGE_BASE=0x10000000` (covered by the kernel's bootstrap
  identity mapping, not the driver's own second-level tables). Second,
  `process_manager_on_child_ready` no longer reads or writes `g_pm.spawn`
  fields; that function executes on the native driver's CPU (not PM's), so any
  `g_pm.spawn` access was an unsynchronised cross-CPU read/write. The race:
  PM writes `in_use=1` and `sync_child_pid` in sequence; if the driver CPU
  observed `in_use=1` before `sync_child_pid` was visible, the pid-match check
  failed, `in_use` was cleared to 0 without sending a reply, and PM's
  `pm_poll_sync_spawn` (which checks `in_use` before entering its polling loop)
  skipped every subsequent iteration — leaving `device-manager` hung at the
  `proc_notify_ready` boundary (visible as a boot stall after "acpi-bus scan
  complete"). Fix: `process_manager_on_child_ready` now only calls
  `process_notify_ready(proc)` (sets `proc->ready=1`); `pm_poll_sync_spawn`
  already checks `child->ready` on every PM iteration and sends the
  `PROC_IPC_RESP` safely from PM's own single-threaded context. Additionally,
  the WASM driver registry now uses an explicit spinlock instead of
  `critical_section_enter/leave`, and `thread_wake_if_blocked` atomically
  transitions `BLOCKED→READY` under the thread-table lock to prevent double
  enqueue from concurrent CPU wakeups. The `blocking_transition` field uses
  acquire/release atomics to guard the `RUNNING→BLOCKED` window. After these
  fixes, 4-CPU SMP boots reliably reach the WAMOS interactive CLI across
  repeated runs.
- Interrupt controller selection is now a build-time Kconfig choice
  (`WASMOS_IRQ_PIC` / `WASMOS_IRQ_LAPIC` / `WASMOS_IRQ_IOAPIC`, mapped to
  `WASMOS_IRQ_MODE` 0/1/2). PIC + PIT remains the default. LAPIC mode replaces
  the 8259 with the Local APIC timer (calibrated via PIT channel 2). IOAPIC mode
  adds full ISA IRQ routing through the I/O APIC (MADT-discovered, all 16 RTEs
  programmed via `irq_late_init()`).  All three modes boot to the CLI halt point.
  See `docs/architecture/05-x86-cpu-architecture.md`.
- Ring-3 strict isolation/hardening, threading phase rollout, DMA rollout,
  filesystem/PM service discovery, and CLI/runtime updates are tracked in the
  dedicated docs under `docs/architecture/`.
- CLI `ps` process diagnostics now also expose runtime kind (`wasm` true/false
  in table view and `wasm=true|false` annotations in tree view), sourced from
  process-manager spawn metadata.
- Threading is production-complete for the current single-core scope; final
  ABI/policy decisions and closure status are in
  `docs/architecture/08-threading-and-lifecycle.md` sections 15 and 17.
- Recent threading runtime hardening (user-thread kernel-stack setup for
  `THREAD_CREATE` and syscall frame/context synchronization for yield/block
  paths) is documented in `docs/architecture/08-threading-and-lifecycle.md`.
- Graphics/compositor Phase 0 scaffold (shared ABI constants and minimal
  native Zig `gfx-compositor` endpoint handshake path) is tracked in
  `docs/architecture/20-graphics-framebuffer-and-compositor.md`.
- Graphics/compositor baseline now also includes typed compositor opcode
  dispatch and minimal window lifecycle handling (`CREATE_WINDOW` /
  `DESTROY_WINDOW`) with owner-checked state slots and `GFX_STATUS_*` replies.
- Graphics/compositor baseline now also includes `RESIZE_WINDOW`,
  `ALLOC_SHARED_BUFFER` (opaque random 32-bit `buffer_id` + shmem backing),
  `RELEASE_SHARED_BUFFER`, and `PRESENT_WINDOW` by `buffer_id` with
  shmem-backed damage rect lists.
- Graphics/compositor lifecycle hardening now enforces window-generation-aware
  buffer validity (pre-resize stale buffers denied on present), explicit
  in-use buffer release denial, and deterministic owner/binding validation for
  present/release transitions.
- Graphics/compositor input routing now subscribes to keyboard driver
  notifications and exposes focused-window events through `GFX_IPC_POLL_EVENT`
  (`FOCUS_GAINED`, `FOCUS_LOST`, `KEY`).
- Graphics/compositor input routing now also subscribes to mouse-driver move
  notifications, emits focused pointer events (`POINTER`) through
  `GFX_IPC_POLL_EVENT`, and applies click-to-focus + raise-on-click policy for
  topmost hit-tested windows.
- Graphics/compositor now also performs runtime input subscription recovery
  (for late-started `kbd`/`mouse` services) and idle orphan-state cleanup so
  dead client endpoints cannot leave stale windows/buffers/events or persistent
  overlay mode.
- Graphics/compositor now also renders a software cursor overlay above window
  composition and repaints old/new cursor rectangles on movement, making
  pointer position/focus interactions directly visible during bring-up.
- Graphics/compositor now also renders minimal window chrome (title/border with
  close + maximize/restore hit targets), emits `GFX_EVENT_CLOSE_REQUEST` when
  close is clicked, and toggles maximized geometry with a second top-right
  button.
- Graphics/compositor pointer interaction now also includes title-bar
  drag-to-move behavior (close zone excluded), with window coordinates clamped
  to framebuffer extents.
- Graphics/compositor pointer interaction now also includes bottom-right
  corner live-resize behavior with dimension clamping to framebuffer extents
  and window min/max policy limits.
- Graphics/compositor now emits resize notifications (`GFX_EVENT_RESIZE`) to
  window owners during pointer-driven resize and maximize/restore toggles;
  current smoke validation reallocates and re-presents buffers on that event.
- WASM-side `libui` scaffold now exists as shared headers (`wasmos/libui.h` in
  libc + libsys mirrors), providing a small struct-based component tree
  (`Panel`/`Label`/`Button`/`Checkbox`/`TextInput`/`ScrollView`/`ListView`),
  lightweight bitmap text rendering, pointer focus + key-input editing for text
  inputs, clipped viewport rendering and drag scrolling for scroll/list views,
  and app-owned IPC pass-through
  (`ui_loop_handle_ipc`), and dirty-frame flush (`ui_loop_drain`).
- Graphics/compositor design phase for text now targets a dedicated
  `font-service` (glyph rasterization + metrics + shared atlas IPC) instead of
  a fixed built-in compositor font path; current bring-up scope is TTF-only.
- Native Zig `font-service` baseline now builds and is packaged as
  `/boot/system/services/fontsvc.wap`, with `font` endpoint registration,
  TTF file load from `/boot/system/fonts/*.ttf`, and owner-checked
  `OPEN_FONT` + `GET_METRICS` IPC; glyph raster IPC is still TODO.
- Input-driver baseline now also includes a wasm `mouse` driver with
  subscription IPC (`MOUSE_IPC_SUBSCRIBE_REQ` + `MOUSE_IPC_MOVE_NOTIFY`) that
  emits PS/2 packet-derived movement deltas and button masks to subscribers.
- Graphics validation now also includes a wasm `gfx-smoke` app available under
  `/boot/apps/gfx_smoke.wap` for manual CLI execution, keeping compositor tests
  opt-in at runtime instead of sysinit auto-spawn; the smoke scenario now
  covers two concurrent windows and close-event teardown of each window, plus
  a `libui` component demo window.
- Native-driver ABI now includes endpoint-owner lookup and shmem grant
  callbacks so native services (including `gfx-compositor`) can share
  compositor-owned shmem buffers with requesting wasm clients.
- WASM hostcall surface now also includes kernel-managed shared-memory
  auto-mapping (`shmem_map_auto`) that returns a process-local linear-memory
  offset from a managed tail window, removing hardcoded map offsets in clients
  such as `gfx-smoke`.
- Software composition now redraws clipped dirty regions in stable z-order,
  including overlap with higher-z windows; invalid/missing damage falls back to
  full-frame redraw.
- Process-manager runtime bookkeeping now grows on demand (`apps`, `waits`,
  and `services` use internal linked-list pools), removing fixed small slot
  caps from PM-managed state.
- Memory-management design now includes a phased migration plan to remove
  duplicated hardcoded physical-window limits, introduce intent-based
  allocation policy, and decouple kernel-internal allocations from DMA-style
  low-address constraints.
- Kernel dynamic container baseline now includes a centralized `list`
  interface with selectable backends (linked vs growable array-chunk);
  process-manager list backend selection is wired through Kconfig.
- Threadable-scheduler wakeup handling now avoids single-CPU deadlock when an
  IPC/event sender wakes a thread during the narrow RUNNING-to-BLOCKED
  transition after event registration but before the blocked yield path has
  finished; scheduler selftests now cover that race.
- WASM event-loop intent registration now arms the request-id slot before
  sending IPC, preventing fast replies from being consumed as unhandled
  messages during early process-manager metadata and sync-call traffic.
- Process-manager module-metadata path lookup now accepts caller FS-buffer
  payloads (the shared libc contract) in addition to direct user pointers, and
  device-manager uses that buffer path for initfs module discovery during
  early bring-up.
- Device-manager sync IPC waits now use a dedicated select set over both the
  PM reply endpoint and `devmgr.query`, so bring-up stays responsive to
  filesystem metadata queries while waiting on process-manager replies.
- Device-manager query/inventory/reply housekeeping paths now drain endpoints
  explicitly in nonblocking mode; this avoids a threadable-scheduler bring-up
  stall where the new select-backed WASM event-loop poll blocked on an empty
  `devmgr.query` wait before the next pending spawn target (for example
  `acpi-bus`) could run.
- Higher-level components may use C++, while low-level kernel boundaries stay
  C/ASM. WASM C++ build policy is no exceptions/RTTI and explicit C ABI at
  integration points.
- Process-manager test injection hooks are now behind a dedicated Kconfig/CMake
  switch (`WASMOS_PM_TEST_HOOKS`) and are no-op when disabled.
- `fs-manager` no longer relies on a fixed-size client slot table; client
  state now grows in heap-backed chunks.
- Process-manager context buffer tracking for filesystem/framebuffer borrows is
  list-backed instead of fixed `PROCESS_MAX_COUNT` arrays.
- Kernel list internals include an early-boot static-arena allocator fallback
  so list-backed modules can initialize before general heap allocation is ready.
- MM context registration and capability state tracking are list-backed, so
  context growth is no longer bounded by static `MM_MAX_CONTEXTS` slot arrays.
- Per-context memory-region storage is also list-backed, removing fixed
  `MM_MAX_REGIONS` limits within each context.
- WASM `libui` component-tree state is heap-backed (dynamic component, text,
  and list-item storage) instead of fixed compile-time caps, and includes list
  views/dropdowns with text rendering through required `font-service` IPC.
- Font IPC includes text-run measurement and client-buffer rasterization
  (`FONT_IPC_MEASURE_GLYPH_REQ`, `FONT_IPC_RASTER_GLYPH_INTO_REQ`) with shared
  memory payload/response packing.
- Compositor window-title rendering uses the text-run path (measure +
  raster-into) with per-window title-run caching to avoid compose-time per-char
  font IPC requests.
- Compositor pointer delivery to focused clients uses content-local
  coordinates, and client buffers render in the content pane below window
  chrome/titlebar.
- WASM libc now implements a process-local linear-memory allocator
  (`malloc/free/calloc/realloc`) backed by `memory.grow`.
- Native driver ABI includes an explicit shared-memory flush hook for stable
  shared-buffer publication contracts in native services/drivers.
- WASM hostcalls include both shared-memory sync directions:
  `shmem_flush` (WASM -> shared) and `shmem_refresh` (shared -> WASM).
- ACPI class/subclass matching in `device-manager` includes RTC bring-up
  (`PNP0B00` class `0x08` / subclass `0x03`) alongside serial/keyboard/mouse
  ISA devices from `acpi-bus`.
- The wasm `serial` driver now binds `proc.endpoint` like the other ACPI
  input/ISA drivers, so its `PROC_IPC_NOTIFY_READY` message reaches
  process-manager during sync spawn instead of looping respawns after the boot
  ACPI rules load. The missing binding was a latent bug masked before SMP by
  the old non-deterministic auto-ready path for service/driver children.
- `sysinit` now gives `start` commands a longer sync-spawn timeout so heavier
  native services such as `font-service` can finish boot-time warmup and send
  their explicit ready signal before `sysinit` aborts the script. Under SMP,
  the old 5-second timeout was too short even though the child continued
  initializing and later reported ready correctly.
- RTC IPC message IDs and payload packing are explicitly defined in shared
  kernel/user headers (`rtc_ipc.h`) for a single client/driver contract.
- CLI builtin `echo` and script `echo` share one parser/expander path in libc
  script helpers, including `-n`/`-e`/`-E`/`--`, quoting, and `${VAR}`
  expansion.
- Environment-variable architecture now targets per-context scope ownership
  with POSIX-like snapshot inheritance, including explicit `script` (child
  scope) versus `source` (current scope) behavior.
- A generic `virtio-serial` driver baseline is available as a PCI-matched WASM
  service (`virtio.serial`) with discovery and register-access IPC as a
  foundation for higher-level transport consumers.
- Networking design baseline now has a dedicated architecture plan in
  `docs/architecture/22-networking-virtio-net-and-stack.md`, defining explicit
  QEMU NIC configuration, `virtio-net` driver/service boundaries, and phased
  TCP/UDP stack rollout, including full-scope IPv6 and multi-address/
  multi-stack instance support in later phases.

- Two scheduler bugs affecting kernel worker threads are fixed:
  (1) `proc->ctx.rsp` was initialized to the process stack top at spawn time,
  causing the scheduler to treat every fresh worker as "has blocked context to
  resume" and incorrectly invoke `context_switch_high` → `process_trampoline`
  instead of `process_run_worker_on_stack`.  Fixed by clearing `proc->ctx.rsp`
  to 0 after copying into `main_thread->ctx`, and teaching
  `process_validate_context` to treat rsp==0 as a valid "no saved context"
  sentinel.
  (2) On SMP (4-CPU QEMU) worker threads were permanently orphaned: while the
  main thread ran (proc->state==RUNNING), other CPUs dequeued and silently
  dropped the workers from the ready queue (proc->state!=READY check), leaving
  them with in_ready_queue=0 and state=READY but no re-enqueue path.  Fixed
  by running orphaned fresh workers inline via `process_run_worker_on_stack`
  immediately after the main thread yields and before proc->state is set back
  to READY, exploiting the RUNNING window to guarantee exclusive access.
  Together these allow the threading IPC stress self-test to complete 32
  in-process IPC message exchanges and emit `[test] threading ipc stress ok`.
