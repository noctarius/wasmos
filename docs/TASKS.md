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
- Every item also carries a priority tag. Priority is about consequence and
  blocking, not effort:
  - `[P0]` — something is broken right now on a default path: a gate that is
    red, a feature that cannot work at all, or a defect reachable by an
    unprivileged guest. Fix before starting anything new in that subsystem.
  - `[P1]` — memory-unsafe, a silently-unenforced security check, data loss, or
    a limitation that blocks a direction the project has already committed to.
  - `[P2]` — should be fixed; nothing is blocked on it.
  - `[P3]` — opportunistic. Do it while already in the file.
- A `[P0]`/`[P1]` `[BUG]` in a subsystem is a reason not to build on that
  subsystem yet; that is the whole point of carrying both tags.

- Prefer draining `[BUG]` before `[FEATURE]` in a subsystem you are about to
  extend: several entries here describe defects that a new caller would
  otherwise inherit.

## Kernel, Memory, and Isolation

Source: `architecture/06-memory-management.md`,
`architecture/10-capability-and-policy.md`,
`architecture/11-ring3-isolation-and-separation.md`, and
`architecture/28-smp.md`.

- [ ] [ENHANCEMENT][P2] Replace the WARP shared-linear-memory local fault retry with cross-CPU
  TLB-shootdown IPIs before any live page-table reclaim under concurrent APs
  (`src/kernel/arch/x86_64/cpu_x86_64.c:554` `TODO(smp-tlb)`,
  `src/kernel/warp/ring3_trampolines.c:166`).
- [ ] [CLEANUP][P3] Retire the `shmem` subsystem in favour of a single `xfer_buffer` sharing
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
- [ ] [FEATURE][P2] Move native `.wap` services from ring 0 to the ring-3 native execution
  path (syscall-backed `libsys_native` primitives + capability enforcement).
  Unblocks isolating `gfx-compositor`, `font-service`, and `net-stack`
  (`architecture/11`:380-414).
- [ ] [FEATURE][P2] Run the **wasm3 interpreter at CPL=3** via a ring-3 trampoline, reusing the
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
- [ ] [ENHANCEMENT][P2] Finish ring-3 hardening TODOs: drop `PML4[0]` from the kernel root once
  bootstrap no longer needs the low slot (`src/kernel/paging.c:338`
  `TODO(ring3-phase3)`); add process-local exception handling beyond the
  current `-11` kill (`src/kernel/arch/x86_64/cpu_x86_64.c:484`
  `TODO(ring3-phase5)`); handle stack allocations that can exceed the 512 MB
  higher-half window (`src/kernel/process.c:131` `TODO(ring3-phase2)`).
- [ ] [ENHANCEMENT][P2] Tighten the temporarily-broadened page-fault reason back to
  `EXEC_VIOLATION`-only once the ring-3 fault tests are stable
  (`src/kernel/arch/x86_64/cpu_x86_64.c:578`).
- [ ] [ENHANCEMENT][P2] Introduce allocation intents (`STACK`, `PGTABLE`, `DMA32`, `GENERIC`),
  then remove the low-memory DMA constraint from kernel stacks and page tables
  (`src/kernel/paging.c:31`; `architecture/06`:455-497).
- [ ] [FEATURE][P2] Decide and implement kernel reachability beyond the current 512 MB low
  window: full physmap or a bounded `kmap` cache (`architecture/06`).
- [ ] [ENHANCEMENT][P2] Route pointer-bearing syscall/IPC entry paths through
  `mm_user_range_permitted` (`src/kernel/memory.c:1204`).
- [ ] [ENHANCEMENT][P2] Replace the 64-bit-bitmap linmem-slot ceiling with a growable pool so
  concurrent linear-memory slots are bounded by memory, not 64
  (`src/kernel/linmem_slots.c:15` `TODO(linmem-pool)`).
- [ ] [ENHANCEMENT][P2] Retire `PROCESS_MAX_COUNT` (48) and `THREAD_MAX_COUNT`
  (128), moving `g_processes[]` and `g_threads[]` onto `idtable` so live processes
  and threads are bounded by memory rather than by a compile-time constant. Spawn
  currently fails outright past 48 processes (`src/kernel/process.c:42`,
  `src/kernel/thread.c:9`, `FIXME(process-list)`).

  This is a storage swap, not an identity change: `pid` and `tid` already come
  from monotonic counters (`g_next_pid`, `g_next_tid`), not from the slot index,
  so nothing outside these two files depends on an element's position. `idtable`
  already provides the needed shape -- id-keyed lookup, chunked growth, and
  `idtable_release_owner` for teardown -- and the endpoint table is precedent.

  Three things make it more than a mechanical substitution:

  - **`PROCESS_MAX_COUNT` also sizes three unrelated per-process side tables**,
    each indexed by process slot: `g_wasm_driver_registry`
    (`src/kernel/wasm_driver.c:26`), `ND_HEAP_SLOTS`
    (`src/kernel/native_driver.c:79`), and `g_syscall_ipc_call_slots`
    (`src/kernel/syscall.c:78`). Leaving these behind keeps the 48-process cap in
    disguise, so they have to become id-keyed too (or move into the process
    record).
  - **Roughly eighteen `for (i = 0; i < MAX; ++i)` scans** across `process.c`,
    `thread.c` and `wasm_driver.c` become iterations. `sched_event.c:98`
    documents its timeout sweep as O(`THREAD_MAX_COUNT`) and leans on the
    `g_sched_timeout_next` hint to stay cheap; that cost model needs re-checking
    against unbounded storage.
  - **`idtable` recycles ids after the id space wraps** (`src/kernel/idtable.c`),
    whereas the current counters are monotonic for the kernel's lifetime. Reused
    pids/tids must be checked against `process_wait` and `thread_join`, which
    park on a target id and would otherwise be satisfiable by an unrelated
    later process.
- [ ] [ENHANCEMENT][P2] Replace the global shared-region cap, make context region sizing
  configurable, and add committed/resident (RSS) memory accounting for `ps`
  (`src/kernel/process.c:2492` `TODO(memory-rss)`; `architecture/06`:507-512).
- [ ] [CLEANUP][P3] Wire the kernel-thread trampoline into PM launch policy and delete the
  legacy trampoline (`src/kernel/process.c:1503,1771`).
- [ ] [BUG][P1] Fix the `warp_ring3_dispatch` `proc_info_stats` ctx bug: the case passes
  `ctx5` (== `a4`, the `stats` param) as the kernel ctx, but a 5-param host call
  needs ctx in `a5` (R9) — there is no `ctx6`, so the hand-written case silently
  reused `ctx5`. Ring-3 `proc_info_stats` therefore gets a garbage ctx (a user
  offset) → wrong `warp_mem` resolution. The generated dispatch computes
  `ctx = a<arity>` and fixes it; fix lands when the ring-3 dispatch is swapped in.
  Found during host-call dispatch codegen (`src/kernel/warp/link.cpp:3130`).
- [ ] [FEATURE][P2] Extend DMA isolation to an IOMMU domain model (VT-d/AMD-Vi) and add
  non-coherent cache-maintenance hooks before targeting non-coherent hardware
  (`architecture/12`:88,618,625).
- [ ] [ENHANCEMENT][P2] Harden the boot and native ELF loaders with checked arithmetic for
  program-header offsets, segment file/virtual ranges, boot-info layout totals,
  and cursor advances; reject values that overflow allocation sizes or boot ABI
  fields before copying.


- [ ] [BUG][P1] Pass `MEM_REGION_FLAG_*` to `paging_map_4k`, not raw `PT_FLAG_*`. The
  callee rebuilds the PTE from its own flag space, so `PT_FLAG_PCD` is dropped
  and device scratch pages lose cache-disable, leaving their memory type to the
  MTRRs alone. Same pattern in `arch/x86_64/lapic.c` and `ioapic.c`
  (`src/kernel/mmio.c:17` `FIXME`).
- [ ] [BUG][P1] Apply the shmem/WARP physical-zone floor in `linmem_slot_commit`. It
  calls `pfa_alloc_pages` with no floor, so slot-backed linear memory falls
  outside the guarded zone that `memory.h` and `physmem.h` still describe as an
  invariant (`src/kernel/linmem_slots.c:105`).
- [ ] [BUG][P1] Serialize the capability table. Grants (spawn) and checks (hardware host
  calls) run on any CPU with no lock, so a grant that grows `g_cap_ctx` can race
  a concurrent lookup (`src/kernel/capability.c`).
- [ ] [ENHANCEMENT][P2] Validate the initfs higher-half alias. The pointer fixup assumes initfs
  lies inside the 512 MiB shared window; a higher firmware placement yields an
  unmapped pointer with no diagnostic (`src/kernel/kernel_boot_runtime.c:69`).
- [ ] [BUG][P1] Give `isr_exception_1` (#DB) the `PUSH_REGS` every other user-fault stub
  performs. Without it `x86_exception_panic_frame` reads the CPU frame where it
  expects the register block, so the #DB dump is garbage and reads past the
  frame (`src/kernel/arch/x86_64/cpu_isr.S:140` `FIXME`).
- [ ] [BUG][P1] Push a zero error code in `DECL_EXC` for the vectors that do not supply
  one. The macro passes the post-`PUSH_REGS` `%rsp` unconditionally, which the
  panic decoder reads as the error-code layout; for vectors 5, 9, 15, 16, 18-20,
  22-28 and 31 the dump prints rip as "err" and cs as "rip"
  (`src/kernel/arch/x86_64/cpu_isr.S:102` `FIXME`).
- [ ] [BUG][P1] Read the interrupted RSP from `frame[4]` for kernel faults too. In long
  mode the IRET frame always carries SS:RSP, so the current
  `(cs & 3) == 3` branch reports the IST/exception-stack address instead of the
  interrupted stack (`src/kernel/arch/x86_64/cpu_x86_64.c`,
  `x86_exception_panic_frame`).
- [ ] [BUG][P1] Correct both PCI protocol GUIDs in the bootloader. Neither matches the
  UEFI spec value, so both `ConnectController` passes are silent no-ops
  (`src/boot/boot.c:213` `FIXME`, `src/boot/uefi.h`).
- [ ] [BUG][P1] Free the previous buffer on the `EFI_BUFFER_TOO_SMALL` memory-map retry
  and on `boot_capacity` growth. Both paths reallocate without releasing, so the
  pages leak as `EFI_LOADER_DATA` and the kernel never reclaims them
  (`src/boot/boot.c:617`, `:861`).


- [ ] [BUG][P1] Free the two ring-3 trampoline pages at
  `warp_r3_teardown`. `paging_destroy_address_space` reclaims page-table
  structures only, not mapped leaf frames, so 8 KiB leaks on every WARP guest
  teardown (`src/kernel/warp/ring3_trampolines.c`).
- [ ] [BUG][P1] Free the private PD/PT frames beneath a cloned low slot.
  `paging_destroy_address_space` frees only the slot-0 PDPT frame and never
  walks below it, so every root that went through
  `paging_clone_low_slot_in_root` leaks its lower tables; the clone's own error
  paths leak earlier iterations' PDs/PTs too (`src/kernel/paging.c`).
- [ ] [BUG][P1] Serialize `x86_irq_mask`/`x86_irq_unmask`. They read-modify-write
  `g_pic_mask1/2` with no lock, while only the `irq_sharing` ops path holds
  `g_irq_lines_lock` (`src/kernel/arch/x86_64/irq_x86_64.c`).
- [ ] [BUG][P1] Recover the lower remnant in `pfa_alloc_pages_above`. When
  `g_ranges` is full the middle-split fallback front-allocates and silently
  drops `[rbase, start)` from the free list; those frames become unreachable
  and invisible to `pfa_free_bytes` (`src/kernel/physmem.c`).
- [ ] [ENHANCEMENT][P2] Pack `EFI_ADDRESS_SPACE_DESCRIPTOR`. Without
  `__attribute__((packed))` the padding disagrees with the ACPI byte stream it
  mirrors; harmless only because nothing reads it (`src/boot/uefi.h`).
- [ ] [ENHANCEMENT][P2] Give the 64 KiB boot stack a real output section.
  `linker.ld` reserves it by address arithmetic only, so no program header
  allocates or zeroes it; it works because entry.S's 2 MiB identity map happens
  to cover the range (`src/kernel/arch/x86_64/linker.ld`).
- [ ] [ENHANCEMENT][P2] Clear `bootstrap_pd_high` entries 32..511 in `_start`. Only
  the first 32 are written, unlike the three upper-level tables, so their
  contents depend on section placement (`src/kernel/arch/x86_64/entry.S`).
- [ ] [DOCS][P2] Correct `architecture/06-memory-management.md:112`: it states
  `paging_clone_low_slot_in_root` copies PML4[511]; it deep-copies PML4[0], the
  low identity slot. PML4[511] is copied by `paging_create_address_space`.

- [ ] [BUG][P1] Refuse a fault against a region with `phys_base == 0` instead of
  mapping physical page 0. `mm_handle_page_fault` computes
  `phys_base + (page_base - region->base)`, so a region whose backing was never
  assigned resolves to low physical memory and maps it into the faulting process
  (`src/kernel/memory.c`).
- [ ] [BUG][P1] Balance `mm_shared_map`'s pin in `mm_shared_unmap`. Unmap drops the
  logical reference but neither undoes the `pfa_pin_pages` that map installed nor
  tears down the PTEs; only `mm_context_release_regions` balances it, so a map/unmap
  pair leaks the pin for the life of the context (`src/kernel/memory.c`).
- [ ] [BUG][P2] Reclaim a shared region that is created and never used.
  `mm_shared_create` publishes with `refcount == 0` while reclamation happens only
  on the release/unmap paths, so a region nobody retains or maps holds its frames
  until the kernel exits (`src/kernel/memory.c`).
- [ ] [BUG][P2] Give `capability_dma_commit` a matching release, a clamp to
  `dma_max_bytes`, and an overflow check. It only ever accumulates, so a long-lived
  context's committed total drifts upward and past the window it is meant to
  enforce (`src/kernel/capability.c`).
- [ ] [ENHANCEMENT][P2] Make `kernel_boot_run_low_slot_sweep_diagnostic` behave like
  its name. It mutates every root it visits, returns nothing, and stops at the first
  failure -- leaving later processes with their low slot intact and no indication
  which ones (`src/kernel/kernel_boot_runtime.c`).
- [ ] [CLEANUP][P3] Remove the dead branch in `paging.c` whose
  `if (pt[pt_idx] & PT_FLAG_PRESENT)` arm is byte-identical to its fallthrough.
- [ ] [CLEANUP][P3] Resolve the unused serial driver-hook surface:
  `serial_set_driver`'s `put_char`/`read_char` hooks are never invoked (only
  `.init` is honoured, because `serial_transmit`/`serial_read_char` address COM1
  directly by design), and `serial_set_driver`, `serial_get_driver` and
  `serial_console_ring_ptr` have no in-tree callers (`src/kernel/serial.c`).
- [ ] [CLEANUP][P3] Remove or wire up `g_mem_service_reply_endpoint`: memory_service
  registers and stores it, and nothing ever reads it
  (`src/kernel/memory_service.c`).

## Scheduler, Threads, and IPC

Source: `architecture/07-scheduling-and-preemption.md`,
`architecture/08-threading-and-lifecycle.md`,
`architecture/09-process-and-ipc.md`, `architecture/29-threadable-scheduler.md`,
`architecture/30-ipc-direct-switch.md`, `architecture/32-coroutines-futures-promises.md`,
and `architecture/33-completion-ports.md`.

- [ ] [FEATURE][P2] Add scheduler/process observability: committed-memory-aware process
  reporting (feeds `memory-rss`), scheduler latency/stall counters, and useful
  per-process metrics.
- [ ] [FEATURE][P2] Define a fairness/budget policy now that the priority bands are actually
  wired (`pm_sched_prio_for_flags` → `process_set_main_prio`); measure first and
  keep the existing preemption and SMP regression gates.
- [ ] [FEATURE][P2] Surface futex to userspace. The kernel primitive exists (`futex_wait/wake`
  in `src/kernel/futex.c`, WASM hostcalls at `src/kernel/wasm3/link.c:3800`) but
  is absent from libc `api.h` and the native `int 0x80` path; user mutexes still
  yield-spin (`src/libsys/wasm/include/wasmos/mutex.h:44`,
  `src/libsys/native/include/wasmos/libsys_native.h:296`,
  `src/libc/include/wasmos/mutex.h:44`). Add the declarations + native syscall
  and make the user mutex consume it.
- [ ] [ENHANCEMENT][P2] Promote libsys event-loop intents into the shared future/promise contract
  with one receive pump per endpoint and request-id/generation cancellation.
- [ ] [CLEANUP][P3] Remove the legacy `process_block_on_ipc` shim once all callers move to the
  select/idle-wait path (`src/kernel/process.h:225`, `src/kernel/process.c:1588`).
- [ ] [DOCS][P2] Reconcile `architecture/30-ipc-direct-switch.md` (fully unimplemented; its
  header flags conflict with the futures direction) with the futures model, or
  formally drop it. Do not add a direct-switch API that reintroduces nested
  blocking IPC.
- [ ] [FEATURE][P2] Define a PM-mediated cooperative lifecycle-control protocol over IPC:
  capability-gated shutdown/cancel, acknowledgement, deadline-based escalation
  to `process_kill`, and event-loop safe points. No POSIX signal handlers or
  arbitrary asynchronous thread interruption.
- [ ] [FEATURE][P2] Add asynchronous, capability-gated process-death watches for supervisors
  and integrate them with the lifecycle-control event path.
- [ ] [ENHANCEMENT][P2] Normalize request-id validity across WASM and native `libsys` (signed vs
  unsigned wire representation and the reserved invalid value).
- [ ] [BUG][P1] Make console-backed libc `read`/`write` reject or chunk counts beyond the
  `int32_t` ABI limit and return the actual byte count from the console backend.
- [ ] [FEATURE][P2] Defer true WASM parallelism and hard coroutine preemption until runtime
  locking/reentrancy has a dedicated design and validation plan.
- [ ] [FEATURE][P2] Implement completion ports (`architecture/33`, design proposal only): a
  kernel-owned bounded CQ with notification doorbells and generation-tagged
  operation tokens, as a batched completion source for the future/promise
  runtime and high-rate networking.
- [ ] [FEATURE][P2] Green-thread coroutine runtime (`architecture/32` §52, spike): re-base the
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


- [ ] [BUG][P1] Make the `IPC_CALL` request-id counter atomic and lock the per-pid slot
  table. Two CPUs issuing `IPC_CALL` concurrently can mint the same id, which
  both reply correlation and the `syscall_ipc_request_id_issued` replay guard
  assume is unique (`src/kernel/syscall.c:58` `FIXME`).
- [ ] [BUG][P1] Bounds-check `line` in `irq_sharing_register/ack/unregister/dispatch/
  has_sharers`. `WASMOS_ERR_IRQ_BAD_LINE` exists but is returned only for a
  NULL table; an out-of-range line is an out-of-bounds access
  (`src/kernel/irq_sharing.c:71`, `:102`, `:144`).
- [ ] [BUG][P1] Serialize `kenv_set`/`kenv_unset`. The store has no lock, and the
  "callers run descheduled" justification does not hold on SMP: two CPUs can
  claim the same free slot (`src/kernel/include/kenv.h:23` `FIXME`).
- [ ] [BUG][P1] Release the process slot on every `process_spawn_as_internal`
  failure after the `->NEW` claim. `process_find_slot` reclaims only
  UNUSED/DEAD and there is no NEW->DEAD edge, so the slot is stranded for the
  life of the kernel, plus a leaked mm context on the later paths
  (`src/kernel/process.c` `FIXME(spawn-slot-leak)`).
- [ ] [BUG][P1] Sweep select sets on owner death. Endpoints are reclaimed via
  `idtable_release_owner`; `g_select_table` has no equivalent, so a process
  exiting without `ipc_select_destroy` leaks its select-set id and its
  per-context quota (`src/kernel/ipc.c`).
- [ ] [ENHANCEMENT][P2] Initialise `g_user_mutex_lock` with `ksync_spinlock_init`.
  It is never initialised, benign today only because all-zero storage happens
  to be a valid unlocked `spinlock_t` (`src/kernel/user_mutex.c`).
- [ ] [CLEANUP][P3] Remove `process_t::wait_event` and `process_t::wait_target_pid`.
  Nothing waits on or signals the event, and the pid field is only ever written
  0; real waiters park on `thread_t` and `process_wake_waiters` scans the thread
  table (`src/kernel/include/process.h`).
- [ ] [CLEANUP][P3] Resolve `thread_find_main_for_pid`: it returns the first slot
  with a matching `owner_pid` rather than consulting the owner's `main_tid`, so
  it is not the main thread for a multi-threaded process. Zero callers today
  (`src/kernel/thread.c`).
- [ ] [CLEANUP][P3] Retire `poll_watcher_t::user_data` (stored by `poll_struct_add`,
  never read) and either raise or remove `POLL_EV_OUT`/`POLL_EV_CLOSE`/
  `POLL_EV_KERNEL`, which are declared but never signalled
  (`src/kernel/include/poll.h`).

- [ ] [ENHANCEMENT][P2] Make `ipc_endpoint_owner_context` distinguish its outcomes.
  It returns 0 for a rejected argument, for an endpoint that does not exist, and
  for an endpoint owned by the kernel -- because `IPC_CONTEXT_KERNEL` *is* 0
  (`src/kernel/ipc.c:87`). Its only caller (`ipc_send_from`, `ipc.c:269`) is
  reached solely when the sender is not the kernel, where every one of those three
  cases must deny, so the answer is right today for reasons the function cannot
  express.

  That is the hazard rather than a live bug: a second caller, or a guard that
  learns to treat kernel-owned sources as permitted, would read 0 as "the kernel
  owns it, allow" and admit a forged source endpoint. Return the owner through an
  out-parameter with a status, as the endpoint lookups already do, so "no such
  endpoint" and "owned by the kernel" stop sharing a value.

- [ ] [CLEANUP][P1] Retire blocking IPC from app and service CALL SITES, then delete
  the blocking primitives that only those call sites use. Nothing gates the start:
  the four AS drivers (keyboard, mouse, serial, rtc) are already `@coroutine` entry
  points driven by libc's pump, and the future/promise bridge is live in net-stack
  and on the CLI's VT path. Sequenced, because the order is forced by what gates
  what:

  1. **AssemblyScript surface that is still blocking**: five `ipc_recv(...)` sites
     in `src/libc/assemblyscript/wasmos.ts`, eight `ipc.call` sites in
     `src/libui/assemblyscript/libui.ts`, three in `src/utils/date/date.ts`, and
     `examples/assemblyscript/minesweeper`. libui.ts is the one that matters: every
     AS UI app inherits its blocking behaviour, so a UI cannot service input while
     it waits on the compositor.
  2. **C**, ~36 files and not uniform. cli, fs_manager, virtio_net, pci_bus and the
     net_tcp_* examples already touch futures, so they are partial conversions; the
     utils and examples are shallow. fs_manager (6 sites), cli (5) and fs_fat (5)
     are the bulk, and three sites carry a named hazard:
     - fs_manager's nested synchronous `DEVMGR_QUERY_MOUNT_REQ` round-trip can
       deadlock (`src/services/fs_manager/fs_manager.c:608`; `architecture/09`
       synchronous-IPC section).
     - the CLI's PM/FS/spawn paths still block on
       `wasmos_ipc_select_one(g_reply_endpoint)` (`cli.c` ~1111, ~1381, ~1404,
       ~2076), and `cli_register_vt_writer` / `cli_set_vt_mode` drain the VT
       endpoint directly at init, which is why the CLI is structurally blind to
       input while a command waits (Ctrl+C during a long command cannot work).
       Converting these is the prerequisite for collapsing the CLI's two endpoints
       onto one, and then for replacing the `g_phase`
       (`INIT/PROMPT/READ/WAIT_IPC/FAILED`) machine with a coroutine per command.
       Do not collapse the endpoints first: sharing one endpoint while those
       blocking receives remain just moves the input loss into them.
     - `examples/c/menu_bar/menu_bar.c` has one blocking `wasmos_ipc_call` at line
       50 plus a `sched_yield` spin waiting for the rtc service to appear. The spin
       is the more objectionable of the two under the project's no-busy-spin rule;
       the service-discovery wait belongs on a class subscription or a bounded park.
  3. **Zig / Rust / Go**, small: font_service (a hand-rolled 50 ms x 200 poll, the
     one with a symptom to point at), gfx_compositor, `libc/rust/wasmos.rs`,
     tetris, `libc/go/wasmos.go`.
  4. **Delete the primitives** -- `ipc_select_one`/`ipc_recv` host calls,
     `wasmos_ipc_call*`, `wasmos_sys_ipc_call_native`, and the synchronous
     request/reply wrappers in libc, libsys and the native shims. Only meaningful
     after 1-3; doing it earlier is a flag day.

  **Syscall 6 (`WASMOS_SYSCALL_IPC_CALL`) is independent and can go now.** Its
  libc wrapper `wasmos_sys_ipc_call` has zero callers, and the kernel handler at
  `src/kernel/syscall.c:724` is reachable only through it. Removing it also
  removes the reply-replay surface that `syscall_ipc_request_id_issued` guards.
  Do not confuse it with `wasmos_sys_ipc_call_native`, which is the native
  vtable path and has live callers.

  **`ipc_endpoint_wait_for` stays.** A coroutine still has to park somewhere: the
  bottom of an event loop must block or the CPU spins, which is the whole point of
  the idle/yield-spin work (native drivers already call `api->ipc_wait` so the CPU
  can reach idle/hlt). So the target is not "no blocking anywhere" but exactly one
  sanctioned blocking point per component, at the bottom of its loop -- and none in
  a call site, where blocking is what makes a component unable to service anything
  else while it waits.

## Runtime, Packaging, and Service Discovery

Source: `architecture/13-runtime-and-packaging.md`,
`architecture/14-libsys-and-service-runtime.md`, and
`architecture/15-drivers-and-services.md`.

- [ ] [FEATURE][P2] Give the non-C entry shims a real `argv`. The C `crt1`
  (`src/libc/src/startup.c`) now calls `main(argc, argv)` from
  `wasmos_startup_argv()`, but the Rust, Go, Zig and AssemblyScript shims
  (`src/libc/{rust,go,zig,assemblyscript}`) still call their `main` with an empty
  argument list, so a guest in those languages must read `wasmos_startup_args`
  and tokenize it. Port the same split per language, with a guest test per
  language that passes an argument and asserts it lands at index 1.
- [ ] [FEATURE][P3] Carry the program name in the startup contract so `argv[0]` can
  be one. `wasmos_spawn_info_t` (`src/drivers/include/wasmos_spawn_info.h`) has no
  name field, so `wasmos_startup_argv` fills `argv[0]` with an empty string
  (`TODO` at `src/libc/src/spawn_info.c`). The header is versioned and
  append-only, and PM already parses the `.wap` package name; adding it means the
  header field, a `version` bump, the native `api->spawn_info` path, and the
  accessor in every language.
- [ ] [CLEANUP][P3] Retire the hand-rolled argument tokenizers now that
  `wasmos_startup_argv` exists: `src/utils/host/host.c:48` (`first_token`),
  `src/utils/curl/curl.c`, `src/utils/ip/ip.c`, and
  `src/services/wasmos_script/wamos_script.c` each split the argument string
  themselves. Each conversion is a behaviour-preserving switch to `main(argc,
  argv)` and needs its own guest check.
- [ ] [ENHANCEMENT][P2] Close the remaining WARP refinement TODOs (host-call coverage itself is
  broad): synchronise symbol lookups/alloc under SMP (`src/kernel/warp/link.cpp:90`
  `TODO(smp-warp)`, `src/kernel/warp/shim.cpp:579` `FIXME(smp-warp)`); reserve
  shmem auto-map windows against real heap growth (`link.cpp:1988`); write-combining
  PAT for framebuffer/scanout (`link.cpp:2145`); enable W^X once kernel paging
  supports per-4K remap (`src/kernel/warp/posix_kernel.c:97`). Provide a supported
  alternative to the vendored-runtime-pointer shim without modifying `libs/warp`.
- [ ] [FEATURE][P2] Make WARP multithreaded WASM either functional (locate the owner module
  bytes so VM threads run under WARP — `src/kernel/warp_driver.cpp:952`
  `module_bytes = nullptr`) or explicitly unavailable at the API boundary.
- [ ] [BUG][P1] Fix (or confirm resolved) the WARP ring-3 delegated-executor argv
  coherence bug: `wasmos_script`'s first xfer-buffer read of argv can read as
  zero because the kernel higher-half alias and the ring-3 user-VA view diverge
  for that page (wasm3 is unaffected). Verify against a repro before asserting
  fixed (`architecture/13`:549-551; STATUS known non-green path).
- [ ] [BUG][P1] Restore prior linear-memory PTEs on wasm3 `xfer_buffer`/`shmem` unmap
  instead of only dropping shared refcounts (`src/kernel/wasm3/link.c:1553,1563,2504,2552`
  `FIXME(xfer-unmap)`/`FIXME(shmem-map-auto)`).
- [ ] [ENHANCEMENT][P2] Replace the wasm3 parent-name spawn heuristic with explicit per-process
  identity (`src/kernel/wasm3/link.c:589` `FIXME`).
- [ ] [BUG][P1] Track native VM `malloc`/`free` per-PID so a reaped process's native heap
  pages are reclaimed (`src/kernel/native_driver.c:597` `TODO(nd-vm)`; native
  services are currently long-lived, so this leaks).
- [ ] [FEATURE][P2] Complete executable-broker handoff: route non-builtin subsystem handler
  kinds to a userland broker instead of returning `-1` (`src/kernel/wasmos_app.c:797`),
  ensure delegated argv/transfer-buffer reads are coherent, define failure
  handling, and add end-to-end broker tests.
- [ ] [FEATURE][P2] Finish service-class discovery lifecycle: enumeration,
  add/remove/death notifications, capability-gated registration, and consumer
  migration where class lookup removes hardwired provider names.
- [ ] [FEATURE][P2] Add driver/service supervision, restart/reincarnation, and controlled
  capability revoke/reissue on restart.


- [ ] [BUG][P1] Bound `argc` in `wasm_driver_call` before marshalling. The wasm3 backend
  builds a fixed 4-slot argument array and passes `argc` straight to `m3_Call`;
  `wasm_driver_call_entry` checks, this path does not
  (`src/kernel/include/wasm_driver.h:96` `FIXME`).
- [ ] [CLEANUP][P3] Retire or build `wasm_chardev.c`. It is absent from the kernel source
  list, nothing calls its API, and no rule emits the
  `_binary_chardev_server_wasm_*` symbols it references
  (`src/kernel/wasm_chardev.c:8` `FIXME(chardev-dead)`).
- [ ] [BUG][P1] Fix `freeAlignedMemory` in the AOT host shim. It reads a size header at
  `ptr - sizeof(size_t)` that `allocAlignedMemory` never writes, so it faults or
  `munmap`s a garbage length (`src/tools/warp_aot/host_mem_utils.cpp:141`).


- [ ] [BUG][P1] Replace the WARP C++ compat stubs that return silently wrong values
  with either a correct implementation or a hard failure. Each compiles and
  links, produces no diagnostic, and yields a wrong answer:
  `numeric_limits<T>::max()` returns 0 for an unspecialised T
  (`compat/limits`); `std::function::operator()` loops forever
  (`compat/functional`); `sstream::str()` always returns `""`; `compat/mutex`
  locks nothing; `is_convertible` maps to `__is_constructible`, making
  `FunctionRef`'s signature constraint unconditional (`compat/type_traits`);
  `mprotect` no-ops returning 0, so a W^X request appears to succeed, and
  `posix_memalign` ignores its alignment (`warp/posix_kernel.c`).
- [ ] [BUG][P1] Resolve the `std::terminate` ODR violation: an inline definition in
  `warp/compat/exception` and an out-of-line one in `warp/cxx_abi.cpp`. It
  links, but which one runs depends on whether the translation unit saw the
  compat header.
- [ ] [BUG][P1] Make `realloc(ptr, 0)` free and return NULL in the wasm3 shim. It
  currently returns 0 without freeing -- neither the C contract nor
  leak-free (`src/kernel/wasm3/shim.c`).
- [ ] [ENHANCEMENT][P2] Reconcile the wasm3/WARP behavioural divergences a guest can
  observe, or document them as ABI: physical allocation floor for
  `block_buffer_phys` (512 MiB under WARP vs 2 GiB under wasm3); an
  out-of-linear-memory guest pointer traps the module under wasm3 but returns
  `BAD_POINTER` under WARP; `sched_ready_count` returns 0 under WARP rather than
  the live count; `wasi.random_get` fills zeros; `env.strlen` is wasm3-only;
  `wasm3_runtime_enter` disables preemption for the whole call while
  `warp_runtime_enter` does not.

## ABI, Code Generation, and Error Handling

Source: `architecture/34-abi-idl-and-error-model.md`.

The IDL surfaces (`abi/errors.yaml`, `abi/hostcalls.yaml`, `abi/opcodes.yaml`,
`abi/constants.yaml`) and their generators are in place, swapped into the kernel,
both runtimes, the AOT tool and all five guest languages, and guarded by the
`quality` re-gen check; `STATUS.md` records that baseline. What remains is the
tail.

- [ ] [ENHANCEMENT][P3] Extend the typed request/reply stub generator (an optional `rpc:`
  block on a request opcode in `abi/opcodes.yaml`) beyond the `rtc` proof of
  concept, which generates a C future-returning stub (`wasmos_rpc_wasm.h`) and an
  AS synchronous one (`wasmos_rpc.ts`). Needed before it can carry more opcode
  families: model bit-packed args (`rtc` packs a time into arg words, so the stub
  exposes raw arg words and the packing stays hand-written), define the
  transfer-buffer borrow/release ownership contract for payload-carrying opcodes,
  add a native (`wasmos_sys_native_*`) flavour, and adopt it at a real call site
  (`src/utils/date/date.ts` onto `rtcIpcRead`). Roll out only if the ergonomics
  pay off — this is the optional tail of the ABI effort.
- [ ] [CLEANUP][P3] Unify the transport axis: `IPC_ERR_INVALID/PERM/FULL` in
  `src/kernel/include/ipc.h` duplicates `wasmos_status_t`'s `INVAL`/`DENIED`/
  `FULL` at the same values, and the same three names are redeclared in
  `fs_fat/fat_types.h` and `services/vt/vt_types.h`. Replace them with the
  generated transport constants. The packed migration covered the domain axis;
  this is the last duplicated status vocabulary. Deliberately left alone:
  `PM_SPAWN_INTERNAL_ERR_*` (internal by name) and `WAMOS_SCRIPT_ERR_*` /
  `SCRIPT_BROKER_ERR_*`, which are service startup/exit statuses returned from
  `initialize()`, not IPC reply codes.
- [ ] [BUG][P2] Make the ABI re-gen check fail loudly when PyYAML is missing rather
  than skipping. `scripts/gen_abi_*.py --check` prints
  `skipping (PyYAML is required ...)` and exits 0, so on a machine without
  PyYAML `cmake --build build --target lint` reports success while verifying
  none of the generated ABI. That is not hypothetical: a `clang-format -i` sweep
  re-wrapped `abi/generated/c/wasmos_imports.h` and `wasmos_opcodes.h`, the
  local gate passed, and CI caught it only because its runners have PyYAML.

  A skipped check should be visible in the gate's exit status, or the gate
  should refuse to run without the dependency. Related: generated files are
  reformattable at all, so a formatter sweep can silently desynchronise them
  from their generator -- excluding `abi/generated/` from the format pass would
  remove the hazard at its source.

- [ ] [ENHANCEMENT][P2] Widen the advisory `-1` lint: it greps for a literal `return -1;`,
  which is why the `fs_init` reply-code default and the named `*_STATUS_` `-1`
  values were invisible to it. It should also flag a reply code arg that is a
  variable reaching `-1`, and any negative-int status enum defined outside
  `abi/errors.yaml`.


- [ ] [BUG][P1] Match replies by request id in the Zig, Rust and Go `ipc.call`. All three
  return the first message arriving on the shared managed reply endpoint; C
  matches both request id and source, AssemblyScript matches request id. A
  context with two requests in flight can be handed the wrong reply.
- [ ] [BUG][P1] Add `//go:linkname` beside `//go:extern` on `wasmFutureThen` and the four
  `wasmIPCFuture*` declarations. Without both, the symbols stay undefined at
  link, so `Future.Then` and the `IPCFuture` methods fail to link for any Go
  guest that uses them (`src/libc/go/coroutine.go:113`
  `FIXME(go-extern-linkname)`).
- [ ] [BUG][P1] Replace the remaining bare `-1`/ad-hoc integers that cross a subsystem
  boundary with packed `abi/errors.yaml` codes: `PROC_IPC_ERROR` arg1 in
  `src/kernel/process_manager_spawn.c`, the PM spawn retry match in
  `src/kernel/kernel_init_runtime.c`, `x86_irq_configure` (guest-reachable via
  the `irq_configure` host call, `src/kernel/arch/x86_64/irq_x86_64.c`),
  `framebuffer_map_high`/`framebuffer_fill`, `native_ipc_future_status`
  (`src/libsys/native/ipc_future_native.c`), the FAT zero-copy submit path
  (`src/drivers/fs_fat/fat_block.c:108` `FIXME`), `virtio_serial` (-2/-22/-38),
  the AssemblyScript serial/keyboard/mouse reply statuses, `ata.c`'s 1..5 status
  ints, and `chardev_server.c`.
- [ ] [DOCS][P2] Correct `BLOCK_ABOVE_4G`'s documented threshold. `abi/errors.yaml:335`
  says "above 4 GiB"; `block_buffer_check_phys` rejects at 2 GiB
  (`0x80000000`).
- [ ] [BUG][P1] Reconcile the wrong-owner code in `socket.c`: bind/connect/listen return
  `WASMOS_ERR_NET_INVALID` while close returns `WASMOS_ERR_NET_DENIED`
  (`src/services/net_stack/socket.c:22`).
- [ ] [CLEANUP][P3] Resolve the duplicate mutex API. `src/libsys/wasm/include/wasmos/mutex.h`
  and `src/libc/include/wasmos/mutex.h` define the same type and functions under
  different include guards; including both in one TU is a redefinition error.
- [ ] [BUG][P1] Correct or replace `logf`'s accuracy claim and implementation. The
  coefficients are the truncated `ln(1+t)` series, not a minimax polynomial, and
  the error reaches ~0.08 near `m -> 2`, which `powf`'s fractional-exponent path
  inherits (`src/libc/src/math.c`).


- [ ] [BUG][P1] Align the wasm and native coroutine completion polarity. A wasm task
  returning a non-zero, non-yield status REJECTS its completion future, so
  `join()` yields that negative status; `wasmos_native_coroutine_exit()` always
  RESOLVES, so `join()` yields 0 and the value lands in `out_result`. Same API
  name, opposite failure signalling.
- [ ] [BUG][P1] Guard group reuse and partial registration in the wasm coroutine
  runtime. `future_group` does not test `group->active` before overwriting the
  record (native does), orphaning continuations that still point at it; and a
  registration that fails part-way returns NULL with some continuations already
  linked and `active == false`, a half-built state the caller cannot detect
  (`src/libsys/wasm/coroutine_wasm.c`).
- [ ] [ENHANCEMENT][P2] Reconcile the remaining wasm/native `libsys` divergences:
  `intent_send` returns -1 on wasm but the raw `ipc_send` status on native (a
  caller testing `== -1` misses native failures); `event_loop_poll` never
  returns negative on wasm but returns -1 on native, discarding the count
  already dispatched; `HANDLER_MAX` is 16 vs 24; "no endpoint" is a negative
  value vs `0xFFFFFFFF`.
- [ ] [ENHANCEMENT][P2] Add a `wasmos_sys_event_loop_destroy`. `event_loop_init`
  creates a select set that is never released, so a transient loop leaks it
  (`src/libsys/wasm/include/wasmos/libsys.h`).
- [ ] [DOCS][P2] Correct `ipc_last_field` in `abi/hostcalls.yaml` (~line 132): it
  documents -1 for both "no message stored" and "field out of range". Both
  shims return `IPC_ERR_NOENT` (-4) for the former and `IPC_ERR_INVALID` (-1)
  only for the latter (`src/kernel/warp/link_ipc.cpp:209,236`,
  `src/kernel/wasm3/link_ipc.c:296,317`). The generated C header inherits the
  wrong text.
- [ ] [DOCS][P2] Document the argument layouts missing from `abi/opcodes.yaml`:
  `FONT_IPC_*` and `PCI_IPC_MSI_*` have none at all, unlike the vt/gfx
  families; `VT_IPC_WRITE_REQ`/`VT_IPC_SERIAL_INPUT_REQ` omit that `request_id`
  carries the sender's cached switch generation rather than a request id, and
  that a stale value silently drops the chunk; `NET_IPC_IFADDR_LIST` overloads
  `arg0` (the status slot) with the record count.
- [ ] [DOCS][P2] Correct `wasm_driver.h`'s claim that `wasm_driver_call_entry`
  returns -1 when `entry_argc` exceeds 4: the WARP backend does not check
  `argc` and silently drops the extras.

- [ ] [CLEANUP][P1] Retire the register/packed-args spawn and service-registration
  path in favour of the descriptor form, then delete it. Packing a name into
  `arg0..arg3` caps it at 16 characters, and packing spawn arguments into the four
  entry-arg registers caps those at four words -- limits that come from the
  transport, not from anything the system needs. The register form has already
  been half-abandoned: `pm_apply_entry_bindings` passes zeros in all four entry-arg
  registers and startup values travel in the spawn-info buffer instead, which is
  why the Rust and Go ports (still reading the registers) cannot obtain their PM
  endpoint. Sizing storage to the packed form is also what produced the
  service-table name truncation fixed in fa19006629.

  Callers to migrate to `SVC_IPC_REGISTER_DESC_REQ`: all four AssemblyScript
  drivers (`src/drivers/{serial,keyboard,rtc,mouse}/*.ts`),
  `src/services/net_stack/net_stack.c:1407`, and
  `src/libsys/native/zig/libsys_native.c:699`. Each needs an xfer buffer for the
  descriptor, which the AssemblyScript drivers do not currently acquire -- that is
  the bulk of the work. Then drop `SVC_IPC_REGISTER_REQ`,
  `pm_handle_service_register`, `pm_unpack_name_args`, the `char name[17]` locals,
  and the four `entry_argv` words from `wasmos_app_instance_t` /
  `wasm_driver_manifest_t` (which also removes the fixed `args[4]` marshalling
  limit in the wasm3 backend).

  The container half of the entry-argument mechanism is gone: the manifest key, the
  packer flag, the header count, the record section and the parser's walk over it
  no longer exist, and `wasmos_app_desc_t` carries `reserved[4]` in their place.
  **The guest-arity half is now gone too.** Every entry point takes no arguments:
  all five `wasmos_main` ports, the 19 wasm `initialize` entries, the two wasm
  libsys async shims, and -- behind native ABI 14 -- the four native entries,
  where `driver_api` is the only surviving parameter. `pm_apply_entry_bindings`
  sets `entry_argc = 0` and `native_driver.c` calls `entry(&api)`. Verified on
  BOTH runtimes, which matters here: `m3_Call` validates the count, so wasm3 is
  the arm that can actually fail, while the WARP backend does not check `argc`.

  What remains is inert storage: the `entry_arg0..3` words in `pm_app_state_t`,
  `entry_argv`/`entry_argc` in `wasm_driver_manifest_t` and
  `wasmos_app_instance_t`, and the fixed `args[4]` marshalling in the wasm3
  backend. Nothing reads them now that `argc` is 0, so removing them is
  mechanical rather than a flag day.

- [ ] [ENHANCEMENT][P3] Decide what the generated cause-chain helpers are for, or
  drop them. `wrap`/`unwrap`/`root`/`is`/`as` and the 8-byte frame / 40-byte error
  object are generated in all five languages and have **zero call sites**, because
  an IPC reply's `arg0..arg3` fits only two frames -- so a chain cannot cross the
  boundary the error model exists to serve. Either give them a transport (an error
  object in a transfer buffer, say) and convert real call sites, or remove the
  generators and keep the packed `(domain, code)` axis alone.

- [ ] [FEATURE][P2] Give the path-based spawn a descriptor form, so a driver can
  have BOTH declared register windows and startup args. `PROC_IPC_SPAWN_PATH_CAPS_SYNC`
  packs a single `io_port_min`/`io_port_max` pair and cannot describe more than one
  window; only the index-based form escalates to `wasmos_spawn_caps_v2_t`. Device
  manager therefore picks one or the other (`device_manager.c`, the rule-spawn
  branch): a driver with `[[regions]]` spawns by module index and forgoes the
  `pci=..:..:.. vendor=... irq=...` args, and everything else spawns by path with
  args and a single window. Today nothing needs both -- `ata` is the only package
  declaring regions and takes no args -- so this is a latent limit, not a live bug.
  It becomes live the moment a driver on `/boot` (not a boot module, so
  path-spawned only) declares more than one window.

- [ ] [ENHANCEMENT][P3] Let a device-manager rule express alternatives, so one
  driver does not need N near-identical lines. `virtio_rng` has two rules solely
  because virtio exposes a transitional (0x1005) and a modern (0x1044) device ID
  for the same function, and the grammar has no OR. A list value
  (`ATTR{device}=="0x1005|0x1044"`) would collapse them.

- [ ] [ENHANCEMENT][P3] Rule matching cannot express "subclass is 0xFF", because
  `MATCH_ANY_U8` is 0xFF and reads as the wildcard. `virtio-rng` presents PCI
  class 0x00 / subclass 0xFF and is matched on vendor/device instead, with a
  comment in `default.rules` explaining the workaround. A separate
  present/absent flag per field would remove the collision; the same convention
  makes a real class or vendor of 0xFF/0xFFFF inexpressible.

- [ ] [CLEANUP][P2] Generate the POSIX libc constants; they are hand-copied into
  four languages today. `abi/constants.yaml` and `gen_abi_constants.py` now exist,
  so this is adopting a generator rather than building one.

  `O_RDONLY`, `O_WRONLY`, `O_CREAT`, `O_TRUNC`, `O_APPEND` (`libc/include/fcntl.h`),
  `SEEK_SET`, `SEEK_CUR`, `SEEK_END` (`unistd.h`) and `S_IFDIR`, `S_IFREG`
  (`sys/stat.h`) are each re-declared in `src/libc/rust/wasmos.rs`,
  `src/libc/zig/wasmos.zig` and the AssemblyScript runtime. The copies are
  byte-identical down to their comments ("Open flags, POSIX-valued. Bit 0 is the
  access mode..." appears verbatim in the Rust and Zig files), which is what makes
  drift a matter of time rather than a risk: nothing detects it. They cross a
  boundary in both directions -- the flags go to the FS service, the `S_IF*` bits
  come back in a stat reply -- so they are ABI, not a per-language convenience.
  Phase 3b deferred these deliberately ("can adopt the generated files
  incrementally"); the value generator is what was missing.

- [ ] [CLEANUP][P2] Generate the gfx protocol values, and move the gfx/fb opcodes
  to `abi/opcodes.yaml` where they belong. `libc/include/wasmos/gfx_ipc.h` holds
  30 hand-written constants of two different kinds, and the split matters:

  Message opcodes -- `FB_IPC_GET_INFO` (0x0100) through `FB_IPC_QUERY_MODES`
  (0x0107) -- are `ipc_message_t.type` values and belong in `opcodes.yaml`, which
  already models fb/gfx as subsystems.

  Everything else is a value a peer interprets and belongs in `constants.yaml`:
  `GFX_EVENT_*` (8 event codes), `GFX_POINTER_BUTTON_*` (3),
  `GFX_POINTER_GESTURE_*` (7), and the `FB_IPC_ABI_MAGIC`/`_VERSION` +
  `GFX_IPC_ABI_MAGIC`/`_VERSION` handshake pairs. Five of them
  (`GFX_EVENT_KEY`, `_POINTER`, `_CLOSE_REQUEST`, `GFX_IPC_ABI_MAGIC`,
  `_VERSION`) are already hand-declared in Rust, so the duplication has started.
  A gfx app in any language has to name an event code to dispatch on it.

- [ ] [CLEANUP][P3] Generate the remaining cross-service values in
  `wasmos_driver_abi.h`. That header exists to be shared, so a value in it is
  boundary-crossing by construction; 60 are still hand-written. The ones that
  clearly qualify:

  Enum-like alternatives a peer switches on -- `PROC_STATUS_*` (3),
  `PROC_MODULE_SOURCE_INITFS`/`_FS`, `WASMOS_EXEC_MATCH_*` (6 node kinds),
  `WASMOS_BROKER_PLAN_KIND_*` (2), `SVC_CLASS_EVENT_ADD`/`_REMOVE`,
  `FSMGR_BACKEND_BOOT`/`_INIT`, `VT_INPUT_MODE_*`.

  Descriptor versions both sides check -- `WASMOS_SVC_REGISTER_DESC_VERSION`,
  `WASMOS_MODULE_META_DESC_VERSION`, `WASMOS_PCI_DEVICE_DESC_VERSION`,
  `WASMOS_BROKER_SPAWN_PLAN_VERSION`, and the two register-desc versions.

  Capacities both sides size buffers from -- `WASMOS_SVC_NAME_MAX`,
  `WASMOS_SVC_CLASS_MAX`, `WASMOS_SUBSYSTEM_TAG_LEN`,
  `WASMOS_EXEC_HANDLER_NAME_LEN`, `WASMOS_IO_RANGE_LIMIT`,
  `WASMOS_MODULE_META_MAX_REGIONS`, `WASMOS_PCI_BAR_COUNT`,
  `HRNG_MAX_BYTES_PER_REQ`, `WASMOS_BLOCK_ZC_*`.

  `WASMOS_IPC_MSI_EVENT_TYPE` (0xFF01) is NOT one of these -- it is a message
  type and is already tracked for `opcodes.yaml` with `IPC_IRQ_EVENT_TYPE`.
  Struct layouts stay hand-written; layout is not what the constant IDL
  expresses.

- [ ] [CLEANUP][P3] Generate the capability-grant flags and font ids.
  `WASMOS_BUFFER_GRANT_READ`/`_WRITE` (`libc/include/wasmos/api.h`) are passed by
  every app that lends a buffer and are already duplicated into AssemblyScript.
  `FONT_ID_ROBOTO`/`_ROBOTO_MONO`/`_NOTO_SERIF` (`font_ipc.h`) are picked by any
  client asking for text. Both are small, and both are values a non-C client
  cannot currently name.

## Filesystems and Storage

- [ ] [ENHANCEMENT][P2] Apply the non-blocking reactor model to `fs-init` (currently a blocking
  dispatcher with no SEEK/STAT — `src/drivers/fs_init/fs_init.c:498-569`) and
  preserve the transfer-buffer ownership contract through all VFS relay paths.
- [ ] [ENHANCEMENT][P2] Re-enable ATA bus-master DMA on the GENERIC transfer path.
  `ata_dma_prepare` returns `WASMOS_ERR_DMA_DENY` unconditionally to force PIO,
  and `ata_dma_finish` is unreachable behind it
  (`src/drivers/ata/ata.c:655-673`). Carry the client `borrow_id` in the block IPC
  for that path and map via `dma_map_borrow`.

  Not "every op is PIO", which this entry used to claim: the zero-copy READ path
  already does real bus-master DMA. `BLOCK_IPC_READ_ZC_REQ` carries the client's
  borrow, `ata_read_zc_dma` maps it with `dma_map_borrow`
  (`WASMOS_DMA_DIR_FROM_DEVICE`), programs `ATA_BM_CMD_START` over a PRDT, then
  syncs and unmaps — the driver logs "bus-master DMA active" and "zero-copy reads:
  direct DMA into client buffer" on a normal boot. What is missing is that path
  for writes (`TODO(zero-copy writes)` in the same file) and for the generic
  prepare/finish hook above.
- [ ] [FEATURE][P2] Complete initfs zero-copy mapping with an explicit entry-offset ABI and
  correct revoke/lifetime behavior (still copy-based today).
- [ ] [ENHANCEMENT][P2] Refetch fs-manager boot metadata out-of-band (push/idle-step) to remove
  the nested synchronous `DEVMGR_QUERY_MOUNT_REQ` deadlock hazard during
  class discovery (`src/services/fs_manager/fs_manager.c:608`).
- [ ] [TEST][P2] Expand FAT coverage deliberately: FAT32 update modes and behavioral tests
  for each added contract.
- [ ] [FEATURE][P2] Evaluate additional filesystems and dynamic mount lifecycle only after the
  existing VFS/backends have clear mount, ownership, and recovery semantics.


- [ ] [TEST][P2] Mount a volume whose `bytes_per_sector` is not 512. The block
  layer now transfers `blk->sector_bytes`, set from the BPB at mount and refused
  unless it is a 512-multiple within `FAT_MAX_SECTOR_BYTES`, so the FAT and
  directory code no longer parses past what was staged. No fixture exercises it:
  both `newfs_msdos` and `mkfs.vfat` default to 512, and the host harnesses
  address their RAM images in 512-byte units. Needs a formatter invocation with
  an explicit sector size plus a harness that reads its own geometry.

- [ ] [ENHANCEMENT][P3] Let a slot run longer than one cluster span a GROWN
  directory. `fat_find_free_dir_slots` refuses `WASMOS_ERR_FS_NO_SPACE` when
  `needed` exceeds one cluster's entries and the directory had to grow, rather
  than appending a second cluster and running the LFN chain across both.

  Deliberately not fixed. It is unreachable -- `FAT_LFN_MAX` (255 characters)
  needs at most 21 entries and the smallest cluster this driver mounts holds 16,
  so it takes a name over ~200 characters on a 512-byte single-sector cluster --
  and an attempt at it added a second growth pass reachable by a `goto` INTO a
  switch-based coroutine body, jumping across resume points. That is more risk
  to a working allocation path than the case is worth. A free run that spans
  clusters is already handled when the clusters exist; only the
  append-two-at-once case is missing.



## Device Drivers and Input

Source: `architecture/16-device-manager-and-bus-enumeration.md`,
`architecture/17-console-io-and-character-device.md`, and
`architecture/21-virtual-input-testing-via-virtio-serial.md`.

- [ ] [FEATURE][P2] Implement virtio-serial queue setup plus data/control-plane byte-stream
  IPC; discovery/register access alone cannot transport host data
  (`src/drivers/virtio_serial/virtio_serial.c:160` `TODO(virtio-serial-transport)`).
- [ ] [FEATURE][P2] Build the `virt-input` service and host bridge after virtio-serial data
  transport exists; inject keyboard/mouse events through the normal compositor
  IPC path and add sequential QEMU UI automation tests (no source exists yet;
  `architecture/21`).
- [ ] [FEATURE][P2] Wire parsed device-manager rules into runtime bind/unbind/mount policy;
  spawn already works but the rule actions are still informational
  (`src/services/device_manager/device_manager.c:127`).
- [ ] [ENHANCEMENT][P2] Split a dedicated `irq.configure` capability from `irq.route` for
  level/active-low configuration (`src/services/pci_bus/linker.metadata:25`;
  kernel side `src/kernel/warp/link.cpp:2414`).
- [ ] [ENHANCEMENT][P2] Preserve each driver module's declared IRQ capability mask in
  device-manager metadata instead of granting the fixed IRQ 14/15 pair.
- [ ] [CLEANUP][P3] Remove the now-dead DMA-window defaulting in the individual
  `PROC_IPC_SPAWN_*_CAPS` handlers (`process_manager_spawn.c`); DMA windows are
  now installed from the driver's `dma.buffer` manifest capability
  (`capability_grant_name`), not the spawner.
- [ ] [CLEANUP][P3] Consolidate the per-file `#define PAGE_SIZE 0x1000` copies
  (physmem.c/process.c/memory.c/native_driver.c/capability.c/…) into one shared
  header.
- [ ] [ENHANCEMENT][P2] Replace the fixed `DEVMGR_RULE_TEXT_CAP` rules-file read buffer (now 4096)
  with an `FS_IPC_STAT_REQ`-sized (or streaming/chunked) read so the rules file
  has no size limit; a fixed buffer silently truncates trailing rules.
- [ ] [FEATURE][P2] Add hotplug/event publication and future bus providers (USB/virtual)
  through the normalized device-record contract.


- [ ] [BUG][P1] Correct 12-hour RTC decoding. The noon and midnight hours are off by 12 in
  both the BCD and binary branches; unreached while register B reports 24-hour
  mode (`src/drivers/rtc/rtc.ts:218` `FIXME`).
- [ ] [BUG][P1] Add the producer-lapped snap-forward to the PCI framebuffer's console-ring
  drain. The UEFI driver performs `rp = wp - cap`; this copy does not, so an
  overrun leaves it reading overwritten bytes
  (`src/drivers/framebuffer_pci/framebuffer_pci_native.c:237` `FIXME`).

## Networking

Source: `architecture/22-networking-virtio-net-and-stack.md`.

Baseline done and e2e-validated: virtio-net driver, netif bring-up, UDP,
TCP client (connect/stream/close) and server (listen/accept/echo) over rings,
DHCP/static addressing, DNS (`NET_IPC_RESOLVE`), and a verifying TLS 1.2 client.
Remaining:

- [ ] [ENHANCEMENT][P2] Harden TCP timeout/retransmit and the close path: drive
  `sys_check_timeouts()` from a dedicated timer source rather than only the idle
  loop, with retransmit/close e2e tests (`src/services/net_stack/net_stack.c:2646`;
  STATUS `Current Gaps`).
- [ ] [FEATURE][P2] Add IPv6 / NDP / ICMPv6 / dual-stack, multiple addresses per interface,
  and isolated multi-stack instances (`src/services/net_stack/lwipopts.h:41`
  `LWIP_IPV6 0`).
- [ ] [FEATURE][P2] Validate TLS certificate dates: wire an RTC time source into mbedTLS
  (`MBEDTLS_HAVE_TIME`/`HAVE_TIME_DATE`) so validity windows are checked.
- [ ] [ENHANCEMENT][P2] Harden TLS/large-transfer RX-ring backpressure so big bodies do not stall
  on a full RX ring (app-side flow control).
- [ ] [FEATURE][P2] Enable guest-to-guest loopback (`LWIP_NETIF_LOOPBACK` + net-stack loopback
  polling) so an in-guest server is reachable.
- [ ] [FEATURE][P2] Give `ata` real device DMA. There is no bus-master IDE (BMIDE/PRD)
  programming today, so every transfer is PIO regardless of the `dma_*`
  scaffolding. On QEMU's PIIX this means bus-master IDE; an AHCI controller
  (`ich9-ahci`) would be the better target and would also bring MSI, which
  legacy IDE cannot offer at all.
- [ ] [ENHANCEMENT][P2] Move the resident pci-bus request loop onto the coroutine/event-loop
  runtime. It blocks (never spins), which is sufficient while every request is
  answered from config space, but hot-plug will need it to originate requests
  while serving.
- [ ] [FEATURE][P2] PCI hot-plug: add a rescan opcode over `pci_scan_and_publish()` — small on
  its own, but knowing *when* to rescan needs an ACPI GPE/SCI path that does not
  exist yet. Depends on the coroutine-loop item above.
- [ ] [FEATURE][P2] Support multi-message plain MSI (cap `0x05`): needs a contiguous,
  naturally-aligned vector block from the kernel (an `msi_alloc_block`), so
  pci-bus currently reports MSI as exactly one vector
  (`src/services/pci_bus/pci_bus.c`, `msi_query`). Only the MSI-X path is
  exercised in-tree — no QEMU device in the default boot offers MSI without
  MSI-X.
- [ ] [ENHANCEMENT][P2] Steer MSI vectors at CPUs other than the BSP. Both `msi_x86_64.c`
  (`MSI_DEST_APIC_ID`) and `ioapic_program_rtes()` hardcode LAPIC 0; changing
  one without the other splits interrupt affinity across two models.
- [ ] [CLEANUP][P2] Move the kernel notify-type space (`IPC_IRQ_EVENT_TYPE` 0xFF00,
  `IPC_MSI_EVENT_TYPE` 0xFF01) into `abi/opcodes.yaml`. Both are currently
  hand-mirrored into each driver source instead of generated.
- [ ] [FEATURE][P2] Define the net owner-push wire protocol so TX/RX carry an explicit
  client `buffer_id`/grant instead of overloading `msg.arg0`/`arg1`
  (`src/drivers/virtio_net/virtio_net.c:479,718,752,775,900` `FIXME(owner-push)`).
- [ ] [FEATURE][P2] Add a multi-interface ifcfg selector so boot config can target non-default
  interfaces (`src/services/net_stack/net_stack.c:977` `FIXME(multinet-ifcfg)`).
- [ ] [ENHANCEMENT][P2] Evaluate a packet DMA fast path that removes the RX copy (only after the
  negative-path/restart/link-down test coverage lands).
- [ ] [ENHANCEMENT][P2] Minor: expand the net-stack lwIP diagnostic `vprintf`
  (`net_stack.c:316`) and source net-stack's clock directly from a native
  driver-api millisecond hook (`src/services/net_stack/port.c:34`).


- [ ] [ENHANCEMENT][P2] Raise `MEM_ALIGNMENT` to 8 in `lwipopts.h`. It is 4 on an
  LP64 x86_64 target against lwIP's guidance of 8 for 64-bit platforms; it
  works only because x86 tolerates misaligned access
  (`src/services/net_stack/lwipopts.h`).
- [ ] [CLEANUP][P3] Use `IP_IFADDR_RECORD_BYTES` in `ip`'s `cmd_show` instead of the
  hardcoded `24u` it repeats twice; a record-size change would otherwise desync
  silently (`src/utils/ip/ip.c`).

## Graphics, VT, and User Space

Source: `architecture/19-virtual-terminal.md`,
`architecture/20-graphics-framebuffer-and-compositor.md`,
`architecture/23-cli-and-user-space.md`, and
`architecture/24-environment-scopes-and-inheritance.md`.

VT I/O-multiplexer phase 5 (remaining; phases 0–4 shipped):

- [ ] [FEATURE][P2] Route an app's output to its controlling tty instead of straight to
  serial. Only three writers should reach the UART directly: the vt (mirroring
  the serial-bound slot), early boot (before the vt exists), and the panic/fault
  path. Everything else writes to a slot and lets the vt fan out.

  Today every other component writes to the kernel log instead. libc's
  `printf`/`puts`/`putsn` and `write(1|2, …)` call `wasmos_console_write`
  (`src/libc/src/stdio.c:294`, `unistd.c:308`), native services call
  `api->console_write`, and both land in `klog_write` -> `serial_write`, which
  fans out to COM1 and the vt's klog ring. The consequence is that an app's
  output is *system log*: it appears on the console slot no matter which slot the
  app belongs to, and on serial no matter where serial is bound. An app on vt-2
  prints onto vt-1's screen. The CLI is the only component in the tree that
  writes to a slot (`VT_IPC_WRITE_REQ`, `cli.c:637`), and it writes to serial as
  well — that duplication is exactly why vt-1 currently has to drop writer writes
  and suppress echo (doc 19, the log-mirror rules).

  Do it in libc, not in the ~53 files that print: a process already knows its
  controlling tty (`wasmos_startup_tty()`, from spawn info), so `putsn` can send
  `VT_IPC_WRITE_REQ` to that slot and fall back to `console_write` only when
  there is no tty (drivers, early services, anything spawned without one). Native
  services need the same treatment behind `api->console_write`. Then apps need no
  changes at all, and `console_write` becomes what its name says: a kernel-log
  call, not the way user code prints.

  Blocked on one missing primitive: the vt cannot mirror a slot to serial while
  `console_write` feeds the klog ring, because the mirrored bytes come straight
  back into the slot it just wrote (klog -> vt-1 -> console_write -> klog). A
  serial write that bypasses the klog ring — a host call, or a flag on the
  existing one — has to land first.

  Done when: an app spawned on vt-2 prints on vt-2 and nowhere else; the
  serial-bound slot's output reaches serial through the vt; vt-1 no longer needs
  its log-mirror special cases; and `wasmos_console_write` has no callers outside
  libc's fallback path, the kernel, and the panic path.
- [ ] [CLEANUP][P3] Delete `console_ring_t` and the plumbing that fills it now that nothing
  drains it: the kernel producer (`src/kernel/serial.c` `serial_ring_write`,
  `serial_console_ring_id`, `serial_console_ring_ptr`), the native driver API's
  `console_ring_id` (an ABI version bump), and the type itself in
  `src/drivers/include/wasmos_driver_abi.h`. The klog ring replaced it.
- [ ] [ENHANCEMENT][P3] Move the console slot's shell out of `sysinit.rc` too, so the vt owns
  every shell. The vt already spawns one per slot on first use; the console
  slot's is still started by `sysinit.rc`, last, because the first prompt is what
  the test framework reads as "the system is up". Spawning it from the vt (which
  comes up early) put that prompt in the middle of boot and made tests drive a
  half-started system — `test_shmem_grant_revoke_pair` failed that way. Needs the
  readiness contract to stop keying off the first prompt first.

Other graphics/VT/UI:

- [ ] [BUG][P1] Reclaim old libui font shared-memory objects when text buffers grow
  (`src/libui/include/libui.h:471`) and the compositor's title-glyph shmem IDs
  on growth (`src/services/gfx_compositor/gfx_compositor.zig:2007`).
- [ ] [ENHANCEMENT][P2] Make `libui`, font-service, and compositor allocation/index arithmetic
  overflow-safe: validate dimensions before multiplication, compute buffer
  indexes in `usize`, and cap growth before capacity doubling.
- [ ] [FEATURE][P2] Add an explicit mode-set / update-framebuffer-info path before the VBE
  reprogram after ExitBootServices
  (`src/drivers/framebuffer/framebuffer_native.c:159`,
  `src/drivers/framebuffer_pci/framebuffer_pci_native.c:275`).
- [ ] [ENHANCEMENT][P2] Make VT cell/reply/replay writes robust under framebuffer backpressure;
  they are best-effort today and drop on persistent queue-full
  (`src/services/vt/vt_main.c:244,287,892`).
- [ ] [BUG][P1] Reproduce and fix the deferred rapid-TTY-switch prompt
  duplication/misalignment (`src/services/vt/vt_main.c:1755`); keep deferred
  until a stable repro exists.
- [ ] [FEATURE][P2] Add a real WASM link step to the AssemblyScript `libui` build (currently a
  stub — `src/libui/assemblyscript/libui.ts:133`).
- [ ] [ENHANCEMENT][P2] Add script-engine diagnostics for unclosed `if` blocks (the EOF
  `total_depth > 0` warn path is a no-op) and preserve the documented `script`
  vs `source` environment-scope semantics.
- [ ] [FEATURE][P2] Expand VT behavior only from an explicit compatibility need: richer ANSI,
  UTF-8, scrollback, or input APIs should each include focused behavioral tests.


- [ ] [BUG][P1] Type-check `ui_component_set_text`. It casts `component_data` to
  `ui_text_data_t*` with no check, so setting text on a LIST_VIEW, TREE_VIEW,
  SCROLL_VIEW or MENU_BAR reinterprets that component's data
  (`src/libui/include/wasmos/libui.h:676` `FIXME`).
- [ ] [BUG][P1] Destroy an open popup's compositor window at teardown.
  `ui_menu_item_destroy_data` releases only the shmem, leaking the window and
  its shared buffer (`src/libui/include/wasmos/libui_menu_item.h:557` `FIXME`).
- [ ] [BUG][P3] Add the '9' glyph to `drawDigit3x5`; the table holds 0-8 and the guard
  rejects 9 (`src/libui/assemblyscript/libui.ts`).
- [ ] [CLEANUP][P3] Remove the no-op self-assignment `d->list.capacity = d->list.capacity`
  (`src/libui/include/wasmos/libui_dropdown.h:69` `FIXME`).
- [ ] [BUG][P1] Flush after the tetris back-buffer blit under wasm3, or gate the app on
  WARP. `present()` writes through the mapped window, which aliases the shared
  region's own pages only under WARP; under wasm3 `shmem_map_auto` rewrites the
  process page tables while the interpreter reads linear memory through its
  kernel-side buffer, so the blit never reaches the shared pages. The app builds
  for both runtimes (`examples/rust/tetris/tetris.rs:414` `TODO`).


- [ ] [BUG][P1] Apply the scroll offset when hit-testing scroll-view children.
  `ui_layout_scroll_view` assigns child bounds without it and
  `ui_render_component_clip` subtracts `offset_y` only at paint time, while
  `ui_find_clickable_at`/`ui_find_component_at` test the raw `bounds` -- so a
  click inside a scrolled container does not match what is on screen
  (`src/libui/include/wasmos/libui.h`,
  `src/libui/include/wasmos/libui_scroll_view.h`).
- [ ] [BUG][P1] Type-check `ui_component_text_len`, which has the same missing check
  as `ui_component_set_text` and reinterprets LIST_VIEW/TREE_VIEW/SCROLL_VIEW/
  MENU_BAR data as `ui_text_data_t` (`src/libui/include/wasmos/libui.h`).
- [ ] [BUG][P1] Make "no selection" representable. `ui_component_alloc` and
  `ui_component_collection_clear` set `selected = -1`, but the list, tree and
  dropdown layout passes clamp it to 0 for a non-empty collection, so the state
  cannot survive a layout (`src/libui/include/wasmos/libui.h`).
- [ ] [BUG][P1] Reopen a menu popup when its entries change without changing the
  child count: `ui_menu_item_sync_popup` compares only the height, so replacing
  entries leaves a stale popup (`src/libui/include/wasmos/libui_menu_item.h`).
- [ ] [ENHANCEMENT][P2] Split `ui_component_list_append`'s return: it yields a row
  index for LIST_VIEW/TREE_VIEW/DROPDOWN but a component id for MENU_ITEM, two
  incompatible non-negative domains a caller cannot tell apart
  (`src/libui/include/wasmos/libui.h`).

## Validation and Documentation

Source: `architecture/25-diagnostics-status.md`,
`architecture/26-repo-map-and-validation.md`, and
`architecture/27-python-test-framework.md`.

- [ ] [TEST][P2] Add behavioral regression coverage with every new subsystem contract;
  reject source-text assertions.
- [ ] [FEATURE][P2] Finish the toolchain SDK's remaining Stage 1 milestones
  (`docs/toolchain.md`): build `compiler-rt` builtins for wasm32 if a guest ever
  needs `__int128` — measured to be the ONLY shape that needs them, reaching eight
  symbols (`__multi3`, `__udivti3`, `__divti3`, `__umodti3`, `__modti3`,
  `__fixdfti`, `__fixunsdfti`, `__floatuntidf`); 64-bit arithmetic and float
  conversions are native wasm instructions and need nothing, nothing in tree uses
  `__int128`, and `tests/test_sdk_arithmetic.py` pins that boundary, so this is a
  gap to close on demand rather than a missing piece; and ship a standalone SDK
  build that does not borrow the host LLVM. Stage 2 (a native
  `wasm32-unknown-wasmos` LLVM triple) follows those.
- [ ] [ENHANCEMENT][P3] Decide whether the 32 KiB layout budget
  (`WASMOS_ZIG_USER_VA_LIMIT`, enforced by `scripts/wasm_stack_check.py`) should be
  relaxed or dropped. The reason it existed is gone: it was documented as a
  pointer-validity rule — a fixed 16-page `MEM_REGION_WASM_LINEAR` mirror bounding
  every host-call pointer, so globals above it fail silently — and reserved-VA
  linmem made that false, because `mm_context_rebind_wasm_linear` and
  `mm_context_bind_wasm_linear_scattered` resize that region to the guest's real
  linear memory. Measured 2026-08-21: a Zig module built `--stack 1048576` (data at
  `0x100000`, failing the check) ran `console_write` and `xfer_buffer_read` with
  pointers above 1 MB under **both** WARP and wasm3. The check is kept as a *size*
  guard — it catches a module that reverted to its toolchain's 1 MB default and so
  needs 2 MiB of declared memory instead of one page — and the comments now say so.
  What is left is a judgement call: keep it as a size guard, raise the number, or
  drop it and let each app declare the memory it wants. (The divergence that
  prompted this is gone: `examples/rust/hello` builds through `wasmos-rustc` now, so
  it takes the same small-stack layout as an SDK-built module and passes the check
  where it used to fail it.)
- [ ] [TEST][P3] Run the SDK's *compilers* on Linux. The wrappers themselves are
  covered: all eight parse and run their argument handling, manifest reading and
  path resolution under busybox `ash`/`awk` on Linux (harsher than CI's `dash`),
  relocation and symlinked invocation included. What has only ever run on macOS is
  the compilation itself. CI's `defconfig` job installs every language toolchain and
  builds `run-qemu-test`, and each SDK smoke app is a dependency of the kernel
  target, so the first CI run of this branch is that check — it just has not run
  yet.
- [ ] [TEST][P2] Give the host suite a way to test wasm32-only libc behaviour.
  Some defects are invisible on the host by construction: `%lld`/`%llx` truncated
  to 32 bits because `vsnprintf` cast `long long` through `long`, which is 64-bit
  on the host and 32-bit on wasm32, so `test_libc_stdio.c`'s existing `%lld`
  cases passed throughout and the fix is verified by inspection alone. The only
  wasm32 coverage today is `test_go_abi_sizes.c`, which is `-fsyntax-only` and
  therefore limited to `_Static_assert`. A runtime harness would need a C module
  compiled `--target=wasm32` and executed — node with a stub import table, as
  `tests/unit/{as,go}/run_*_test.mjs` already do for those languages, is the
  cheapest route and would serve every future bug of this class.
- [ ] [TEST][P2] Cover `fread`/`fwrite`. Both were fixed (short reads no longer
  report EOF, short writes now raise the error flag) with no test, because a
  `FILE` here is backed by an fd whose `read`/`write` go through FS IPC, and the
  host suite has no stub for that layer. A fake backend would also make the
  console-vs-file split testable, which is where the remaining `read`/`write`
  ABI-limit item lives (`src/libc/src/unistd.c`).
- [ ] [TEST][P2] Add focused stress/negative tests for TLB shootdown, service restart,
  futures cancellation, virtio-serial transport, networking link-down/restart,
  and new DMA paths.
- [ ] [TEST][P2] Add graphical input-injection tests only after virtio-serial transport is
  usable; use distinct sockets and never run QEMU sessions in parallel.
- [ ] [ENHANCEMENT][P2] Extend `scripts/kconfig_to_cmake.py:37` symbol map as more CMake cache
  settings migrate to Kconfig, and make `scripts/quality.sh:133` clang-tidy lint
  C++ sources (missing `--extra-arg` flags).
- [ ] [BUG][P2] Make `wasmos_ide_libc` and `wasmos_ide_tools` compile. Both emit
  compile-DB entries whose flags cannot parse, so CLion indexes those files with
  errors: `wasmos_ide_libc` builds `src/libc` for the host, where
  `__builtin_wasm_memory_size`/`_grow` do not exist (`src/libc/src/stdlib.c:30`;
  the real build passes `--target=wasm32`), and `wasmos_ide_tools` builds the
  host tools with no sysroot, so libc++'s `<cstring>`/`<cstdlib>` cannot find
  their C headers — the top-level `CMakeLists.txt` blanks `CMAKE_OSX_SYSROOT` for
  the freestanding targets. `scripts/quality.sh lint` is green regardless,
  because it rewrites the DB: it injects `--target=wasm32` for `src/libc` and
  drops `src/tools` from clang-tidy entirely. The fix is a target-triple and a
  sysroot option on `wasmos_add_ide_c_target` (`skills/wasmos-ide-targets`).
- [ ] [DOCS][P2] Keep architecture documents authoritative for design, `STATUS.md` concise
  for current behavior, and this file limited to unfinished work.


- [ ] [TEST][P2] Restore coverage for the cases whose assertions no longer reach the
  documented scenario: the `sched_timeout_arm` 0-to-1 coercion is unreachable
  through `sched_event_wait` (`tests/unit/test_sched_event.c:312` `FIXME`);
  `test_malloc_overflow` cannot distinguish overflow handling from a heap the
  harness stubs dead (`tests/unit/test_libc_stdlib.c:89` `FIXME`); the P5
  work-steal case is pinned against the opposite field from the one the
  scheduler reads (`tests/unit/test_sched_runqueue.c:1450` `FIXME`); the libui
  key-decode clamp case reaches only the lower bound; `test_ipc.c` S4 duplicates
  Q10 rather than exhausting the select table.
- [ ] [TEST][P2] Align `stubs_native_libsys.c`'s `str_copy_bytes` with the real one. The
  stub accepts `src_len == 0` where `src/libc/src/string.c` refuses it, and
  `libsys_native.c` branches on that return, so host and target disagree
  (`tests/unit/stubs_native_libsys.c:7` `FIXME`).
- [ ] [TEST][P2] Correct the `user_mutex_user_try_lock`/`_unlock` stub arity. The stubs take
  2 parameters; `user_mutex.h` declares 4 and `syscall.c` calls with 4. It
  compiles only because the test never includes the header
  (`tests/unit/test_syscall_ipc.c`).
- [ ] [TEST][P2] Wire up or delete `tests/unit/include/spinlock.h`. Nothing includes it, so
  `-DWASMOS_TEST_USE_REAL_SPINLOCK_DECLS` has no effect
  (`tests/unit/include/spinlock.h:11` `TODO`).
- [ ] [TEST][P2] Make the threading selftest's join markers reflect what they name. Three
  markers are emitted from the wait/kill result, so a broken join-after-kill
  still prints "ok" (`src/kernel/kernel_threading_selftest_runtime.c:320`
  `FIXME`).


- [ ] [TEST][P2] Make `stubs_xfer_buffer_platform.c`'s `pfa_free_pages` model the
  real allocator. It is a total no-op, so both leaks and double-frees are
  invisible to every test built on it.
- [ ] [TEST][P2] Distinguish the `_noirq` spinlock forms in `stubs_spinlock.c`. On
  the host lock/unlock and their `_noirq` variants are interchangeable, so a
  mispaired acquire/release passes in tests and corrupts the preempt depth on
  target.
- [ ] [TEST][P2] Close the gaps in `tests/unit/include/sched_event.h`: it is
  layout-incompatible with the kernel struct (`wait_list` replaced by a pthread
  mutex/condvar), ignores `timeout_ms` so no timed-wait path is covered, and
  loses a wake raised before a waiter parks -- something the kernel's
  under-lock enqueue cannot do.
- [ ] [TEST][P2] Make `test_hostcall_ipc.cpp`'s timed-select rows actually reach a
  deadline. `timer_ticks()` is frozen and nothing calls `sched_timeout_check`,
  so `WASMOS_TIMEOUT` arrives via the spurious-wake path instead.

- [ ] [TEST][P2] Make `run-qemu-sched-stress-test` actually spread across CPUs, or
  stop claiming it does. Its pass condition is only "every worker finished with
  no orphans"; the summary line reports `cpus=`/`mask=` and nothing checks them.
  Observed over three runs with `online=4` every time: `cpus=3`, then `cpus=2
  mask=0x9` (CPUs 0 and 3) under `warp_smp`, and `cpus=1` under `wasm3_smp` --
  that last run forwarded all 2048 tokens on a single CPU and exercised no
  cross-CPU path at all.

  The cause is structural, not luck. The ring is a strict hand-off: each
  `ipc_send_from` wakes the next worker, and `sched_wake_thread` enqueues a woken
  thread on the **waking** CPU's queue (`src/kernel/sched_thread.c:634`), with
  `WASMOS_SCHED_CALLER_CPU_BIAS` (ON by default, `CMakeLists.txt:848`) also
  rewriting `last_cpu` to the waker's CPU. A token therefore follows whichever
  CPU currently holds it, and only work stealing moves it. With
  `SMP_STRESS_TOKENS` (4) at most equal to the core count, the ring can sit
  entirely on one CPU while satisfying every assertion.

  So the fix is not just asserting `cpus > 1` -- that would make a green gate red
  without making it test more. Raising the token count above the core count, or
  giving workers disjoint affinity, is what would force the migration path;
  measure across configs and repeated boots first
  (`src/kernel/kernel_sched_smp_stress_runtime.c:140`).

- [ ] [TEST][P2] Drive the `set_ready` half of the exiting-owner refusal properly.
  `tests/unit/test_process_lifecycle.c` hits `process_set_running` hundreds of
  times per run but `process_set_ready` only 0-7 times per 300 rounds, because
  `process_kill` marks the owner's threads shortly after setting `exiting`, so the
  requeue usually finds no READY sibling and parks instead. Reaching it needs the
  sub-window `process.h` describes as "`exiting` 1 slightly ahead of ->state".
  The suite therefore asserts the SUM of both counters. That is honest but thin
  for the half the original panic actually named
  (`set_ready zombie`, CI run 32561829781): a regression that broke only that
  branch could pass. Widening it probably means driving the kill and the
  retirement from a shared barrier rather than the current announce-and-spin.
- [ ] [TEST][P2] Put a test behind the promotion demotion-guard, or drop it.
  `process_set_ready` and the `process_wake_process_waiters` fast path promote
  only a BLOCKED thread so they cannot write READY over a RUNNING one. That guard
  came from a PR review hypothesis, not an observed failure, and nothing
  demonstrates the previous unconditional `thread_set_state` ever demoted
  anything. Two attempts at implementing it wedged the boot in CI (once by
  dropping `block_reason`, once by gating `sched_wake_claim_enqueue` on the
  result), and no local gate caught either — unit suite, both kernels and
  `run-qemu-test` on both defconfigs pass with either bug in place, because
  booting to a prompt does not route a wake through those sites. Either construct
  the interleaving that proves the guard is needed, or revert it to the simpler
  unconditional form. Leaving an untested guard whose motivation is a hypothesis
  invites a later "cleanup" to reintroduce the regressions.
- [ ] [BUG][P1] Confirm the SMP scheduler stress panic stays fixed. One cause is
  found and fixed: dispatch took no exclusive claim on the thread it was about
  to run, so two CPUs could resume one `process_context_t` on one kernel stack
  and the thread resumed a torn rip (`cpu_sched_claim_for_dispatch`, pinned by
  `X7 one dispatch per thread` in `tests/unit/test_sched_concurrency.c`).

  Reopen — do not re-diagnose from scratch — if `FAIL: SMP scheduler stress test
  did not pass (stalled ring)` recurs with a `reason : cpu_exception` panic. The
  two original captures, both on the SMP defconfig jobs (the only ones that run
  `run-qemu-sched-stress-test`), were run 31949875433 (`wasm3_smp`, `a=0x0e`) and
  run 31970194315 (`warp_smp`, `a=0x06` at `rip=ffffffff803a2860`). Both resumed
  a rip inside `g_cpus`; `cpu_local_t.self` is its own address, which is why the
  bytes at that rip read as a pointer to themselves.

  What the fix is NOT verified against: whether it was the only cause. The
  panic's natural rate is roughly 1 boot in 40, so the 85 clean stress boots it
  has since survived (40 `warp_smp`, 25 instrumented `warp_smp`, 20 `wasm3_smp`,
  all Linux/MTTCG) do not resolve a second mechanism if one exists. The unit
  test is what pins this one.

  Reproducing needs a Linux x86 runner; MTTCG on Apple Silicon masks it. Force
  TCG (`WASMOS_QEMU_ACCEL=tcg`) and reset the OVMF vars file between boots:

      cmake -S . -B build-warp_smp -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig \
            -DWASMOS_SCHED_SMP_STRESS=ON
      cmake --build build-warp_smp --target run-qemu-sched-stress-test

  A `[sched] claim lost` line means the race fired and the claim resolved it.

- [ ] [BUG][P1] `test_virtio_net_notify_e2e` (the `notify rx=` / RX-frame-notify
  case) fails intermittently, roughly 1 run in 5: the guest stays alive and
  reaches `arp sent`, then no `notify rx=` arrives, so it fails as an assertion
  rather than a timeout. Distinguish it from the whole-session stalls, where
  everything stops and the test ERRORs instead.

  The cause is known, so this is a scheduling question rather than an
  investigation: the net-stack-to-driver framing is still legacy per-frame IPC
  with `tx_slots[4]` and drop-on-`ERR_MEM` under burst, not the ring transport
  (see the owner-push wire-protocol item under Networking, which is the real
  fix). Until that lands the test is expected to flake.

  If bisecting it anyway: **use separate build directories and do not `git
  stash`.** A previous attempt was unsound because a stash silently did not
  take, so both arms ran identical code and the result meant nothing.

- [ ] [BUG][P2] Confirm the whole-session wedge stays fixed. It had THREE causes,
  all fixed, and finding each only because the previous one was gone is the
  reason this item stays open rather than closed on one green run.

  1. `46bd8b5d26` -- a READDIR's terminating `FS_IPC_RESP` was refused for a full
     relay queue and dropped, stranding fs-manager and, behind it, the client and
     every later test in that session. Replies on both sides of the FS chain now
     retry rather than drop. Its signature is a `[diag]!    refused` line.
  2. The running-elsewhere guard duplicated in `sched_enqueue_thread_from`, which
     marked a thread READY and returned without leaving a claim, so nobody owed
     the enqueue. Same stranding as the `cpu_sched_enqueue` half fixed in
     `8c063c62f3`, on the second entry point. Its signature is
     `stranded(ready,no-rq)>0` plus a `[sched] enqueue current` line carrying
     `caller=`, which is the field that distinguishes the two copies.
  3. Select-set readiness was edge-triggered: `ipc_select_wait` consulted only
     the single-slot `ready_ep` latch, so a send that landed before
     `ipc_select_add` registered the watcher raised no signal at all, and two
     signals before a wait collapsed into one. Either way the message stayed
     queued on an endpoint nobody was told about and its owner parked until an
     unrelated later send re-armed the set. The wait now scans the watched
     queues first, which makes the queues the authority. Its signature is a
     blocked thread whose `wait=select:` line shows a watched endpoint with
     `q>0` (CI run 32009665809: fs-fat parked on `ep:54(q=1)` holding
     fs-manager's `type=0x420`, wedging the boot before the CLI).

  What would settle this: several consecutive green full-suite runs with no
  `[diag]!    refused` line, no `stranded(ready,no-rq)` that PERSISTS across the
  dump's samples, and no parked select waiter watching a `q>0` endpoint. Each
  marker names one of the three causes, so a recurrence says immediately which
  mechanism came back.

  Persistence is the whole test for the second marker, not a refinement of it. A
  single sample reporting `stranded=1` is normal: an enqueue claim published but
  not yet settled looks identical to a stranded thread, and the dump takes its
  samples ~1 s apart. Run 32009665809 shows exactly that -- `stranded=1` then
  `stranded=0` on a healthy guest. The real one held across all three samples
  while the thread's `disp` never moved.

- [ ] [BUG][P1] `test_shmem_grant_revoke_pair` fails intermittently in the
  `scheduler-and-ipc` battery: `[test] shmem e2e forged id denied` never arrives,
  and the tail at that point is still early boot (font loading), so the guest had
  not reached the probe rather than answering it wrongly. Seen on CI runs
  31882420302 and 31891304942, passing in between, so it is a flake and not a
  broken assertion.

  It is NOT the whole-session wedge, which the old description guessed it was.
  Reproduced locally on 2026-08-16 with the thread dump: no endpoint had refused
  a send (the wedge's signature), and cli was RUNNING rather than stranded, with
  the marker missing at the early-boot stage. The same battery passed on an
  immediate rerun, 69/69. So this is its own fault and needs its own capture --
  though still not by starting from `shmem_grant`/`shmem_revoke`
  (`tests/test_shmem_grant_revoke_e2e.py`), since the guest had not reached the
  probe.
- [ ] [BUG][P2] Make `run-qemu-ring3-threading-test` assert the probe it names.
  The ring-3 thread lifecycle probe never issues a join syscall: instrumenting
  every join for the `ring3-threading` process name gave zero hits, so all three
  `wasmos_thread_spawn_cont(...) > 0` guards in
  `src/kernel/ring3_thread_lifecycle_probe.c` are false and every join is skipped.
  The test cannot notice, because every marker it requires (`thread create`,
  `join`, `join self deny`, `detach syscall ok`) is gated on the `ring3-native`
  process name and comes from the OTHER probe -- so the "lifecycle marker test"
  asserts a different process's markers and the lifecycle probe contributes
  nothing observable.

  When fixing: the probe blob is mapped exec+non-writable over `code_size` bytes
  only, so growing its `.bss` (for example adding a fourth 4 KiB stack) is not
  obviously safe.

- [ ] [DOCS][P3] Correct the eight commit messages in `fdd4472ef6..1c6bc236ec`
  whose "verified on both runtimes" gate lines actually name WARP twice. The work
  itself was verified; only the messages are wrong. A follow-up note commit is
  preferable to a history rewrite.

- [ ] [ENHANCEMENT][P2] Make the build configuration say what it is, or refuse.
  Two related traps, both of which have already cost a session by testing the
  wrong runtime:
  - `-DWASMOS_WASM_RUNTIME_*` on the command line LOSES to a `.config` already
    seeded in the build directory, because kconfig FORCEs the cached value
    afterwards. Consistent with the documentation, but the flag silently does
    nothing rather than erroring.
  - A build tree seeded from the base defconfig ends up with a `.config` that does
    not describe its own runtime (the cache retains the previous value). Trees
    seeded from a runtime-bearing defconfig are correct.

  Either make the explicit flag win, or fail the configure step when it disagrees
  with the cached value. Until then the only reliable check is the `runtime=` boot
  marker plus `nm` on the staged `kernel.elf`
  (`skills/wasmos-build-and-run/SKILL.md`).

- [ ] [TEST][P1] Give the host unit gate a per-test timeout. A corrupted wait
  list HANGS the gate instead of failing it -- demonstrated by the M-D mutant --
  and the scheduler suite has the same property, because nothing bounds an
  individual case. The consequence is worse than a slow gate: the bugs most
  likely to produce a corrupted list or a lost wake are exactly the run-queue and
  wait-list defects this suite exists to catch, so the suite converts a
  diagnosable red test into an indefinite stall with no indication of which case
  was running. A per-case alarm that reports the case name and aborts would make
  those failures readable.

- [ ] [TEST][P2] Investigate the `ps tree` flake under the 64-test CLI suite. It
  passes in isolation in ~12 s and the suite went green on a re-run, so it is
  order- or timing-dependent rather than broken. Uninvestigated; the value is in
  finding out whether it shares a cause with the other intermittent CLI-suite
  failures rather than in the case itself.

- [ ] [BUG][P1] Make the kernel build track header dependencies. The kernel objects
  are produced by hand-written `clang -c` commands whose DEPENDS list names only the
  source file, so editing a header -- or even touching the `.c` -- does not trigger a
  rebuild. Every compile-time guarantee in the kernel is therefore only evaluated on
  a clean build: `_Static_assert`s added to couple two constants pass locally while
  the object is stale, and a changed struct layout can link against callers compiled
  against the old one.

  This is the same defect class as the AOT symbol-table staleness already fixed at
  `CMakeLists.txt:1147`, where `warp_aot.cpp` depended only on its own source while
  including a generated table named by arity. That one was found only after it
  produced an "Imported symbol could not be found" that was mistaken for a WARP
  quirk for a whole session; the kernel rule has the same shape and has not
  produced a visible symptom yet.
