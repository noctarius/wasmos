# Active Tasks

This file is the agent-facing work index: it lists unfinished, actionable work
by subsystem. It is not a changelog and does not repeat completed baseline
work. See `STATUS.md` for the current implementation snapshot, `git log` for
history, and `ARCHITECTURE.md` for the complete document map.

## How to Use This File

- Treat each top-level section as an independent workstream. Read its linked
  architecture document before changing the subsystem.
- Keep tasks concrete and remove them when their done condition is met; record
  the resulting baseline in `STATUS.md`.
- Preserve ABI parity across wasm3, WARP, native, libc, and libsys variants.
- QEMU integration targets share `build/esp`; run them sequentially.
- Code citations (`file:line`) point at a live `TODO`/`FIXME` marker or the
  exact site of the gap; verify the line before acting (code moves).
- Every item carries exactly one category tag. The tag states what kind of
  change the item is, not how urgent it is:
  - `[BUG]` — the code does something incorrect: a wrong result, a crash, a
    leak, a race, a memory-safety violation, or a documented contract the
    implementation does not honour. A `[BUG]` is safe to fix on its own.
  - `[FEATURE]` — a capability that does not exist yet.
  - `[ENHANCEMENT]` — existing behaviour is correct but should be better:
    scaling limits, hardening against inputs not yet reachable, performance,
    or ergonomics.
  - `[CLEANUP]` — removal or consolidation with no behaviour change: dead
    code, duplicated definitions, superseded shims.
  - `[TEST]` — missing coverage, or coverage that does not exercise what it
    claims to.
  - `[DOCS]` — a documentation or IDL-doc statement that disagrees with the
    code. Fixing one changes no behaviour, but a wrong contract in a header or
    in `abi/*.yaml` propagates to every caller written against it.
- Prefer draining `[BUG]` before `[FEATURE]` in a subsystem you are about to
  extend: several entries here describe defects that a new caller would
  otherwise inherit.

## Kernel, Memory, and Isolation

Source: `architecture/06-memory-management.md`,
`architecture/10-capability-and-policy.md`,
`architecture/11-ring3-isolation-and-separation.md`, and
`architecture/28-smp.md`.

- [ ] [ENHANCEMENT] Replace the WARP shared-linear-memory local fault retry with cross-CPU
  TLB-shootdown IPIs before any live page-table reclaim under concurrent APs
  (`src/kernel/arch/x86_64/cpu_x86_64.c:554` `TODO(smp-tlb)`,
  `src/kernel/warp/ring3_trampolines.c:166`).
- [ ] [CLEANUP] Retire the `shmem` subsystem in favour of a single `xfer_buffer` sharing
  mechanism. The two are implementations of one concept -- an id, a
  grant/borrow step, a copy path and a zero-copy map path -- differing mainly
  in that `shmem_grant` addresses a **pid** while `xfer_buffer_borrow`
  addresses the context owning an **endpoint** (the shape the rest of the
  system uses), and that shmem is gated by the DMA capability rather than by
  explicit per-borrow READ/WRITE rights. Nothing needs shmem's extra
  expressiveness: xfer buffers already back **DMA** -- the ATA driver DMAs into
  a borrowed buffer via `wasmos_dma_map_borrow` (`src/drivers/ata/ata.c:452`,
  `abi/hostcalls.yaml` `dma_map_borrow`) -- and already back the compositor's
  own framebuffer and backbuffer (`ND_BUFFER_KIND_FRAMEBUFFER` /
  `ND_BUFFER_KIND_XFER`, `src/services/gfx_compositor/gfx_compositor.zig:726`
  and `:755`).

  The duplication has already cost a real bug: the linear-memory window
  placement rule was hand-written once per mapper, so `shmem_map_auto` and
  `xfer_buffer_map` carried the same defect and both mapped overlays on top of
  live app data (fixed in `3d8811828e` by sharing
  `wasm_linmem_place_overlay()`). Two mechanisms means every such rule is
  written twice.

  Main consumer to migrate is graphics: the compositor allocates each window's
  app-facing buffer with `shmem_create`
  (`src/services/gfx_compositor/gfx_compositor.zig:1632`) and replies
  `(buffer_id, shmem_id, stride_bytes)`; apps map it with `shmem_map_auto`
  (e.g. `examples/rust/tetris/tetris.rs:359`, `src/libui/assemblyscript`).
  Note those apps also call `shmem_flush` per frame on a window they have
  already mapped, which appears to be a self-copy into the pages it is mapped
  onto -- confirm before carrying that pattern across.

  Done when: no `shmem_*` host call remains in `abi/hostcalls.yaml` (ids
  renumbered per the ID RULES, whole-world rebuild + `.cache/warp_aot`
  cleared), both runtimes' shims are gone, and gfx/libui/tetris run on xfer
  buffers under wasm3 AND WARP.
- [ ] [FEATURE] Move native `.wap` services from ring 0 to the ring-3 native execution
  path (syscall-backed `libsys_native` primitives + capability enforcement).
  Unblocks isolating `gfx-compositor`, `font-service`, and `net-stack`
  (`architecture/11`:380-414).
- [ ] [FEATURE] Run the **wasm3 interpreter at CPL=3** via a ring-3 trampoline, reusing the
  WARP scaffolding. Depends on the native ring-3 execution path above.

  Two payoffs, the second bigger than the first. (a) A wasm3 guest is currently
  never timer-preempted -- `process_preempt_from_irq` refuses a kernel-mode
  frame outright (`src/kernel/process.c`, `if (from_kernel) return 0;`), so a
  guest loop with no host call holds its CPU until it returns and one heavy app
  stalls the desktop (`architecture/11`, *Which Workloads Reach Ring 3*).
  (b) `src/kernel/wasm3/link.c` is ~3,400 lines of 103 hand-written host-call
  shims that exist only because the interpreter runs in-kernel and can call
  kernel functions directly. In ring 3 they become syscalls into the SAME
  dispatch WARP already uses (`abi/generated/c/wasmos_ring3_dispatch.inc`), so
  there is one implementation per host call instead of two. Most wasm3/WARP
  divergences fixed to date were two copies of one rule drifting apart.

  Reuse rather than invent -- all of this is built and proven for WARP:
  the `WARP_R3_*` VA layout (`src/kernel/include/warp_ring3.h`: linmem RW, HC
  stub page R-X with 8 bytes per call, entry/ret trampolines, 256 KiB user
  stack), the `READ|EXEC|USER` mapping in
  `src/kernel/warp/ring3_trampolines.c:103`, `r3_do_iretq`
  (`src/kernel/warp_driver.cpp:348`), and the embedded-blob-into-a-process
  loader pattern in `src/kernel/kernel_ring3_probe_runtime.c`.

  A second compile of `libs/wasm3` is unavoidable and is the right shape:
  today its objects use `CFLAGS_WASM3 = CFLAGS_KERNEL + ...`
  (`src/kernel/CMakeLists.txt:16`), which carries `-mcmodel=kernel` (top-2 GiB
  addressing, wrong for a user VA) and links against kernel symbols. A separate
  link unit makes "no kernel symbols" a **link error** instead of a runtime #GP.

  Phases, each independently verifiable:
  1. **Spike.** Build `libs/wasm3` plus a minimal ring-3 platform layer as a user
     blob; map it R-X per process with a private RW data copy and a user stack;
     `iretq` to it; run a trivial module whose only host call is `console_write`
     through the HC stub page. Answers the three unknowns at once: does it link
     with no kernel symbols, does entry/exit work, is writable state per-process.
  2. **Generated ring-3 host-call client.** A new emitter in
     `scripts/gen_abi_hostcalls.py` producing syscall stubs (`RAX = 0x100 + id`);
     the `m3ApiRawFunction` bodies become those stubs. Deletes the 103 kernel
     shims.
  3. **Linear memory** at `WARP_R3_LINMEM_BASE`, grown by syscall. Expect
     surgical edits in `libs/wasm3/source/m3_env.c` -- it has been patched
     exactly here twice already (`d7f644f02c`, `fbb5b0dc81`), which is the
     accepted last resort for this file, kept minimal.
  4. **Heap and libc** from a user RW region. This RETIRES most of
     `src/kernel/wasm3/shim.c`: the per-pid heap table, the CPU-local heap
     binding, its 42 spinlock calls and its `preempt_disable` all exist solely
     because one in-kernel interpreter multiplexes every wasm process. One
     interpreter per address space removes the multiplexing, not just the lock.
  5. Flip wasm3 apps onto the ring-3 path and delete the in-kernel execution
     path.

  Done when: a wasm3 guest is preempted (a full-frame blitting app stays
  responsive AND leaves the desktop responsive), `wasm3/link.c`'s shims are
  gone, and both runtimes pass `run-qemu-test` plus the CLI suite.

  Risks: the user code model versus `-mcmodel=kernel`; wasm3 internals that
  assume a kernel-ish environment (`setjmp`/`longjmp`, allocator assumptions);
  and step 3, which is where the historical linmem aliasing bugs lived.
- [ ] [ENHANCEMENT] Finish ring-3 hardening TODOs: drop `PML4[0]` from the kernel root once
  bootstrap no longer needs the low slot (`src/kernel/paging.c:338`
  `TODO(ring3-phase3)`); add process-local exception handling beyond the
  current `-11` kill (`src/kernel/arch/x86_64/cpu_x86_64.c:484`
  `TODO(ring3-phase5)`); handle stack allocations that can exceed the 512 MB
  higher-half window (`src/kernel/process.c:131` `TODO(ring3-phase2)`).
- [ ] [ENHANCEMENT] Tighten the temporarily-broadened page-fault reason back to
  `EXEC_VIOLATION`-only once the ring-3 fault tests are stable
  (`src/kernel/arch/x86_64/cpu_x86_64.c:578`).
- [ ] [ENHANCEMENT] Introduce allocation intents (`STACK`, `PGTABLE`, `DMA32`, `GENERIC`),
  then remove the low-memory DMA constraint from kernel stacks and page tables
  (`src/kernel/paging.c:31`; `architecture/06`:455-497).
- [ ] [FEATURE] Decide and implement kernel reachability beyond the current 512 MB low
  window: full physmap or a bounded `kmap` cache (`architecture/06`).
- [ ] [ENHANCEMENT] Route pointer-bearing syscall/IPC entry paths through
  `mm_user_range_permitted` (`src/kernel/memory.c:1204`).
- [ ] [ENHANCEMENT] Replace the 64-bit-bitmap linmem-slot ceiling with a growable pool so
  concurrent linear-memory slots are bounded by memory, not 64
  (`src/kernel/linmem_slots.c:15` `TODO(linmem-pool)`).
- [ ] [ENHANCEMENT] Move the process table off its static array onto kernel list storage
  (`src/kernel/process.c:36` `FIXME(process-list)`).
- [ ] [ENHANCEMENT] Replace the global shared-region cap, make context region sizing
  configurable, and add committed/resident (RSS) memory accounting for `ps`
  (`src/kernel/process.c:2492` `TODO(memory-rss)`; `architecture/06`:507-512).
- [ ] [CLEANUP] Wire the kernel-thread trampoline into PM launch policy and delete the
  legacy trampoline (`src/kernel/process.c:1503,1771`).
- [ ] [BUG] Fix the `dma_map_borrow` capability-enforcement divergence: the WARP
  wrapper (`src/kernel/warp/link.cpp`) omits the DMA-capability + max-bytes/range
  check that the wasm3 wrapper (`src/kernel/wasm3/link.c`) enforces, so the WARP
  path is weaker. Bring WARP to parity. Found during the host-call ABI inventory.
- [ ] [BUG] Fix the `warp_ring3_dispatch` `proc_info_stats` ctx bug: the case passes
  `ctx5` (== `a4`, the `stats` param) as the kernel ctx, but a 5-param host call
  needs ctx in `a5` (R9) — there is no `ctx6`, so the hand-written case silently
  reused `ctx5`. Ring-3 `proc_info_stats` therefore gets a garbage ctx (a user
  offset) → wrong `warp_mem` resolution. The generated dispatch computes
  `ctx = a<arity>` and fixes it; fix lands when the ring-3 dispatch is swapped in.
  Found during host-call dispatch codegen (`src/kernel/warp/link.cpp:3130`).
- [ ] [FEATURE] Extend DMA isolation to an IOMMU domain model (VT-d/AMD-Vi) and add
  non-coherent cache-maintenance hooks before targeting non-coherent hardware
  (`architecture/12`:88,618,625).
- [ ] [ENHANCEMENT] Harden the boot and native ELF loaders with checked arithmetic for
  program-header offsets, segment file/virtual ranges, boot-info layout totals,
  and cursor advances; reject values that overflow allocation sizes or boot ABI
  fields before copying.


- [ ] [BUG] Pass `MEM_REGION_FLAG_*` to `paging_map_4k`, not raw `PT_FLAG_*`. The
  callee rebuilds the PTE from its own flag space, so `PT_FLAG_PCD` is dropped
  and device scratch pages lose cache-disable, leaving their memory type to the
  MTRRs alone. Same pattern in `arch/x86_64/lapic.c` and `ioapic.c`
  (`src/kernel/mmio.c:17` `FIXME`).
- [ ] [BUG] Apply the shmem/WARP physical-zone floor in `linmem_slot_commit`. It
  calls `pfa_alloc_pages` with no floor, so slot-backed linear memory falls
  outside the guarded zone that `memory.h` and `physmem.h` still describe as an
  invariant (`src/kernel/linmem_slots.c:105`).
- [ ] [BUG] Serialize the capability table. Grants (spawn) and checks (hardware host
  calls) run on any CPU with no lock, so a grant that grows `g_cap_ctx` can race
  a concurrent lookup (`src/kernel/capability.c`).
- [ ] [ENHANCEMENT] Validate the initfs higher-half alias. The pointer fixup assumes initfs
  lies inside the 512 MiB shared window; a higher firmware placement yields an
  unmapped pointer with no diagnostic (`src/kernel/kernel_boot_runtime.c:69`).
- [ ] [BUG] Give `isr_exception_1` (#DB) the `PUSH_REGS` every other user-fault stub
  performs. Without it `x86_exception_panic_frame` reads the CPU frame where it
  expects the register block, so the #DB dump is garbage and reads past the
  frame (`src/kernel/arch/x86_64/cpu_isr.S:140` `FIXME`).
- [ ] [BUG] Push a zero error code in `DECL_EXC` for the vectors that do not supply
  one. The macro passes the post-`PUSH_REGS` `%rsp` unconditionally, which the
  panic decoder reads as the error-code layout; for vectors 5, 9, 15, 16, 18-20,
  22-28 and 31 the dump prints rip as "err" and cs as "rip"
  (`src/kernel/arch/x86_64/cpu_isr.S:102` `FIXME`).
- [ ] [BUG] Read the interrupted RSP from `frame[4]` for kernel faults too. In long
  mode the IRET frame always carries SS:RSP, so the current
  `(cs & 3) == 3` branch reports the IST/exception-stack address instead of the
  interrupted stack (`src/kernel/arch/x86_64/cpu_x86_64.c`,
  `x86_exception_panic_frame`).
- [ ] [BUG] Correct both PCI protocol GUIDs in the bootloader. Neither matches the
  UEFI spec value, so both `ConnectController` passes are silent no-ops
  (`src/boot/boot.c:213` `FIXME`, `src/boot/uefi.h`).
- [ ] [BUG] Free the previous buffer on the `EFI_BUFFER_TOO_SMALL` memory-map retry
  and on `boot_capacity` growth. Both paths reallocate without releasing, so the
  pages leak as `EFI_LOADER_DATA` and the kernel never reclaims them
  (`src/boot/boot.c:617`, `:861`).

## Scheduler, Threads, and IPC

Source: `architecture/07-scheduling-and-preemption.md`,
`architecture/08-threading-and-lifecycle.md`,
`architecture/09-process-and-ipc.md`, `architecture/29-threadable-scheduler.md`,
`architecture/30-ipc-direct-switch.md`, `architecture/32-coroutines-futures-promises.md`,
and `architecture/33-completion-ports.md`.

- [ ] [FEATURE] Add scheduler/process observability: committed-memory-aware process
  reporting (feeds `memory-rss`), scheduler latency/stall counters, and useful
  per-process metrics.
- [ ] [FEATURE] Define a fairness/budget policy now that the priority bands are actually
  wired (`pm_sched_prio_for_flags` → `process_set_main_prio`); measure first and
  keep the existing preemption and SMP regression gates.
- [ ] [FEATURE] Surface futex to userspace. The kernel primitive exists (`futex_wait/wake`
  in `src/kernel/futex.c`, WASM hostcalls at `src/kernel/wasm3/link.c:3800`) but
  is absent from libc `api.h` and the native `int 0x80` path; user mutexes still
  yield-spin (`src/libsys/wasm/include/wasmos/mutex.h:44`,
  `src/libsys/native/include/wasmos/libsys_native.h:296`,
  `src/libc/include/wasmos/mutex.h:44`). Add the declarations + native syscall
  and make the user mutex consume it.
- [ ] [ENHANCEMENT] Promote libsys event-loop intents into the shared future/promise contract
  with one receive pump per endpoint and request-id/generation cancellation.
- [ ] [CLEANUP] Remove synchronous request/reply IPC from libc, libsys, native wrappers,
  and remaining service call sites (nested `ipc_select_one` reply-waits). The
  future/promise bridge has landed and net-stack uses it, but the
  fs-manager↔device-manager sync-round-trip deadlock hazard remains
  (`architecture/09` synchronous-IPC section; `src/services/fs_manager/fs_manager.c:608`).
  - [x] CLI VT path: the three post-init VT round-trips (`GET_ACTIVE_TTY`,
    `SWITCH_TTY`, `READ_REQ`) now go through one owned `wasmos_sys_event_loop`
    pump as IPC futures. This fixed a real character-loss bug — see below.
  - [ ] CLI, rest of the way to a coroutine app. Remaining synchronous receives:
    `cli_register_vt_writer` / `cli_set_vt_mode` still drain the VT endpoint
    directly (init-only, before the pump is in use), and the PM/FS/spawn paths
    still block on `wasmos_ipc_select_one(g_reply_endpoint)`
    (`cli.c` ~1111, ~1381, ~1404, ~2076), which is why the CLI is structurally
    blind to input while a command waits (Ctrl+C during a long command cannot
    work). Converting those is the prerequisite for collapsing the CLI's two
    endpoints onto one, and then for replacing the `g_phase`
    (`INIT/PROMPT/READ/WAIT_IPC/FAILED`) machine with a coroutine per command.
    Do not collapse the endpoints first: sharing one endpoint while those
    blocking receives remain just moves the input loss into them.
- [ ] [BUG] Publish `POLL_EV_IN` on the notification path so NOTIFICATION endpoints
  are visible to `ipc_select_wait`: `ipc_notify_from` does not call
  `poll_notify` (`src/kernel/ipc.c:363-385`). Prerequisite for completion ports.
- [ ] [CLEANUP] Remove the legacy `process_block_on_ipc` shim once all callers move to the
  select/idle-wait path (`src/kernel/process.h:225`, `src/kernel/process.c:1588`).
- [ ] [DOCS] Reconcile `architecture/30-ipc-direct-switch.md` (fully unimplemented; its
  header flags conflict with the futures direction) with the futures model, or
  formally drop it. Do not add a direct-switch API that reintroduces nested
  blocking IPC.
- [ ] [FEATURE] Define a PM-mediated cooperative lifecycle-control protocol over IPC:
  capability-gated shutdown/cancel, acknowledgement, deadline-based escalation
  to `process_kill`, and event-loop safe points. No POSIX signal handlers or
  arbitrary asynchronous thread interruption.
- [ ] [FEATURE] Add asynchronous, capability-gated process-death watches for supervisors
  and integrate them with the lifecycle-control event path.
- [ ] [ENHANCEMENT] Normalize request-id validity across WASM and native `libsys` (signed vs
  unsigned wire representation and the reserved invalid value).
- [ ] [BUG] Make console-backed libc `read`/`write` reject or chunk counts beyond the
  `int32_t` ABI limit and return the actual byte count from the console backend.
- [ ] [FEATURE] Add the AssemblyScript async/coroutine wrappers over the shared
  future/promise contract (native and WASM cores plus Rust/Go/Zig are done;
  AssemblyScript remains deferred).
- [ ] [FEATURE] Defer true WASM parallelism and hard coroutine preemption until runtime
  locking/reentrancy has a dedicated design and validation plan.
- [ ] [FEATURE] Implement completion ports (`architecture/33`, design proposal only): a
  kernel-owned bounded CQ with notification doorbells and generation-tagged
  operation tokens, as a batched completion source for the future/promise
  runtime and high-rate networking.
- [ ] [FEATURE] Green-thread coroutine runtime (`architecture/32` §52, spike): re-base the
  WASM coroutine substrate onto stackful **green threads** (M:1 — many coroutines
  on one OS-scheduled entity per instance), suspending guests at the host-call
  boundary rather than the §51 stackless C baseline. Public API stays
  coroutine/future vocabulary (never `thread_create`; `thread_*` demoted to hidden
  substrate). Engines stay untouched (0 `libs/warp`; 0–10 `libs/wasm3`): WARP rides
  the kernel thread switch at the ring-3 host-call trap (single-invocation already
  works via `process_yield`; concurrent coroutines need per-coroutine ring-3 stacks,
  ~100–200 LOC glue), wasm3 runs `m3_Call` on a switchable C stack (per-coroutine
  operand buffers). Expose the primitives as a generated host-call family so all
  languages — including **AssemblyScript**, which cannot link the C runtime — are
  first-class (closes the AS-has-no-coroutines gap and retires `coroutine.{rs,go,zig}`).
  Invariants: waits yield through the scheduler (never block the OS entity);
  cooperative safepoints for compute-bound coroutines. Boundary: concurrency
  in-process (coroutines), parallelism across processes (no M:N). Decision record:
  native stackless engine support (stack-switching proposal / split state machines)
  **declined** — only buys a coroutine-count scale WASMOS won't reach, at the cost
  of forking two vendored engines + continuation surgery on a single-pass JIT.
  Migration must keep the net stack + TLS green throughout.


- [ ] [BUG] Bound `cpu_sched_try_steal`'s CPU loop and modulus by
  `cpu_sched_usable_cpus()` rather than raw `g_cpu_count`. The raw value is
  unvalidated MADT data: 0 divides by zero, a value above `WASMOS_MAX_CPUS`
  indexes `g_cpus[]` out of bounds (`src/kernel/sched_thread.c:779` `FIXME`).
- [ ] [BUG] Make the `IPC_CALL` request-id counter atomic and lock the per-pid slot
  table. Two CPUs issuing `IPC_CALL` concurrently can mint the same id, which
  both reply correlation and the `syscall_ipc_request_id_issued` replay guard
  assume is unique (`src/kernel/syscall.c:58` `FIXME`).
- [ ] [BUG] Bounds-check `line` in `irq_sharing_register/ack/unregister/dispatch/
  has_sharers`. `WASMOS_ERR_IRQ_BAD_LINE` exists but is returned only for a
  NULL table; an out-of-range line is an out-of-bounds access
  (`src/kernel/irq_sharing.c:71`, `:102`, `:144`).
- [ ] [BUG] Serialize `kenv_set`/`kenv_unset`. The store has no lock, and the
  "callers run descheduled" justification does not hold on SMP: two CPUs can
  claim the same free slot (`src/kernel/include/kenv.h:23` `FIXME`).
- [ ] [BUG] Hold `g_subsystem_lock` across use of a looked-up entry, or refcount it.
  Both lookups return a heap entry pointer after dropping the lock, while
  `wasmos_subsystem_registry_drop_owner` frees broker entries and handler node
  arrays (`src/kernel/subsystem_registry.c:567` `FIXME`).

## Runtime, Packaging, and Service Discovery

Source: `architecture/13-runtime-and-packaging.md`,
`architecture/14-libsys-and-service-runtime.md`, and
`architecture/15-drivers-and-services.md`.

- [ ] [ENHANCEMENT] Close the remaining WARP refinement TODOs (host-call coverage itself is
  broad): synchronise symbol lookups/alloc under SMP (`src/kernel/warp/link.cpp:90`
  `TODO(smp-warp)`, `src/kernel/warp/shim.cpp:579` `FIXME(smp-warp)`); reserve
  shmem auto-map windows against real heap growth (`link.cpp:1988`); write-combining
  PAT for framebuffer/scanout (`link.cpp:2145`); enable W^X once kernel paging
  supports per-4K remap (`src/kernel/warp/posix_kernel.c:97`). Provide a supported
  alternative to the vendored-runtime-pointer shim without modifying `libs/warp`.
- [ ] [FEATURE] Make WARP multithreaded WASM either functional (locate the owner module
  bytes so VM threads run under WARP — `src/kernel/warp_driver.cpp:952`
  `module_bytes = nullptr`) or explicitly unavailable at the API boundary.
- [ ] [BUG] Fix (or confirm resolved) the WARP ring-3 delegated-executor argv
  coherence bug: `wasmos_script`'s first xfer-buffer read of argv can read as
  zero because the kernel higher-half alias and the ring-3 user-VA view diverge
  for that page (wasm3 is unaffected). Verify against a repro before asserting
  fixed (`architecture/13`:549-551; STATUS known non-green path).
- [ ] [BUG] Restore prior linear-memory PTEs on wasm3 `xfer_buffer`/`shmem` unmap
  instead of only dropping shared refcounts (`src/kernel/wasm3/link.c:1553,1563,2504,2552`
  `FIXME(xfer-unmap)`/`FIXME(shmem-map-auto)`).
- [ ] [ENHANCEMENT] Replace the wasm3 parent-name spawn heuristic with explicit per-process
  identity (`src/kernel/wasm3/link.c:589` `FIXME`).
- [ ] [BUG] Track native VM `malloc`/`free` per-PID so a reaped process's native heap
  pages are reclaimed (`src/kernel/native_driver.c:597` `TODO(nd-vm)`; native
  services are currently long-lived, so this leaks).
- [ ] [FEATURE] Complete executable-broker handoff: route non-builtin subsystem handler
  kinds to a userland broker instead of returning `-1` (`src/kernel/wasmos_app.c:797`),
  ensure delegated argv/transfer-buffer reads are coherent, define failure
  handling, and add end-to-end broker tests.
- [ ] [FEATURE] Finish service-class discovery lifecycle: enumeration,
  add/remove/death notifications, capability-gated registration, and consumer
  migration where class lookup removes hardwired provider names.
- [ ] [FEATURE] Add driver/service supervision, restart/reincarnation, and controlled
  capability revoke/reissue on restart.


- [ ] [BUG] Raise `WASMOS_EXEC_APP_VERSION`/`g_header_sizes` to v6. They stop at v5
  while `WASMOS_APP_VERSION` is 6 and the packer writes a 76-byte header, so
  `wasmos_exec_is_wap_blob()` always returns 0 and every broker-delegated spawn
  (`.lua`/`.jar` via `wasmos_script`) fails with
  `WASMOS_ERR_PROC_SPAWN_BROKER_PLAN` (`src/kernel/wasmos_exec_format.c:16`
  `FIXME(wap-v6)`).
- [ ] [BUG] Initialise `hdr.region_count` on the packer's positional path. It is left
  as stack garbage and written into the `.wap`, so the kernel either rejects the
  package or walks N garbage region records and desynchronises every later
  section (`scripts/make_wasmos_app.c:1024` `FIXME`).
- [ ] [BUG] Bound `argc` in `wasm_driver_call` before marshalling. The wasm3 backend
  builds a fixed 4-slot argument array and passes `argc` straight to `m3_Call`;
  `wasm_driver_call_entry` checks, this path does not
  (`src/kernel/include/wasm_driver.h:96` `FIXME`).
- [ ] [CLEANUP] Retire or build `wasm_chardev.c`. It is absent from the kernel source
  list, nothing calls its API, and no rule emits the
  `_binary_chardev_server_wasm_*` symbols it references
  (`src/kernel/wasm_chardev.c:8` `FIXME(chardev-dead)`).
- [ ] [BUG] Fix `freeAlignedMemory` in the AOT host shim. It reads a size header at
  `ptr - sizeof(size_t)` that `allocAlignedMemory` never writes, so it faults or
  `munmap`s a garbage length (`src/tools/warp_aot/host_mem_utils.cpp:141`).

## ABI, Code Generation, and Error Handling

Source: `architecture/34-abi-idl-and-error-model.md`.

Phase order: 1 (error foundation, done) → 2 (host calls) → 3 (opcodes) → 4
(migrate call sites onto the generated surfaces). The error call-site migration
is deliberately last: it rewrites service/driver code that Phases 2/3 also
rewrite, so doing it after the generated host-call/opcode surfaces exist touches
each site once. Error domains ride those surfaces (`SHMEM_ERR_*` are host-call
returns; `FS_ERR_*`/`PROC_*` ride IPC opcodes), so the migration depends on them.

- [x] Phase 1 (error foundation) — COMPLETE: `abi/errors.yaml` IDL +
  `scripts/gen_abi_errors.py` generator + the per-language value ABI under
  `abi/generated/<lang>/wasmos_status.{h,rs,go,zig,ts}` — transport
  `wasmos_status_t`, generated domain registry (stable ids), packed
  `(domain, code)` constants seeded from the legacy taxonomy, decode lookups
  (`wasmos_strerror`), the fixed 8-byte frame / 40-byte error object, and the
  chain helpers (`wrap`/`unwrap`/`root`/`is`/`as`), each verified to compile
  (Rust/Go/C also run-tested) with its native toolchain. Generated files sit
  outside the `src/` format/lint scope. `quality` gained a `--check` re-gen
  guard and an advisory bare-`return -1;` inventory (services/drivers, ~24
  sites). Not yet wired into the OS — wiring is Phase 4.
- [x] Phase 2a: `abi/hostcalls.yaml` (all 117 host calls + the id-less wasm3-only
  `env.strlen`) + `scripts/gen_abi_hostcalls.py` generating the `HC_*` id enum,
  with Model validation (ids unique, dense `0..N-1`, ordered; `reserved` slots for
  retirements) and `--verify-source` proving the IDL's `(symbol, id)` set matches
  the live `src/kernel/include/warp_ring3.h` exactly. Wired into `quality`
  (`--check` + `--verify-source`). Param **kinds** captured but not yet emitted.
- [x] Phase 2b: generate the WARP `WASMOS_SYMBOLS(LINK)` table
  (`abi/generated/c/wasmos_symbols_warp.inc`) and the wasm3 link table
  (`wasmos_link_wasm3.inc`, m3 sig strings derived from param kinds + the
  `wasm3: i32` overrides for the 10 pointers wasm3 passes as a raw offset). Both
  `--verify-source`-proven to reproduce the live `link.cpp`/`link.c` exactly
  (117 host calls; ids, names, wrapper fn names, and sigs all match), wired into
  `quality`. `warp_fn`/`wasm3_fn` are derived (only the 5 `ipc_select_*` family
  fns need an explicit `wasm3_fn` override). Still parallel artifacts — not wired.
- [x] Phase 2c (AOT): generate the WARP AOT symbol table
  (`wasmos_symbols_aot.inc`, `WASMOS_AOT_SYMBOLS(LINK)`) — each entry a
  `stub_i<arity>`; validated against `warp_aot.cpp`. The AOT table is
  name-resolved (not position-coupled — only ring-3 dispatch is), so the
  generator emits the full host-call set and `--verify-source` asserts the live
  table is a subset (it currently omits `env.abort`/`wasi.proc_exit`; the
  generated set completes it, a safe additive change to validate at swap time).
- [x] Phase 2c (ring-3 dispatch): generate `warp_ring3_dispatch_table()`
  (`wasmos_ring3_dispatch.inc`) — a self-contained inline function
  (frame-decode stays hand-written, dispatch is generated). ctx is the computed
  `a<arity>`, which fixes the `proc_info_stats` bug. Compile-checked clean under
  `clang++ -Wall -Wextra -Werror` against arity-derived wrapper decls (proves
  every case's arg count matches its wrapper); `--verify-source` also confirms
  the per-id wrapper fn matches the live switch.
- [x] First swap (proves the loop): `wasm3_link_wasmos` (`src/kernel/wasm3/link.c`)
  now `#include`s the generated `wasmos_link_wasm3.inc` and expands
  `WASMOS_WASM3_LINKS(X)` instead of the hand-written `rc |= wasm3_link_raw(...)`
  cascade (129 lines retired). `abi/generated/c` added to `CFLAGS_KERNEL`. Live in
  the default wasm3 build: `run-qemu-test` boots through to CLI + calculator +
  halt, and `run-kernel-unit-tests` pass.
- [x] WARP kernel surfaces swapped in: the HC enum (`warp_ring3.h`),
  `WASMOS_SYMBOLS` (`link.cpp`), and the ring-3 dispatch
  (`warp_ring3_dispatch` now decodes the frame and calls the generated
  `warp_ring3_dispatch_table`). Validated by a WARP `run-qemu-test` (boots to
  CLI + calculator + halt) and the default wasm3 `run-qemu-test` (the enum header
  is shared). The `proc_info_stats` ctx fix is now live in the WARP kernel.
- [x] AOT tool table swapped in: `src/tools/warp_aot/warp_aot.cpp` now
  `#include`s the generated `wasmos_symbols_aot.inc` and expands
  `WASMOS_AOT_SYMBOLS(DYNAMIC_LINK)` (return-matched `stub_i<N>`/`stub_v<N>`).
  All kernel-side + AOT host-call tables are now generated from the IDL. WARP
  `run-qemu-test` boots with "using AOT binary" (payloads rebind against the
  generated table).
- [x] Phase 2c (client stubs): generate the guest import bindings for **all five**
  languages from the IDL — `abi/generated/{c/wasmos_imports.h,rust,go,zig,
  assemblyscript}/wasmos_imports.*` — every `wasmos`-module host call (incl.
  aliases) with its signature, idioms matched to the in-tree examples (C
  `extern … WASMOS_WASM_IMPORT`, Rust `#[link(wasm_import_module)]`, Go
  `//go:wasmimport`, Zig `pub extern "wasmos" … callconv(.c)`, AS `@external`).
  Each compile-verified with its real toolchain (`zig ast-check`, `rustc --target
  wasm32 -Dwarnings`, `go vet GOOS=wasip1`, `asc`, and a wasm32 `clang` compile of
  `api.h`), all wired into the `quality` `--check` guard.
  - C keeps its **ergonomic typed signatures** (`const char*`, `uint64_t*`,
    struct pointers) via a per-param `c_type:` override in the IDL (default
    `int32_t`, a wasm32 pointer being an i32 offset). `src/libc/include/wasmos/api.h`
    is now just its struct typedefs + `#define`s + the two native-only `mutex_*`
    decls + a relative `#include` of the generated header (relative-path include
    avoids threading `-Iabi/generated/c` through every app/driver/service/test
    compile that pulls in libc). The `mutex_try_lock`/`mutex_unlock` pair stays
    hand-written because it is a driver_api vtable entry (native), not a WASM
    host call — pending the futex migration.
  - **Docs are single-sourced.** Each host call carries a `doc:` field in the IDL,
    emitted as a comment into all five client stubs. Migrated the 17 legacy
    `api.h` comments and authored the remaining ~80 from the wrapper bodies so
    every call is documented in one place.
  - `--verify-source` is repurposed as the permanent hand-written-C-surface guard
    (not retired): every residual `WASMOS_WASM_IMPORT("wasmos", …)` decl in
    `src/libc`/`src/libsys` must name a real IDL host call with a matching arity
    (only the allow-listed `mutex_*` pair remains). The swapped kernel tables
    self-skip; `--check` guards every generated file.
  - (`dma_map_borrow` is a wrapper-body divergence, tracked separately under
    Kernel — not a table swap. A client-side ABI-version `static_assert` was
    dropped as meaningless: the guest has no second source of truth to assert
    the count/version against.)
- [x] Phase 3a (opcodes, ABI-header core): `abi/opcodes.yaml` (164 opcodes /
  19 subsystems, extracted faithfully from the header) + `scripts/gen_abi_opcodes.py`
  generating per-subsystem C enums (`abi/generated/c/wasmos_opcodes.h`), a
  best-effort `wasmos_opcode_name()` diagnostic lookup, and a doc reference table
  (`abi/generated/docs/opcodes.md`). Per-opcode doc comments migrated into the IDL
  (39 documented). `wasmos_driver_abi.h` now `#include`s the generated enums (the
  19 opcode enum blocks retired; error enums/flags/descriptors stay hand-written);
  `--verify-source` proved symbol/value parity before the swap and now self-skips,
  with `--check` + `--verify-source` wired into `quality`. Opcodes are
  endpoint-scoped, so value collisions across subsystems are expected (0x223,
  0x2A3 documented in the name table).
- [x] Phase 3b (opcodes, consolidation): the scattered opcode definitions are
  folded into `abi/opcodes.yaml` (now 193 opcodes / 21 subsystems) and the drift
  retired — `rtc_ipc.h` (was triplicated across kernel/libc/libsys),
  `font_ipc.h`, `gfx_ipc.h` (was duplicated kernel/libc), and the `serial.c`
  0x500 driver opcodes now all `#include` the generated `wasmos_opcodes.h`
  instead of hand-defining enums; their `*_STATUS_*`/structs stay hand-written.
  Per-language opcode constants generated (`abi/generated/{rust,go,zig,
  assemblyscript}/wasmos_opcodes.*`). `font`/`gfx` are modeled as their own
  subsystems (distinct services/endpoints), so their range reuse (`gfx`+`pm` at
  0x200, `font`+`netdrv` at 0xA00) is faithful endpoint-scoping; the diagnostic
  lookup is therefore **subsystem-scoped** — `wasmos_opcode_name(subsystem_id,
  type)`. NOTE: the earlier "font 0xA00 vs 0xA000" bug was a misread —
  `font_service.zig` `REQ_BASE 0xA000` is the request-id counter seed, not an
  opcode base; font clients and server agree on 0xA00. Not force-migrated: the
  pre-existing hand-rolled per-language subsets (libc Go/Zig/AS/Rust FS
  constants, the `.ts` driver literals) — they can adopt the generated files
  incrementally. Deferred: `gfx_ipc.h`'s struct layouts still differ kernel-vs-libc
  (96 lines) — a separate (non-opcode) consolidation.
- [~] Phase 3c (optional, PoC landed): typed request/reply client stubs. An
  optional `rpc:` block on a request opcode in `abi/opcodes.yaml` (`reply`/`error`
  opcodes + `request`/`reply_args` arg-word names) drives generation of a typed
  reply struct + reply-status decoder + a stub. Proven on the `rtc` read/set
  family in two shapes: a C **future**-returning stub over the libsys ipc-future
  bridge (`abi/generated/c/wasmos_rpc_wasm.h`, `wasmos_rpc_<op>()` returns a
  `wasmos_future_t*`) and an AS **synchronous** stub over `ipc.call`
  (`abi/generated/assemblyscript/wasmos_rpc.ts`, mirroring the idiom `date.ts`
  uses). Both compile-verified against the real runtime APIs; wired into `--check`.
  Remaining if pursued: model bit-packed args (RTC packs time into arg words —
  the stub currently exposes raw arg words, packing stays hand-written), the
  transfer-buffer borrow/release ownership contract for payload-carrying opcodes,
  a native (`wasmos_sys_native_*`) flavor, and adoption (e.g. migrate `date.ts`
  onto `rtcIpcRead`). Roll out to more opcode families only if the ergonomics pay
  off — it is the optional tail of the ABI effort.
- [~] Phase 4 (after 2 and 3): migrate the tree onto the packed error model, one
  subsystem at a time. **Real scope (measured 2026-08-03, larger than the earlier
  "~24" estimate):** ~413 legacy refs (`PROC_PM_ERR_` 188, `FS_ERR_` 111,
  `PROC_SPAWN_ERR_` 89, `SHMEM_ERR_` 25) + **438 bare `return -1;`** in
  services/drivers. Transport convention established (doc 34, Result
  representation): a value-or-error host call returns the datum (`>= 0`) or a
  packed code, which is **negative** (`WASMOS_ERR_MAKE(domain, code)`); IPC replies carry
  the `{flags, chain}` block. Provenance wraps only at deliberate abstraction
  seams.
  - [x] Subsystem 1 — **SHMEM** (host calls): `SHMEM_ERR_*` (-30 range) →
    packed `WASMOS_ERR_SHMEM_*` in both `link.c`/`link.cpp` wrappers;
    legacy defs removed; no consumer decoded the specific reason so the wire
    change is safe. Booted both runtimes.
  - [x] Subsystem 2 — **FS**: `FS_ERR_*` deleted; all 91 FAT-backend/relay sites
    now use packed `WASMOS_ERR_FS_*` directly (no compat/alias layer — we
    own every caller). `driver_abi.h` includes `wasmos_status.h`; wire =
    the packed code in `FS_IPC_ERROR`/`RESP` arg0, transparent to fs-manager/libc
    (they only test `< 0`). Booted both runtimes.
  - [x] Subsystem 3 — **PROC**: both `PROC_SPAWN_ERR_*` (domain 1) and
    `PROC_PM_ERR_*` (domain 2) enums deleted; all 239 sites across the kernel
    (process_manager_spawn/services, selftest) + services (cli, init, broker) now
    use packed `WASMOS_ERR_PROC_{SPAWN,PM}_*` directly (returned as the
    negative rc in `PROC_IPC_ERROR.arg1`). Booted both runtimes.
  - [x] Subsystem 4 — **scoped boundary pass**: every bare `-1` that leaves a
    service in an IPC reply code arg now carries a specific packed code. This
    covers *bare* `-1`s only — the named negative-int taxonomies are subsystem 5
    below, so the edges are not yet uniformly on the packed model. Scope was
    "boundary-crossing returns only", not the full 438-site sweep;
    internal-helper `-1`s stay and the `-1` lint stays advisory. New vocabulary:
    8 specific `fs` codes (`NOT_AUTHORIZED`, `NO_CLIENT_SLOT`, `NOT_ABSOLUTE`,
    `NO_BACKEND`, `REBORROW`, `BACKEND_IPC`, `BAD_FD`, `REPLY_SEND`), the `gfx`
    domain populated, and new `vt` (8) / `chardev` (9) / `devmgr` (10) domains.
    No **generic** fs code was added — an unspecific packed code is `-1` with
    extra steps, so each site names its actual failure instead. Three structural
    findings drove the shape:
    - Six `-1`s covered ORed conditions with unrelated causes and had to be split
      before they could be named (e.g. `handle_read_path_req`'s
      `backend < 0 || open_path_len <= 0 || xfer_buffer_write() != 0`).
    - `handle_read_path_req` was **discarding** the backend's own reason: it
      replaced a specific `WASMOS_ERR_FS_*` from the OPEN/READ leg with `-1`. It
      now relays that code and only mints `BACKEND_IPC` when `forward_request`
      itself failed at the transport level. The main dispatch loop already
      relayed backend replies verbatim, so its error path is transport-only.
    - The `open0`/`read0`/`close0`/`r0` reply defaults no longer reach a client:
      every path now selects a specific code, so those `-1`s are purely internal
      "unset" markers.
    Also migrated: `fs_init` (client-slot exhaustion), `device_manager` (the
    unsupported-query path was leaking the request `type` as its code arg; now
    `DEVMGR_UNSUPPORTED_QUERY` with `type` echoed in arg1), and both framebuffer
    drivers (where the ad-hoc `-3` meant *two different things* across the two
    drivers — `NO_RUNTIME_MODES` vs `MODE_TOO_LARGE`). `net`, `font`, `block`,
    `virtio_serial` and `hrng` needed no domains: their `-1`s are all internal
    helper returns. Booted both runtimes.
  - [x] Subsystem 5 — the **named** negative-int status vocabularies. All seven
    migrated onto packed domains: `XFER_BUFFER_ERR_*` (254 refs, host-call edge)
    -> `xfer_buffer` (11); `NET_STATUS_*` (117) -> `net` (5, was reserved);
    `GFX_STATUS_*` (112) -> appended to `gfx` (6); `FONT_STATUS_*` (61) ->
    `font` (12); `RTC_STATUS_*` (27) -> `rtc` (13); `HRNG_STATUS_*` (17) ->
    `hrng` (14); `VT_SWITCH_ERR_*` (10) -> appended to `vt` (8), reusing
    `BAD_TTY_ID` for its `INVALID_TTY` rather than duplicating it. Each legacy
    `*_OK` became `WASMOS_ERR_NONE` (both 0, so comparisons held). Notes worth
    keeping: `HRNG_STATUS_*`'s value gaps were an alignment to `NET_STATUS_*`'s
    numbering, which namespaced domains make unnecessary; `abi/hostcalls.yaml`
    and `abi/opcodes.yaml` documented the old names in prose and were updated so
    all three re-gen guards stay clean.
- [ ] [CLEANUP] Unify the transport axis: `IPC_ERR_INVALID/PERM/FULL` in
  `src/kernel/include/ipc.h` duplicates `wasmos_status_t`'s `INVAL`/`DENIED`/
  `FULL` at the same values, and the same three names are redeclared in
  `fs_fat/fat_types.h` and `services/vt/vt_types.h`. Replace them with the
  generated transport constants. Out of the subsystem-5 scope (that pass covered
  the domain axis), but it is the last duplicated status vocabulary.
  Deliberately left alone: `PM_SPAWN_INTERNAL_ERR_*` (internal by name) and
  `WAMOS_SCRIPT_ERR_*` / `SCRIPT_BROKER_ERR_*`, which are service startup/exit
  statuses returned from `initialize()`, not IPC reply codes.
- [ ] [ENHANCEMENT] Extend the `quality` re-gen guard to the host-call and opcode generators
  as they land (the errors guard already exists), so generated output can never
  silently drift from the IDL.
- [ ] [ENHANCEMENT] Widen the advisory `-1` lint: it greps for a literal `return -1;`, which is
  why both the `fs_init` reply-code default and every named `*_STATUS_-1` above
  escaped subsystem 4. It should also flag a reply code arg that is a variable
  reaching `-1`, and any negative-int status enum defined outside
  `abi/errors.yaml`.


- [ ] [BUG] Read startup values from the spawn-info buffer in the Rust and Go libc
  ports. `pm_apply_entry_bindings` always passes zeros in the four `wasmos_main`
  entry-arg registers, and both ports still return those registers, so
  `startup::arg(0)`/`startup.Arg(0)` yield 0, a guest cannot obtain its PM
  endpoint, and `main` receives an empty argv. C, Zig and AssemblyScript already
  read spawn-info (`src/libc/rust/wasmos.rs:93`, `src/libc/go/wasmos.go:374`
  `FIXME(spawn-info)`).
- [ ] [BUG] Match replies by request id in the Zig, Rust and Go `ipc.call`. All three
  return the first message arriving on the shared managed reply endpoint; C
  matches both request id and source, AssemblyScript matches request id. A
  context with two requests in flight can be handed the wrong reply.
- [ ] [BUG] Add `//go:linkname` beside `//go:extern` on `wasmFutureThen` and the four
  `wasmIPCFuture*` declarations. Without both, the symbols stay undefined at
  link, so `Future.Then` and the `IPCFuture` methods fail to link for any Go
  guest that uses them (`src/libc/go/coroutine.go:113`
  `FIXME(go-extern-linkname)`).
- [ ] [BUG] Zero the whole `spawn_info` record when the magic check fails. Only
  `magic` is cleared, so the accessors can return buffer garbage rather than the
  documented zeros (`src/libc/src/spawn_info.c`).
- [ ] [BUG] Replace the remaining bare `-1`/ad-hoc integers that cross a subsystem
  boundary with packed `abi/errors.yaml` codes: `PROC_IPC_ERROR` arg1 in
  `src/kernel/process_manager_spawn.c`, the PM spawn retry match in
  `src/kernel/kernel_init_runtime.c`, `x86_irq_configure` (guest-reachable via
  the `irq_configure` host call, `src/kernel/arch/x86_64/irq_x86_64.c`),
  `framebuffer_map_high`/`framebuffer_fill`, `native_ipc_future_status`
  (`src/libsys/native/ipc_future_native.c`), the FAT zero-copy submit path
  (`src/drivers/fs_fat/fat_block.c:108` `FIXME`), `virtio_serial` (-2/-22/-38),
  the AssemblyScript serial/keyboard/mouse reply statuses, `ata.c`'s 1..5 status
  ints, and `chardev_server.c`.
- [ ] [DOCS] Correct `BLOCK_ABOVE_4G`'s documented threshold. `abi/errors.yaml:335`
  says "above 4 GiB"; `block_buffer_check_phys` rejects at 2 GiB
  (`0x80000000`).
- [ ] [BUG] Reconcile the wrong-owner code in `socket.c`: bind/connect/listen return
  `WASMOS_ERR_NET_INVALID` while close returns `WASMOS_ERR_NET_DENIED`
  (`src/services/net_stack/socket.c:22`).
- [ ] [CLEANUP] Resolve the duplicate mutex API. `src/libsys/wasm/include/wasmos/mutex.h`
  and `src/libc/include/wasmos/mutex.h` define the same type and functions under
  different include guards; including both in one TU is a redefinition error.
- [ ] [BUG] Correct or replace `logf`'s accuracy claim and implementation. The
  coefficients are the truncated `ln(1+t)` series, not a minimax polynomial, and
  the error reaches ~0.08 near `m -> 2`, which `powf`'s fractional-exponent path
  inherits (`src/libc/src/math.c`).

## Filesystems and Storage
- [ ] [BUG] Fix `test_exec_fs_write_smoke`, the last failing test in the QEMU
  integration suite (run 31081191205, job 92550164406 — everything else in that
  suite is green, as are all four boot configs and the kernel unit tests). The app
  never prints `fs-write-smoke: ok`; the session tail shows a kernel fault:

      =000000000000000d          vector 0x0d = #GP
      [cpu] err=0000000000000000
      [cpu] rip=ff1a2233ff1a2233
      [cpu] cs=0000000000000008   kernel CS

  The faulting address identifies the corruption source. `0xff1a2233ff1a2233` is
  **non-canonical** (bits 63:48 = 0xff1a, neither all-zero nor all-one), which is
  exactly why the CPU raised #GP with `err=0`: a control transfer to a corrupted
  64-bit address. The value is the 32-bit pattern `0xFF1A2233` duplicated — and
  that is the **menu-bar background colour** (`src/libui/include/wasmos/libui.h:1276`
  `mbroot->bg_color`, also `examples/c/menu_bar/menu_bar.c:359,387`). A 32-bit ARGB
  fill writing consecutive words produces precisely this doubled pattern.

  So this is not a filesystem bug: a menu-bar background fill is writing over kernel
  memory that holds a code pointer or return address, and control later transfers to
  it. `fs_write_smoke` is the victim, not the cause. It is not a stack canary
  (those are 0xC0DEC0DEF00DFACE / 0xCAFEBABEDEADC0DE / 0xC0FFEE0DD15EA5E).

  Reproduces only in CI. The suite builds `warp_smp_defconfig` under TCG, which is
  MTTCG (one host thread per vCPU) on the Linux runner, whereas an x86_64 guest on
  an Apple Silicon host forces `thread=single` and serialises vCPUs — the same
  asymmetry that hid the wake/block race fixed in c5dcab1eb3. Passes 2/2 locally on
  the identical config. Prime suspect is therefore a concurrency window in the
  gfx/window buffer mapping under WARP, which runs guests in ring 0 and so has no
  hardware backstop against a guest write landing in kernel memory (see
  `architecture/11` ring-3 work, and the shmem/linmem aliasing bug class).

- [ ] [FEATURE] Implement FAT32 cluster read/write in the FAT-table layer: FAT32 is
  detected at mount (`fat_geom.c:92`) but `fat_fatent_read`/`fat_fatent_write`
  return `FS_ERR_CORRUPT` and `fat_chain_next` decodes only FAT12/16 and stores
  clusters as `uint16_t` (truncation) (`src/drivers/fs_fat/fat_alloc.c:43-44,83-84,148-154`).
  Done when a FAT32 `/user` volume mounts and round-trips a file.
- [ ] [ENHANCEMENT] Apply the non-blocking reactor model to `fs-init` (currently a blocking
  dispatcher with no SEEK/STAT — `src/drivers/fs_init/fs_init.c:498-569`) and
  preserve the transfer-buffer ownership contract through all VFS relay paths.
- [ ] [ENHANCEMENT] Re-enable ATA bus-master DMA under the owner-push ABI: `ata_dma_prepare`
  is stubbed to `WASMOS_DMA_STATUS_DENY` so every op is PIO
  (`src/drivers/ata/ata.c:248-264`). Carry the client `borrow_id` in the block
  IPC and map via `dma_map_borrow`, then drive the PRDT/descriptor path.
- [ ] [FEATURE] Complete initfs zero-copy mapping with an explicit entry-offset ABI and
  correct revoke/lifetime behavior (still copy-based today).
- [ ] [ENHANCEMENT] Extend LFN creation beyond ASCII: new-file LFN entries currently store
  `?`-mapped ASCII, not UTF-16 (`src/drivers/fs_fat/fat_name.c:175`; read-side
  LFN already works).
- [ ] [BUG] Fix FAT12/16 `..` self-reference and cross-cluster-boundary parent
  assumptions (`src/drivers/fs_fat/fat_dir.c:771,775`).
- [ ] [ENHANCEMENT] Port the reactor open-file table into `fat_file` so `fat_dir` reads it
  there rather than the stubbed path (`src/drivers/fs_fat/fat_dir.c:339`,
  `fat_dir.h:54`).
- [ ] [ENHANCEMENT] Refetch fs-manager boot metadata out-of-band (push/idle-step) to remove
  the nested synchronous `DEVMGR_QUERY_MOUNT_REQ` deadlock hazard during
  class discovery (`src/services/fs_manager/fs_manager.c:608`).
- [ ] [BUG] Guard FAT file-capacity growth against `uint32_t` overflow and reject
  writes not representable by the on-disk/open-file size fields.
- [ ] [TEST] Expand FAT coverage deliberately: FAT32 update modes and behavioral tests
  for each added contract.
- [ ] [FEATURE] Evaluate additional filesystems and dynamic mount lifecycle only after the
  existing VFS/backends have clear mount, ownership, and recovery semantics.


- [ ] [BUG] Serve FAT32 or refuse the mount. `fat_parse_boot` detects it, then every
  `fat_fatent_read/write` fails `WASMOS_ERR_FS_CORRUPT` and
  `fat_end_of_chain_marker` returns 0, so the volume mounts unusable
  (`src/drivers/fs_fat/fat_geom.c:94` `TODO`).
- [ ] [BUG] Honour `mnt->bytes_per_sector` in `fat_block_start`. The transfer is fixed
  at `FAT_SECTOR_SIZE` (512) while `fat_parse_boot` accepts 1024/2048/4096 and
  the FAT/dir code then parses `bytes_per_sector` bytes out of the staged
  sector, silently truncating (`src/drivers/fs_fat/fat_block.c:58` `TODO`).
- [ ] [BUG] Resolve `..` against the on-disk parent. `fat_resolve_path`,
  `fat_resolve_parent_dir` and `fat_chdir_next_component` all reset to the root
  region, so `a/b/../c` resolves against the root
  (`src/drivers/fs_fat/fat_dir.c`).

## Device Drivers and Input

Source: `architecture/16-device-manager-and-bus-enumeration.md`,
`architecture/17-console-io-and-character-device.md`, and
`architecture/21-virtual-input-testing-via-virtio-serial.md`.

- [ ] [FEATURE] Implement virtio-serial queue setup plus data/control-plane byte-stream
  IPC; discovery/register access alone cannot transport host data
  (`src/drivers/virtio_serial/virtio_serial.c:160` `TODO(virtio-serial-transport)`).
- [ ] [FEATURE] Build the `virt-input` service and host bridge after virtio-serial data
  transport exists; inject keyboard/mouse events through the normal compositor
  IPC path and add sequential QEMU UI automation tests (no source exists yet;
  `architecture/21`).
- [ ] [FEATURE] Wire parsed device-manager rules into runtime bind/unbind/mount policy;
  spawn already works but the rule actions are still informational
  (`src/services/device_manager/device_manager.c:127`).
- [ ] [ENHANCEMENT] Split a dedicated `irq.configure` capability from `irq.route` for
  level/active-low configuration (`src/services/pci_bus/linker.metadata:25`;
  kernel side `src/kernel/warp/link.cpp:2414`).
- [ ] [ENHANCEMENT] Preserve each driver module's declared IRQ capability mask in
  device-manager metadata instead of granting the fixed IRQ 14/15 pair.
- [ ] [CLEANUP] Remove the now-dead DMA-window defaulting in the individual
  `PROC_IPC_SPAWN_*_CAPS` handlers (`process_manager_spawn.c`); DMA windows are
  now installed from the driver's `dma.buffer` manifest capability
  (`capability_grant_name`), not the spawner.
- [ ] [CLEANUP] Consolidate the per-file `#define PAGE_SIZE 0x1000` copies
  (physmem.c/process.c/memory.c/native_driver.c/capability.c/…) into one shared
  header.
- [ ] [ENHANCEMENT] Replace the fixed `DEVMGR_RULE_TEXT_CAP` rules-file read buffer (now 4096)
  with an `FS_IPC_STAT_REQ`-sized (or streaming/chunked) read so the rules file
  has no size limit; a fixed buffer silently truncates trailing rules.
- [ ] [FEATURE] Add hotplug/event publication and future bus providers (USB/virtual)
  through the normalized device-record contract.


- [ ] [BUG] Correct 12-hour RTC decoding. The noon and midnight hours are off by 12 in
  both the BCD and binary branches; unreached while register B reports 24-hour
  mode (`src/drivers/rtc/rtc.ts:218` `FIXME`).
- [ ] [BUG] Add the producer-lapped snap-forward to the PCI framebuffer's console-ring
  drain. The UEFI driver performs `rp = wp - cap`; this copy does not, so an
  overrun leaves it reading overwritten bytes
  (`src/drivers/framebuffer_pci/framebuffer_pci_native.c:237` `FIXME`).

## Networking

Source: `architecture/22-networking-virtio-net-and-stack.md`.

Baseline done and e2e-validated: virtio-net driver, netif bring-up, UDP,
TCP client (connect/stream/close) and server (listen/accept/echo) over rings,
DHCP/static addressing, DNS (`NET_IPC_RESOLVE`), and a verifying TLS 1.2 client.
Remaining:

- [ ] [ENHANCEMENT] Harden TCP timeout/retransmit and the close path: drive
  `sys_check_timeouts()` from a dedicated timer source rather than only the idle
  loop, with retransmit/close e2e tests (`src/services/net_stack/net_stack.c:2646`;
  STATUS `Current Gaps`).
- [ ] [FEATURE] Add IPv6 / NDP / ICMPv6 / dual-stack, multiple addresses per interface,
  and isolated multi-stack instances (`src/services/net_stack/lwipopts.h:41`
  `LWIP_IPV6 0`).
- [ ] [FEATURE] Validate TLS certificate dates: wire an RTC time source into mbedTLS
  (`MBEDTLS_HAVE_TIME`/`HAVE_TIME_DATE`) so validity windows are checked.
- [ ] [ENHANCEMENT] Harden TLS/large-transfer RX-ring backpressure so big bodies do not stall
  on a full RX ring (app-side flow control).
- [ ] [FEATURE] Enable guest-to-guest loopback (`LWIP_NETIF_LOOPBACK` + net-stack loopback
  polling) so an in-guest server is reachable.
- [x] Migrate virtio-net to per-vq MSI-X so RX interrupts re-deliver per
  notification and the timed-poll workaround drops to a plain blocking wait.
  Done with the whole MSI platform: vectors 48–63 + ISR stubs, arch-neutral
  allocator (`msi_vectors.c`, host unit test), `msi_alloc`/`msi_free` host calls,
  the `msi` error domain, a resident pci-bus owning the capability walk and
  device programming (`PCI_IPC_MSI_*`), and `mmio_write32` for the MSI-X table
  BAR. virtio-net takes RX/TX/config vectors; virtio-rng takes one.
- [ ] [BUG] Fix the unit-test IDE target so the lint gate is green again:
  `wasmos_ide_unit` fails on `tests/unit/test_device_manager_rules.c`
  (undeclared `abort`), so it never reaches `tests/unit/test_libui_key_decode.c`,
  which then has no compile-DB entry and clang-tidy cannot find
  `wasmos/libui.h`. Adding `src/libui/include` to the target is NOT sufficient on
  its own — it pulls the project `string.h` into a hosted TU and breaks the
  target differently. See `skills/wasmos-ide-targets`.
- [ ] [FEATURE] Give `ata` real device DMA. There is no bus-master IDE (BMIDE/PRD)
  programming today, so every transfer is PIO regardless of the `dma_*`
  scaffolding. On QEMU's PIIX this means bus-master IDE; an AHCI controller
  (`ich9-ahci`) would be the better target and would also bring MSI, which
  legacy IDE cannot offer at all.
- [ ] [ENHANCEMENT] Move the resident pci-bus request loop onto the coroutine/event-loop
  runtime. It blocks (never spins), which is sufficient while every request is
  answered from config space, but hot-plug will need it to originate requests
  while serving.
- [ ] [FEATURE] PCI hot-plug: add a rescan opcode over `pci_scan_and_publish()` — small on
  its own, but knowing *when* to rescan needs an ACPI GPE/SCI path that does not
  exist yet. Depends on the coroutine-loop item above.
- [ ] [FEATURE] Support multi-message plain MSI (cap `0x05`): needs a contiguous,
  naturally-aligned vector block from the kernel (an `msi_alloc_block`), so
  pci-bus currently reports MSI as exactly one vector
  (`src/services/pci_bus/pci_bus.c`, `msi_query`). Only the MSI-X path is
  exercised in-tree — no QEMU device in the default boot offers MSI without
  MSI-X.
- [ ] [ENHANCEMENT] Steer MSI vectors at CPUs other than the BSP. Both `msi_x86_64.c`
  (`MSI_DEST_APIC_ID`) and `ioapic_program_rtes()` hardcode LAPIC 0; changing
  one without the other splits interrupt affinity across two models.
- [ ] [CLEANUP] Move the kernel notify-type space (`IPC_IRQ_EVENT_TYPE` 0xFF00,
  `IPC_MSI_EVENT_TYPE` 0xFF01) into `abi/opcodes.yaml`. Both are currently
  hand-mirrored into each driver source instead of generated.
- [ ] [FEATURE] Define the net owner-push wire protocol so TX/RX carry an explicit
  client `buffer_id`/grant instead of overloading `msg.arg0`/`arg1`
  (`src/drivers/virtio_net/virtio_net.c:479,718,752,775,900` `FIXME(owner-push)`).
- [ ] [FEATURE] Add a multi-interface ifcfg selector so boot config can target non-default
  interfaces (`src/services/net_stack/net_stack.c:977` `FIXME(multinet-ifcfg)`).
- [ ] [ENHANCEMENT] Evaluate a packet DMA fast path that removes the RX copy (only after the
  negative-path/restart/link-down test coverage lands).
- [ ] [ENHANCEMENT] Minor: expand the net-stack lwIP diagnostic `vprintf`
  (`net_stack.c:316`) and source net-stack's clock directly from a native
  driver-api millisecond hook (`src/services/net_stack/port.c:34`).

## Graphics, VT, and User Space

Source: `architecture/19-virtual-terminal.md`,
`architecture/20-graphics-framebuffer-and-compositor.md`,
`architecture/23-cli-and-user-space.md`, and
`architecture/24-environment-scopes-and-inheritance.md`.

VT I/O-multiplexer phase 5 (remaining; phases 0–4 shipped):

- [ ] [ENHANCEMENT] Make vt-1 default-visible at boot (`src/services/vt/vt_main.c:21`,
  `g_active_tty = 0`).
- [ ] [CLEANUP] Retire the framebuffer-PCI console-ring drain so the framebuffer is a pure
  blit surface (`src/drivers/framebuffer_pci/framebuffer_pci_native.c:41,233,336`).
- [ ] [FEATURE] Add the serial-bound-slot selector (`VT_IPC_BIND_SERIAL_REQ`, undefined;
  `g_serial_tty` is fixed at 1 — `vt_main.c:24`).
- [ ] [FEATURE] Lazy per-slot CLI spawn: have the VT spawn `cli.wap` pinned to a slot on
  first switch and drop `start cli.wap` from `sysinit.rc`.

Other graphics/VT/UI:

- [ ] [BUG] Reclaim old libui font shared-memory objects when text buffers grow
  (`src/libui/include/libui.h:471`) and the compositor's title-glyph shmem IDs
  on growth (`src/services/gfx_compositor/gfx_compositor.zig:2007`).
- [ ] [ENHANCEMENT] Make `libui`, font-service, and compositor allocation/index arithmetic
  overflow-safe: validate dimensions before multiplication, compute buffer
  indexes in `usize`, and cap growth before capacity doubling.
- [ ] [FEATURE] Add an explicit mode-set / update-framebuffer-info path before the VBE
  reprogram after ExitBootServices
  (`src/drivers/framebuffer/framebuffer_native.c:159`,
  `src/drivers/framebuffer_pci/framebuffer_pci_native.c:275`).
- [ ] [ENHANCEMENT] Make VT cell/reply/replay writes robust under framebuffer backpressure;
  they are best-effort today and drop on persistent queue-full
  (`src/services/vt/vt_main.c:244,287,892`).
- [ ] [BUG] Reproduce and fix the deferred rapid-TTY-switch prompt
  duplication/misalignment (`src/services/vt/vt_main.c:1755`); keep deferred
  until a stable repro exists.
- [ ] [FEATURE] Add a real WASM link step to the AssemblyScript `libui` build (currently a
  stub — `src/libui/assemblyscript/libui.ts:133`).
- [ ] [ENHANCEMENT] Add script-engine diagnostics for unclosed `if` blocks (the EOF
  `total_depth > 0` warn path is a no-op) and preserve the documented `script`
  vs `source` environment-scope semantics.
- [ ] [FEATURE] Expand VT behavior only from an explicit compatibility need: richer ANSI,
  UTF-8, scrollback, or input APIs should each include focused behavioral tests.


- [ ] [BUG] Type-check `ui_component_set_text`. It casts `component_data` to
  `ui_text_data_t*` with no check, so setting text on a LIST_VIEW, TREE_VIEW,
  SCROLL_VIEW or MENU_BAR reinterprets that component's data
  (`src/libui/include/wasmos/libui.h:676` `FIXME`).
- [ ] [BUG] Destroy an open popup's compositor window at teardown.
  `ui_menu_item_destroy_data` releases only the shmem, leaking the window and
  its shared buffer (`src/libui/include/wasmos/libui_menu_item.h:557` `FIXME`).
- [ ] [BUG] Bound the realloc copy in the libui Zig shim by the OLD block size. A grow
  reads past the end of the old block, and past the arena for a block at its
  tail (`src/libui/zig/libui_shim.c:67` `FIXME`).
- [ ] [BUG] Add the '9' glyph to `drawDigit3x5`; the table holds 0-8 and the guard
  rejects 9 (`src/libui/assemblyscript/libui.ts`).
- [ ] [CLEANUP] Remove the no-op self-assignment `d->list.capacity = d->list.capacity`
  (`src/libui/include/wasmos/libui_dropdown.h:69` `FIXME`).
- [ ] [BUG] Flush after the tetris back-buffer blit under wasm3, or gate the app on
  WARP. `present()` writes through the mapped window, which aliases the shared
  region's own pages only under WARP; under wasm3 `shmem_map_auto` rewrites the
  process page tables while the interpreter reads linear memory through its
  kernel-side buffer, so the blit never reaches the shared pages. The app builds
  for both runtimes (`examples/rust/tetris/tetris.rs:414` `TODO`).

## Validation and Documentation

Source: `architecture/25-diagnostics-status.md`,
`architecture/26-repo-map-and-validation.md`, and
`architecture/27-python-test-framework.md`.

- [ ] [TEST] Add behavioral regression coverage with every new subsystem contract;
  reject source-text assertions.
- [ ] [TEST] Add focused stress/negative tests for TLB shootdown, service restart,
  futures cancellation, virtio-serial transport, networking link-down/restart,
  and new DMA paths.
- [ ] [TEST] Add graphical input-injection tests only after virtio-serial transport is
  usable; use distinct sockets and never run QEMU sessions in parallel.
- [ ] [ENHANCEMENT] Extend `scripts/kconfig_to_cmake.py:37` symbol map as more CMake cache
  settings migrate to Kconfig, and make `scripts/quality.sh:133` clang-tidy lint
  C++ sources (missing `--extra-arg` flags).
- [ ] [DOCS] Keep architecture documents authoritative for design, `STATUS.md` concise
  for current behavior, and this file limited to unfinished work.


- [ ] [TEST] Restore coverage for the cases whose assertions no longer reach the
  documented scenario: the `sched_timeout_arm` 0-to-1 coercion is unreachable
  through `sched_event_wait` (`tests/unit/test_sched_event.c:312` `FIXME`);
  `test_malloc_overflow` cannot distinguish overflow handling from a heap the
  harness stubs dead (`tests/unit/test_libc_stdlib.c:89` `FIXME`); the P5
  work-steal case is pinned against the opposite field from the one the
  scheduler reads (`tests/unit/test_sched_runqueue.c:1450` `FIXME`); the libui
  key-decode clamp case reaches only the lower bound; `test_ipc.c` S4 duplicates
  Q10 rather than exhausting the select table.
- [ ] [TEST] Align `stubs_native_libsys.c`'s `str_copy_bytes` with the real one. The
  stub accepts `src_len == 0` where `src/libc/src/string.c` refuses it, and
  `libsys_native.c` branches on that return, so host and target disagree
  (`tests/unit/stubs_native_libsys.c:7` `FIXME`).
- [ ] [TEST] Correct the `user_mutex_user_try_lock`/`_unlock` stub arity. The stubs take
  2 parameters; `user_mutex.h` declares 4 and `syscall.c` calls with 4. It
  compiles only because the test never includes the header
  (`tests/unit/test_syscall_ipc.c`).
- [ ] [TEST] Wire up or delete `tests/unit/include/spinlock.h`. Nothing includes it, so
  `-DWASMOS_TEST_USE_REAL_SPINLOCK_DECLS` has no effect
  (`tests/unit/include/spinlock.h:11` `TODO`).
- [ ] [TEST] Make the threading selftest's join markers reflect what they name. Three
  markers are emitted from the wait/kill result, so a broken join-after-kill
  still prints "ok" (`src/kernel/kernel_threading_selftest_runtime.c:320`
  `FIXME`).
