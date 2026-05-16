# Architecture Notes

This document is the architecture index and entry point for the repository.
Detailed design and implementation status now live in focused documents under
`docs/architecture/`.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.
Threading design details are maintained in
`docs/architecture/15-threading-and-lifecycle.md`.
Ring-3 isolation architecture and separation model are documented in
`docs/architecture/14-ring3-isolation-and-separation.md`.
Latest checkpoint: Phase 2 mapping minimization is closed for current strict
scope; Phase 5 fault-policy expansion coverage is in place for `#PF`, `#UD`,
`#GP`, `#DE`, `#DB`, `#BP`, `#OF`, `#NM`, `#SS`, and `#AC` process-local
termination probes, with explicit containment liveness marker
(`[test] ring3 containment liveness ok`) and mixed abuse churn marker
(`[test] ring3 mixed stress ok`); Phase 6 scheduler/trap robustness is closed
for current scope
(watchdog + mixed-stress + trap-integrity rollout complete); Phase 7
memory-service/shared-mapping isolation
is closed for current strict scope with owner-bound checks, explicit grants/
revoke, strict ring3 cross-process deny/allow markers, app-pair forged/stale
negative checks, kernel misuse-matrix gate markers, and shared-map state
ordering hardening; Phase 8 strict-mode/default compatibility-path deletion is
closed for current scope with `WASMOS_RING3_STRICT` removed, plus removal of
low-slot strict-mode configuration
knobs (`WASMOS_LOW_SLOT_SWEEP`, `WASMOS_LOW_SLOT_SWEEP_LEVEL`,
`WASMOS_IDENTITY_PD_COUNT`) in favor of fixed strict baseline behavior and a
green strict stabilization cycle (`run-qemu-test`, `run-qemu-ring3-test`,
`run-qemu-cli-test`).
Kernel boot smoke now also runs a shared-memory misuse matrix (forged IDs,
wrong-owner grant/revoke attempts, pre/post-grant map deny/allow, idempotent
revoke, and release-balance checks) in the strict-ring3 gate.
Threading rollout (`docs/architecture/15-threading-and-lifecycle.md`) is
closed through Phase D for current scope: scheduler-active internal worker
threads (dedicated kernel stacks + worker entrypoints), targeted multi-thread
IPC stress, and native ring3 syscall coverage (`gettid`, `thread_yield`,
`thread_exit`, `thread_create`, `thread_join`, `thread_detach`, including
deny-path markers) are validated in smoke. A user-facing continuation-style
native thread wrapper API (`wasmos/thread_x86_64.h`) is available for native
ring3 callers. A separate opt-in strict ring3
thread-lifecycle profile is now available via `run-qemu-ring3-threading-test`
to validate strict ring3 threading signals (ring3-threading spawn plus thread
create/join/detach syscall markers). The lifecycle profile now also checks
kill-while-blocked wait wakeup behavior via `[test] threading wait kill wake ok`
plus join-after-kill ordering and kill-during-join wakeup markers
(`[test] threading join after kill order ok`,
`[test] threading join kill wake ok`) without altering baseline strict startup
behavior. Stack teardown now restores guard-page mappings before allocator free
so recycled pages remain reachable through the shared higher-half alias window
under strict threading stress.
Ring3 mapping hardening now requires explicit `MEM_REGION_FLAG_USER` for
user-slot mappings in `paging_map_4k_in_root`; legacy implicit user-slot flag
bridging is removed, and ring3 hostcall map paths were updated to pass
explicit user mapping flags.
Syscall argument hardening now also enforces strict signed-32 width checks for
`EXIT` and `THREAD_EXIT` status arguments, rejecting lossy 64-bit values.
Hostcall argument hardening now rejects negative endpoint values in
`wasmos_serial_register` before endpoint-ID conversion to `uint32_t`.
Strict ring3 IPC-call adversarial coverage now includes a stale/future
`request_id` replay-denial marker (`[test] ring3 ipc call stale id deny ok`) in
the request/reply correlation path.
Strict ring3 IPC-call adversarial coverage now also checks out-of-order pending
reply retention with marker `[test] ring3 ipc call out-of-order retain ok`.
Strict ring3 IPC-call adversarial coverage now also includes invalid-source
spoof denial with marker `[test] ring3 ipc call spoof invalid source deny ok`.
Control-plane deny assertions now also include explicit endpoint-policy deny
coverage marker (`[test] ring3 ipc call control endpoint deny ok`).
Ring3 IPC stress coverage now also includes endpoint-ownership + sender-context
authentication stress marker (`[test] ring3 ipc owner+sender stress ok`) after
multiple inauthentic reply drops in the adversarial call path.
Dedicated strict ring3 fault-storm validation is now available via
`run-qemu-ring3-fault-storm-test` and asserts forward progress/watchdog markers
plus trap-frame integrity under repeated mixed fault churn.
CLI smoke flake reduction now uses per-session isolated ESP runs
(`WASMOS_QEMU_ISOLATE_ESP=1`) and deterministic suite status markers from
`scripts/run_unittest_suite.py`.
Hostcall pointer-boundary audit now verifies explicit user-VA resolution/range
checks across pointer-bearing entry paths; the remaining host-view sync bridge
in `wasm_copy_*_sync_views` is now explicitly tracked with TODOs.
Forward note: future deterministic kernel race/integration tests should use a
centralized hook/instrumentation layer around kernel transition points (for
example scheduler/process/thread lifecycle events) so orchestration logic does
not spread as ad-hoc test fragments across runtime code paths.
Build configuration now has a Kconfig-compatible entry point (`Kconfig`) plus
`configs/wasmos_defconfig`, with CMake importing `build/.config` through
`scripts/kconfig_to_cmake.py` when present. The imported scope is currently
intentionally narrow (existing CMake toggles and a few key scalar values) to
preserve minimalism and keep behavior deterministic.
Service dependency wiring now uses PM-hosted registry IPC (`SVC_IPC_REGISTER_REQ`
and `SVC_IPC_LOOKUP_REQ`) so drivers/services create/register their own
endpoints and discover dependencies at runtime instead of PM-injected
per-application endpoint bindings.
libc stdio compatibility now includes direct console-backed `read`/`write`
handling for `STDIN_FILENO`/`STDOUT_FILENO`/`STDERR_FILENO` (0/1/2), while
filesystem reads/writes continue to use FS IPC descriptors (`>=3`).
Line-oriented console input helpers are now exposed in libc and language
wrappers (`readline` in C stdio plus AssemblyScript/Go/Rust/Zig wrapper APIs)
as thin loops over `console_read`.
Minimal libc coverage was also expanded in both user-space and kernel-space
utility layers with common string/ctype/stdio helpers (`memmove`, `strnlen`,
`strchr`/`strrchr`, `strcpy`/`strncpy`, `isspace`/`isdigit`/`isxdigit`,
`getchar`/`putchar`/`fputs`) to reduce ad-hoc reimplementation in services and
drivers.
Build metadata now also includes IDE-only C source/object targets for
drivers/services so language servers can resolve libc/driver headers for
WASM-module sources that are primarily built via custom commands.
Drivers/services CMake wiring is now centralized through shared root helper
functions for wasm-C module compilation/packing and IDE companion targets,
replacing duplicated per-component custom-command blocks.
Filesystem namespace now starts from a virtual root (`/`) with explicit mount
subtrees and split backend responsibilities: `fs-manager` is the canonical
filesystem IPC entrypoint (`fs.vfs`) and routes requests to registered backend
drivers; `fs-fat` provides boot/FAT routing and `fs-init` provides initfs
listing. Cross-context filesystem buffer sharing is now modeled as explicit
borrow/release grants (`fs_buffer_borrow`, `fs_buffer_release`) with read/write
access bits, so `fs-manager` can proxy requests while backends continue to use
zero-copy FS-buffer access against borrowed caller buffers.
Device discovery now includes PCI-inventory-driven matching in `device-manager`
with enriched `pci-bus` inventory records (class/subclass/prog-if plus minimal
MMIO/IRQ hints). PM now accepts a capability-profile spawn request variant for
module spawns, and kernel policy enforces spawn-time PIO port-range and IRQ
line restrictions (defaulting to coarse app capabilities when no spawn profile
is provided). Driver match/capability values are embedded in each driver
WASMOS-APP package; `device-manager` requests module metadata from PM at
runtime and matches those records against `pci-bus` inventory. PM now also
exposes initfs metadata lookup by module path (`PROC_IPC_MODULE_META_PATH`) so
early user-space can resolve driver metadata without depending only on the
initial boot-module index list.
When native `menuconfig`-style frontends are unavailable, build configuration
can be edited through the in-repo `kconfiglib` interactive fallback script
(`scripts/kconfiglib_menuconfig.py`), exposed via CMake targets.

## Architecture Document Map
- [Goals](architecture/01-goals.md)
- [Current System Summary](architecture/02-current-system-summary.md)
- [Architectural Direction](architecture/03-architectural-direction.md)
- [Boot and Handoff](architecture/04-boot-and-handoff.md)
- [Scheduling and Preemption](architecture/05-scheduling-and-preemption.md)
- [Process and IPC](architecture/06-process-and-ipc.md)
- [Memory Management](architecture/07-memory-management.md)
- [Runtime and Packaging](architecture/08-runtime-and-packaging.md)
- [Drivers and Services](architecture/09-drivers-and-services.md)
- [CLI and User-Space Baseline](architecture/10-cli-and-user-space.md)
- [Diagnostics and Status](architecture/11-diagnostics-status.md)
- [Repository Map and Validation Baseline](architecture/12-repo-map-and-validation.md)
- [Virtual Terminal](architecture/13-virtual-terminal.md)
- [Ring3 Isolation and Separation](architecture/14-ring3-isolation-and-separation.md)
- [Threading and Lifecycle](architecture/15-threading-and-lifecycle.md)

## Update Rules
- Update the relevant feature document(s) in `docs/architecture/` when behavior
  changes.
- Keep cross-document references consistent across `README.md`, `docs/TASKS.md`, and
  architecture docs.
- Prefer appending concrete implementation notes over vague roadmap text.
