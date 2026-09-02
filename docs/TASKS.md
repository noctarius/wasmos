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
- [x] [CLEANUP][P3] Retire the `shmem` subsystem in favour of a single
  `xfer_buffer` sharing mechanism. DONE. The eight `shmem_*` host calls, both
  runtime shims, the native-driver `shmem_*` hooks, the `mm_shared_*` registry,
  `MEM_REGION_SHARED` and the kernel ring-3 shmem tests are all gone, and no
  source file mentions shmem in any case.
  What it took beyond deleting call sites, recorded because each was a design
  decision rather than a port:
  - The window surface INVERTED: `GFX_IPC_ALLOC_SHARED_BUFFER` had the
    compositor allocate and grant, which the transfer-buffer contract forbids.
    `GET_SURFACE_SPEC` / `ATTACH_SURFACE` / `DETACH_SURFACE` put the client on
    the owning side; DETACH is load-bearing because there is no unborrow
    notification and the compositor's mapping comes from a pool every native
    service shares.
  - The damage list moved INSIDE the surface (`PRESENT_WINDOW` arg3 is now a
    byte offset), so it needs no second buffer, borrow or grant.
  - The font protocol needed a descriptor: `RASTER_INTO` carries six
    independent values, past the four opcode words, so both it and `MEASURE`
    now pass `font_raster_request_t` in a caller-owned buffer with the UTF-8 run
    behind it. `FONT_IPC_RASTER_GLYPH_REQ` was deleted outright -- it granted a
    service-owned scratch to the caller and had no callers left.
  - Every write-back call disappeared. The mapping IS the buffer's frames on
    both runtimes, so libui's per-present full-window `memcpy(p, p, n)` (~1.9 MB
    for an 800x600 window, inside a hostcall) is simply gone.
  - Host-call ids are now assigned from list order rather than written by hand,
    since the id already WAS the position; that is what makes a removal a
    deletion instead of a renumbering chore.
- [ ] [FEATURE][P2] Add `xfer_buffer_map_borrow`: a rights-checked, borrow-scoped
  CPU mapping, so a BORROWER can reach a buffer zero-copy. Today the matrix has a
  hole. Owner + CPU is `xfer_buffer_map`; borrower + device is `dma_map_borrow`,
  which returns a device DMA address; borrower + CPU has no zero-copy path at all
  and must use `xfer_buffer_read`/`write`. The distinction the ABI actually draws
  is not owner-vs-borrower but WHO TOUCHES THE PAGES -- a device or a guest CPU --
  and the CPU half of the borrow side is simply missing.

  The hole binds precisely because ownership is fixed by contract: the client owns
  and the server is a grantee (`12-dma-transfers.md`), so any server that must
  READ a client's bytes with the CPU rather than a device is forced onto a copy.
  A compositor reading an app's surface to blend it is the case that makes this
  worth closing; the FS and block paths avoid it only because they DMA.

  Model it on `dma_map_borrow`, which already establishes borrow-scoped mapping
  with rights validation (`flags` must be non-zero and a subset of the borrow's
  rights): take a `borrow_id`, validate the requested access against the borrow,
  return a linmem byte offset like `xfer_buffer_map` does, idempotent per borrow,
  with an unmap that tears the window down.

  Constraints established 2026-08-29, all load-bearing:
  - WRITE implies a readable mapping. x86-64 PTEs encode permission as presence
    plus a single R/W bit (`paging.c:683-690` translates MEM_REGION_FLAG_WRITE to
    PT_FLAG_WRITE, MEM_REGION_FLAG_READ is not consulted at all), so write-only is
    not representable. Say so in the ABI doc rather than implying an enforcement
    the MMU cannot deliver. WRITE alone stays meaningful for `dma_map_borrow` and
    the copy calls, where direction is enforced by the device or by the call.
  - The mapping necessarily sets MEM_REGION_FLAG_USER, so the guard at
    `paging.c:651` on USER && WRITE is where the existing safety reasoning about
    writable user mappings lives; read it before adding a second caller.
  - Teardown ordering: the window must not outlive the borrow. `unborrow` and
    `reborrow` already exist, so revocation has to tear the mapping down rather
    than leave stale PTEs over frames the owner may release.
  - No new disclosure: the frames are already readable by any acquirer via the
    owner map, and a borrower is someone the owner chose to hand the buffer to.
    See the zero-on-acquire item below, which is the pre-existing gap and is
    independent of this one.

  Unblocks the compositor compositing from a borrowed surface without a copy,
  which is the shape the graphics migration wants: a draw list by default via
  `GFX_IPC_SUBMIT_COMMANDS` (0x0204, allocated and "not yet dispatched" --
  `docs/architecture/20-graphics-framebuffer-and-compositor.md:131`), and an
  app-owned surface borrowed to the compositor when an app must author pixels
  itself.

  The draw list itself goes in a CLIENT-OWNED DESCRIPTOR, never in the four IPC
  argument words: `arg0 = buffer_id, arg1 = offset, arg2 = size`, per the rule in
  `AGENTS.md` and `skills/wasmos-add-opcode` §"Step 0". A command list is
  unbounded and grows by construction, which is exactly what the four words are
  not for. The app acquires the descriptor buffer once and reuses it, so the
  per-frame cost is one write into an already-mapped buffer.

  Moving the primitives compositor-side also retires four separate rasterizers --
  `vt_main.c` (35 fill/blit sites), `tetris.rs` (20), `libui.ts` (12),
  `gfx_smoke.c` (2) -- into the one service that owns the pixels.

  TEXT IS THE CASE THAT PAYS BEST. With a text command in the list, NO app needs
  font rendering at all: the compositor resolves it through `font_service`, which
  already exposes `handle_raster_glyph` and `handle_raster_text_into`. Today an
  app that wants text either talks to font_service itself -- acquiring the text
  and mask buffers itself -- or hand-rolls a bitmap font, as `libui.ts` does with its built-in 3x5 digit
  font (`drawDigit3x5`, digits 0-8 only). Both disappear.

  That also settles what the app-owned pixel surface is FOR. Rendering pixels
  yourself stops being the default path every app walks and becomes an ACTIVE
  CHOICE, taken by the apps that genuinely author pixels -- an image viewer, a
  game, a terminal's scrollback blit. Everything else -- chrome, widgets, labels,
  menus -- submits commands and never acquires a surface, never borrows one to
  the compositor. It is also the natural
  virtio-gpu seam: a draw list is already the shape a GPU consumes, so forwarding
  becomes a backend swap behind `SUBMIT_COMMANDS` rather than an ABI change.
- [ ] [BUG][P2] `xfer_buffer_acquire` hands out frames without zeroing them, so a
  fresh buffer can carry a previous tenant's bytes: another process's released
  buffer, a dead app's window contents, a previous FS response. The path is
  `xfer_buffer_acquire` -> `object_alloc_backing` -> `pfa_alloc_pages`, and there
  is no `memset` in `src/kernel/xfer_buffer/xfer_buffer.c` or in the frame
  allocator. Acquiring is not capability-gated (`warp_buffer_acquire` checks kind,
  size, context and `warp_buffer_role_allowed`, no DMA capability), so any process
  that may hold buffers can acquire one, map it as owner and read the recycled
  contents before writing.

  Fix is one `memset` over the freshly allocated frames in `object_alloc_backing`
  -- once at acquire, not per borrow or per map, so it costs nothing on the
  read/write/borrow paths. The value is that "a fresh buffer is empty" becomes a
  property callers may rely on.

  Graphics surfaces are the largest recycled frames in the system, so a freed
  3 MB window handed to the next app is this issue at its worst size.

  Tagged P2 because it is deliberately deferred, not because the consequence is
  small -- by this file's own tag rules a silently-unenforced isolation property is
  P1. Re-tag if guest isolation becomes a near-term goal.
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
- [ ] [BUG][P1] Apply the buffer/WARP physical-zone floor in `linmem_slot_commit`. It
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
  overlay windows against real heap growth (`link.cpp`); write-combining
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
- [ ] [BUG][P1] Restore prior linear-memory PTEs on a wasm3 `xfer_buffer` unmap
  instead of only dropping the overlay tracking entry (`src/kernel/wasm3/link.c`
  `FIXME(xfer-unmap)`).
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
- [ ] [CLEANUP][P2] Convert `src/libui/include/wasmos/libui.h` off its 0/-1
  convention onto the packed codes in `abi/errors.yaml`. The root is
  `ui_send_gfx_raw`, which returns -1 for "the call failed" and "the reply was
  the wrong type" alike; `ui_send_gfx` forwards that, and roughly 70 `ui_*` entry
  points forward it again, so an app cannot tell a transport failure from a
  refused request. `ui_window_set_title` was converted with the transfer-buffer
  migration and is the shape the rest should take: propagate the primitive's own
  code where one exists, `WASMOS_ERR_GFX_*` where the failure is libui's own, and
  return the reply `status` unmodified. All current callers discard the value, so
  the change is source-compatible; the cost is that the conversion touches every
  entry point at once, which is why it is not a rider on a caller's change.
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
  `FSMGR_BACKEND_BLOCK`/`_PSEUDO`, `VT_INPUT_MODE_*`.

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

- [x] [ENHANCEMENT][P2] Route the ATA secondary channel's interrupt (IRQ 15).
  Done. Both channels now take interrupts; the only thing that polls is a channel
  that has explicitly fallen back after its drive failed to assert.

  The comment that had justified polling claimed routing line 15 "would mean
  telling two lines apart from one event and acking the right one". The kernel
  already named the line: `irq_ops_deliver` sets `irq_msg.request_id = line`,
  `ipc_drain` records the message, and `irq_register` / `irq_ack` are per-line.
  `ata_service_irq` discarded the message with `wasmos_ipc_drain` and acked a
  hardcoded constant, which is what made it look impossible. No kernel or ABI
  change was needed.

  The subtle part, recorded because it is what makes two lines on one endpoint
  safe: an event must be handled per MESSAGE, not per drain. The old code drained
  the queue and acked once, which with two lines would ack one and leave the
  other masked -- the disk dead, the hazard the old comment named.

- [x] [BUG][P1] `fs_wfs` could not start under wasm3: its entry export declared
  four parameters and the process manager always calls an entry with argc 0, so
  the driver died at its entry call and the block rule respawned it nine times
  before giving up -- `/wfs` never mounted under the DEFAULT runtime, and every
  test that read it reported `fs failed`. Fixed by declaring `initialize(void)`,
  which `fs_fat` and `fs_init` already do. Reproduced both ways on a wasm3_smp
  tree: 9 respawns and 0 mounts without the fix, 0 and mounted with it.
- [x] [BUG][P1] The QEMU battery matrix ran ONE runtime, so a whole class of
  defect was invisible to CI: WARP does not enforce entry-export arity and
  wasm3 does, which is how the `fs_wfs` defect above shipped with CI green
  while every `/wfs` test failed. `tests/batteries.json` now carries a
  `runtimes` list beside the batteries and `.github/workflows/ci.yml` builds
  its matrix from the cross product, so every QEMU battery runs under both
  runtimes and each cell asserts the runtime it actually linked from the boot
  marker.

- [ ] [ENHANCEMENT][P2] Make `scripts/make_wasmos_app.c` refuse an entry export
  whose arity is not zero. A path-spawned entry is called with argc 0, so a
  module declaring parameters is not startable -- under wasm3 it dies at the call
  with "argument count mismatch" before its first line of output, and under WARP
  it runs by accident. The packer already reads the module and knows the entry
  name from the manifest; checking the signature turns a runtime failure that
  reads as a spawn problem into a build error naming the function.

- [ ] [ENHANCEMENT][P2] Give a guest a way to reach truncation at an arbitrary
  size. `wfs_truncate_task` now shrinks a tree-mapped object to a size INSIDE a
  block and promotes an inline object a grow takes past `WFS_INLINE_DATA_MAX`,
  but no guest API reaches either: the libc has no `ftruncate`, so `O_TRUNC` is
  the only route and it always truncates to zero, which is block aligned and
  never grows. Both paths are covered by the host suites
  (`tests/unit/test_wfs_truncate.c`) and by nothing end to end. Adding
  `ftruncate` means a host call plus the libc and libsys wrappers kept in sync
  across the runtime-specific variants, then a case in
  `examples/c/wfs_write_smoke`.
- [x] [BUG][P3] `mount` named every WFS volume `fs-fat`. Fixed: a backend now
  reports its own `FS_TYPE_*` in `FSMGR_IPC_BACKEND_INFO_RESP` arg1 and the
  listing names a filesystem from that alone (`fsmgr_backend_fs_name`, one lookup
  row per type), so the kind no longer decides a display name. initfs reports
  `FS_TYPE_INITFS` rather than being recognised by its kind, so a devfs or sysfs
  needs no branch either. The same missing identity had made the ROOT filesystem
  the first block-backed backend in slot order, which is now selected by mount
  name; and `FSMGR_BACKEND_BOOT` was renamed `FSMGR_BACKEND_BLOCK`, since reading
  it as "the volume booted from" is what produced both defects.
  `tests/unit/test_fs_manager_backends.c` plus the `mount` cases in
  `tests/test_device_manager.py` and `tests/test_wfs_virtio_blk.py`.
- [ ] [BUG][P2] The `fs.backend` class instance encodes (kind, unit) and not the
  BLOCK BACKEND, so two volumes whose units collide across backends -- ATA unit
  2 and a virtio-blk device at slot 0 function 2 -- derive one instance. The
  second registration is refused and its mount never appears. This is the defect
  the retired `block` NAME had, one layer up: a disk is (backend, unit), so the
  instance has to carry the backend. Fixing it widens the encoding, which
  fs-manager and `fs_fat` decode too (`src/drivers/fs_wfs/fs_wfs.c`).

  Carrying the backend is NOT sufficient on its own. A partition reports the same
  backend AND the same unit as the disk beneath it (see the `SUBSYSTEM=="partition"`
  note in `scripts/system/devmgr/rules/default.rules`), so a whole-disk backend and
  a partition backend on that disk still derive one instance under the widened
  encoding. The instance has to be derived from the block device's CANONICAL ID --
  which already distinguishes `block:ata:1` from `block:ata:1p1` and is what
  `wasmos_block_fingerprint` hashes for the `block` and `volume` classes -- rather
  than from any (backend, unit) pair. Not reachable today: the mounted volumes are
  units 0, 1, 2 and 48.
- [ ] [FEATURE][P3] Symlinks (spec §20). Nothing creates or resolves one:
  `WFS_TYPE_SYMLINK` is defined by the format and used by no code. The format
  stores a short target inline in the object record, so this is a namespace
  operation plus resolution in the path walker, not a layout change.
- [ ] [FEATURE][P3] Hard links (spec §19). `link_count` is maintained correctly
  -- created, incremented on a directory's `..`, decremented on unlink, and
  checked by fsck -- but nothing can create a SECOND name for one object, so the
  count never exceeds what a single link plus `.`/`..` implies. Wants an FS
  operation and an opcode; deletion already does the right thing once one
  exists.
- [ ] [ENHANCEMENT][P3] WFS timestamps are whatever the caller passes, and the
  driver passes nothing: `atime`/`mtime`/`ctime`/`btime` are written from a
  `now_ns` that is zero outside the host tools, so every object on a
  guest-written volume carries the epoch. The RTC service is the source
  (`src/drivers/fs_wfs/wfs_types.h`); it needs a driver-side clock read that
  does not cost an IPC round trip per metadata write.
- [ ] [ENHANCEMENT][P3] Record `WFS_STATE_ERROR` when an ORDINARY operation
  finds a bad checksum -- an object record, a directory tail, an extent node.
  Only a failed journal replay records it today; every other inconsistency
  reaches the caller as `WASMOS_ERR_FS_CHECKSUM` and leaves the volume writable,
  so the next mount sees nothing wrong. Needs a write from paths that are
  otherwise read-only, and a policy decision this has not made: whether one bad
  record should cost the whole volume its writability, as ext4's
  `errors=remount-ro` does (`src/drivers/fs_wfs/wfs_sync.h`).
- [ ] [ENHANCEMENT][P4] A full-block overwrite still reads the block first. The
  read is pure cost when every byte is about to be replaced, exactly as it is
  for a freshly allocated block, which already skips it
  (`src/drivers/fs_wfs/wfs_write.c`).
- [x] [ENHANCEMENT][P3] WFS mount rules named a disk and a unit
  (`DRIVER=="ata", ATTR{unit}=="2"`, `DRIVER=="virtio-blk", ATTR{unit}=="48"`),
  which is what Phase 3 set out to retire. Both now match
  `SUBSYSTEM=="volume"` on fstype and uuid and differ only in identity.

- [ ] [ENHANCEMENT][P2] Batch several WFS metadata operations into one journal
  transaction. The driver retires each transaction before the next begins
  (`wfs_journal_t`), so every metadata write pays a full descriptor, commit and
  checkpoint round trip, and the log's tail never leaves its first block. Lazy
  checkpointing would amortise that, and is what makes a CHAIN of live
  transactions possible -- which in turn needs §21's three separate passes in
  `wfs_recover.c`, since a later transaction's revoke bars an earlier one's image
  and the revoke table must be complete before any image is applied. Recovery
  refuses a chain today rather than half-applying one, so the two land together.
- [x] [FEATURE][P2] Give the block ABI a flush, so a WFS journal barrier means
  what §14 says it means. `BLOCK_IPC_FLUSH_REQ` / `BLOCK_IPC_FLUSH_RESP` are
  served by both block backends -- `ata` issues ATA CACHE FLUSH, `virtio_blk`
  negotiates `VIRTIO_BLK_F_FLUSH` and issues `VIRTIO_BLK_T_FLUSH` -- and
  `wfs_txn_commit_task` awaits one at §14's steps 2, 4 and 6, with §21's replay
  awaiting one before its tail retires the replayed writes.
- [x] [FEATURE][P2] Exclusivity: a volume is claimed while it is mounted.
  `fs_wfs` sends `VOLUME_IPC_CLAIM_REQ` on mount and releases it at shutdown;
  `fsck.wfs` refuses a claimed volume, and refuses one it cannot ask about, with
  `--force` as the documented override (`architecture/37-volume-manager.md` §5).

- [ ] [ENHANCEMENT][P3] Claim the FAT volumes too. `fs_fat` mounts `/boot` and
  `/user` without claiming them, so a tool consulting the flag sees them as idle.
  Nothing consults it for FAT today -- there is no `fsck.fat` -- which is why this
  is an enhancement and not a bug, but the flag means "a filesystem service holds
  this" and for two of the three mounts it currently does not.

- [ ] [ENHANCEMENT][P3] A guest formatter must consult the claim. `mkfs_wfs` is a
  HOST tool that writes an image file, so the sharper half of §5 -- refusing to
  format a mounted volume -- has no call site yet. Whoever adds a guest `mkfs.wfs`
  owes it the same check `fsck.wfs` makes.

- [ ] [ENHANCEMENT][P4] A claim does not catch the overlap it looks like it
  should: a disk and its partitions are distinct volumes, so claiming one does not
  mark the others. Today that is prevented by suppressing the whole-disk volume of
  a partitioned disk, not by the claim.

- [x] [BUG][P2] The partition proxy forwarded a borrow it could not lend.
  `handleTransfer` passed a client's `dst_borrow_id` through to the disk backend
  unchanged. A borrow is held per CONTEXT, so that id named a grant between the
  CLIENT and the PROXY and resolved to nothing for the disk: its zero-copy DMA
  was refused, its staged fallback wrote into a buffer it may not touch and was
  refused too, and the client got fs.IO. The proxy reborrows to the disk now.

- [x] [BUG][P2] The kernel's broker self-test yield-spun on a service from
  `/boot`. `broker_spawn_request_entry` returned `PROCESS_RUN_YIELDED` until
  `font-service` was ready -- ~10^6 dispatches, and load on the bring-up it was
  waiting for. `process_notify_ready` broadcasts on a scheduler event now and
  `process_wait_for_ready_change` parks on it.

- [x] [BUG][P2] `pm_recv_fs_reply` waited without a deadline, so a filesystem
  that accepted a spawn read and then died parked the PROCESS MANAGER for the
  rest of the boot -- every process blocked, fs-manager idle, no fault to look
  at. It gives up after PM_FS_REPLY_TIMEOUT_MS and the spawn fails with a reason
  its caller can report.
  `ipc_endpoint_wait_for` returns IPC_OK whether it was woken or timed out, so
  the deadline is kept by the caller in ticks; the wait's own timeout only bounds
  one sleep.

- [x] [ENHANCEMENT][P2] `/boot` mounts from `SUBSYSTEM=="volume", ATTR{boot}=="1"`
  -- the volume the FIRMWARE loaded this system from, carried from the
  bootloader's own device path through `boot_info` v5 and the `boot.partition`
  kernel-environment variable. NO mount rule names a disk any more.

- [x] [ENHANCEMENT][P2] Deleted `fat_try_parse_mbr`, the last partition-table
  reader outside the partition manager, with `fat_mbr_entry_t` and `tried_mbr`.
  Every mount names a VOLUME, so fs_fat is handed a device whose LBA 0 is a boot
  sector and anything else there is a fault.

- [ ] [BUG][P3] The SMP scheduler stress test can panic with a corrupted
  dispatch context. Seen twice on CI, on both SMP configs:

      wasm3_smp  vector=6 (#UD)  rip=ffffffff80000507  pid=13 name=smp-stress
      warp_smp   vector=14 (#PF) rip=ffffffff80000507  cr2=ffffffffff802312

  Both RIPs are KERNEL_HIGHER_HALF_BASE plus a tiny offset -- a small integer
  used as an address and OR'd with the base -- and both backtraces sit in
  `process_schedule_once`. So a thread is dispatched onto a corrupted context
  rather than faulting on a bad access. The tree already counts the neighbouring
  hazards (`enqueue non-ready`, `double-link`, `dispatch-left-stranded`).

  EXPOSED, and probably not caused, by making `process_notify_ready` broadcast on
  a scheduler event: that put cross-CPU wake traffic on a path every service
  touches during boot, and both SMP configs went from green on every push to
  failing roughly one run in two. Reverting to a bounded park removed the
  failures. The wake path took `ev->lock` before the run-queue lock, which is the
  order every other event user follows, so it introduced no new inversion --
  which is what makes exposure the better reading.

  Not reproducible here: six local runs before the revert and one after all
  passed, and Apple Silicon cannot host MTTCG x86, where every memory-ordering
  race in this tree has lived. Needs a Linux x86 `-smp 4` soak with the broadcast
  restored to decide it.

- [ ] [ENHANCEMENT][P4] `fat_mount_t.boot_lba` is always 0 now that no mount
  starts from a partition table, but it is still added into every FAT, root-dir
  and FSInfo LBA. Removing it simplifies that arithmetic. `is_partition` survives
  on the same terms -- kept for diagnostics, decisive for nothing.
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
- [ ] [ENHANCEMENT][P3] Widen file offsets past 2 GiB across the FS ABI. `lseek`
  refuses any offset outside `INT32_MIN..INT32_MAX`
  (`src/libc/src/unistd.c:370-380`) and `stat` carries size as `int32_t`, so a
  file above 2 GiB is unobservable to an application regardless of what the
  backend holds. IPC carries four 32-bit arguments, so widening means either a
  lo/hi split (as the RTC opcodes already do for time) or a request struct
  staged in a transfer buffer. Blocks no current backend — FAT12/16 cannot reach
  the ceiling — but it caps any format that can, including the WFS proposal
  (`docs/WFS_WASMOS_FILE_SYSTEM.md`, section 22).
- [x] [BUG][P1] A spawned utility did not inherit its spawner's working
  directory, and a fallback in fs-manager hid it for every mount except the first
  boot-kind one. FIXED: the working directory is now a full canonical VFS path
  owned by fs-manager.

  fs-manager held a working directory as `(mount, depth)` and forwarded a
  relative name to a backend UNRESOLVED, choosing that backend by falling back to
  `backend_first_of_kind(FSMGR_BACKEND_BLOCK)` when the client had none. For
  `/boot` and `/init` that fallback is the correct backend, so relative `cat`
  worked there BY ACCIDENT; for any other mount the request went to the wrong
  backend, which answered NOT_FOUND, and the real backend was never asked. The
  fallback was the missing check: it turned "this client has no working
  directory" into a plausible answer instead of an error, which is why the broken
  inheritance stayed invisible while there was only one non-root mount.

  What landed: `fs_client_state_t` carries the full path (`fsmgr_cwd_join`
  canonicalizes, host-unit-tested in `tests/unit/test_fs_manager_path.c`), every
  client path is joined onto it before routing, `FSMGR_IPC_CLONE_CWD` copies the
  path rather than a `(mount, depth)` pair, and choosing a backend moved out of
  "this client named no mount" and into routing an ABSOLUTE path
  (`route_absolute_path`), where a first segment matching no mount means the boot
  volume as the root filesystem — the spelling `/system/utils/ip` and
  `/apps/calculator` rely on, and a rule rather than a guess.
  `FS_IPC_CHDIR` reports the resolved path back to the client, so the CLI's
  `g_cwd` is adopted rather than re-derived and a prompt cannot disagree with the
  FS layer. A path-less `READDIR` is preceded by a re-assertion of the requesting
  client's directory, because a backend holds one current directory per
  fs-manager connection and cannot tell two clients apart.

  Two tests changed shape rather than expectation. `test_cat_startup`
  (`tests/test_cli.py`) named no directory and passed via the fallback; it now
  states the one its relative name is resolved against, like every other case in
  that file. `test_reading_a_file_by_relative_name`
  (`tests/test_wfs_mount_read.py`) was marked `expectedFailure`, which reported
  the battery green over a live bug; the marker is gone and the test is a plain
  passing case.

- [x] [BUG][P2] `FS_IPC_CHDIR_REQ` packed its target into arg0..arg3, capping a
  path component at 15 bytes plus a NUL. FIXED: the target travels as a path in a
  transfer buffer, `arg0` = length / `arg2` = buffer id / `arg3` = the grant, the
  same transport `FS_IPC_OPEN_REQ` uses.

  A directory whose name was longer could not be expressed at all: the request
  arrived TRUNCATED, the lookup missed, and the client was told the directory did
  not exist rather than that its name did not fit — and a truncated name could
  also match a DIFFERENT directory sharing the first 15 bytes. Depth is no longer
  capped either, so `cd /boot/foo/bar` is one request; the CLI's
  `PENDING_CD_CHAIN` split, which hit the same packing limit on each piece, is
  deleted along with the CLI's duplicate path normalizer. fs_wfs resolves the
  target through the same path walker OPEN uses (255-byte `WFS_NAME_MAX` names
  included), fs_fat reads it into a `FAT_MAX_PATH` buffer and reports
  PATH_TOO_LONG rather than NOT_FOUND for a component that does not fit, and
  fs_init reads it through `copy_path_from_xfer_buffer`. Covered by
  `test_cd_into_a_directory_whose_name_exceeds_fifteen_bytes` and
  `test_cd_a_deep_path_in_one_command`.

- [ ] [FEATURE][P3] Widen the orderly shutdown beyond WFS. The mechanism is in
  place -- `WASMOS_IPC_SHUTDOWN_REQ` / `_DONE`, the process manager's sequence in
  reverse spawn order, the halt/reboot host calls in both runtimes -- and
  `fs-wfs` uses it to record `WFS_STATE_CLEAN`. What remains is other
  participants declaring `WASMOS_SVC_FLAG_WANTS_SHUTDOWN` when they gain state
  worth flushing: `fs-fat`'s dirty sectors and FSInfo free count are the known
  case. Nothing needs it today, which is why this sits low.

  The flag is opt-IN because the sequence is sequential -- a participant may need
  the services beneath it while it quiesces -- so a participant with nothing to
  persist would spend its deadline saying so. With ~29 registered services and
  one shared async runner (`virtio_blk` alone), notifying every one of them would
  have added roughly a minute to every halt.

- [ ] [BUG][P2] `run-qemu-test` flakes roughly 1 run in 3 on the WARP build, in
  TWO distinct shapes. Both were seen while landing WFS work that reaches no boot
  artifact (no app target, no manifest, no device-manager rule, absent from
  `build/esp`), so neither is attributed to it.

  Shape 1: `FAIL: calculator did not fully initialise`. The guest prints
  `[calculator] start` and the harness times out before `[calculator] ready`.

  Shape 2: no `FAIL:` line at all and no `halt`. The guest reaches
  `[calculator] ready` and the harness's typed `halt` never takes effect, so it
  times out with the log ending mid-session. This is the serial-input path, the
  same family as the drain-and-discard bug fixed in 24afe3fc4c.

  Everything earlier in the boot — script broker, gfx smoke, compositor
  handshake, CLI banner — is identical in passing and failing logs for both.
  Capture a failing run with the FULL log (a `tail` loses the signal, which cost
  two runs to learn), and check whether the harness deadlines are simply too
  tight under MTTCG before treating either as a guest bug.
- [ ] [ENHANCEMENT][P3] Widen the block layer's 32-bit LBA. `fat_block_t` carries
  `uint32_t wait_lba` / `loaded_lba` with a `0xFFFFFFFF` sentinel
  (`src/drivers/fs_fat/fat_block.h`) and `BLOCK_IPC_READ_ZC_REQ` already spends
  all four IPC arguments, so a 64-bit LBA needs a new opcode shape. At 512-byte
  sectors the current ceiling is 2 TiB.


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

- [ ] [BUG][P2] The CLI can lose the ability to resolve ANY command mid-session:
  after two successful `blkinfo` execs it answered `no such command found:
  blkinfo` to every later attempt, permanently and within 20 ms — too fast to be
  a timed-out FS request. Captured once under `wasm3_smp` in the filesystem
  battery (run 33324196644 job 99291355777, `test_virtio_blk`), green on the
  rerun of the same commit and on 8 local runs (1, 4 and 16 vCPU), so it is
  load-dependent, not deterministic.

  `cli_resolve_exec_path` fails only when every PATH candidate's `fopen` fails,
  and `open()` fails fast when `libc_fs_stage_path` cannot acquire a transfer
  buffer — so the suspect is a per-context transfer-buffer leak on the exec
  path, not the FS lookup itself. `g_pending_spawn_bid` is released when the
  PENDING_EXEC reply arrives (`cli.c`); an exec whose reply is dropped or
  mismatched leaks its buffer, and the CLI acquires one per exec. Confirm by
  counting a context's live buffers across execs before changing anything.

- [ ] [BUG][P2] `gfx-smoke` can page-fault the kernel inside WARP's linear-memory
  growth. Captured once locally on `warp_smp` (`run-qemu-test`, 4 vCPU) right
  after `[test] gfx smoke visible done`, green on 4 further runs of the same
  tree, so it is load-dependent rather than deterministic.

  The fault is a kernel `#PF` with `err=0` (a not-present read) at
  `cr2=0xffffffff84746000`, inside `memcpy` called from `warp_krealloc` —
  `ActiveMemoryManager::probe` → `ensureLinearSize` → `ensureCapacityForLinearSize`
  → `ExtendableMemory::extensionRequest` → `WasmModule::runtimeMemoryAllocFnc`.
  So the guest asked to grow its linear memory and the copy into the new
  allocation walked off the end of a mapping, which points at the reallocation
  itself rather than at anything gfx-specific; `gfx-smoke` is simply the guest
  that grows.

  Note this is the reserve-then-commit linmem path (`architecture/06`), which is
  supposed to make a grow non-relocating — a `krealloc` + `memcpy` in the growth
  path is itself worth explaining before diagnosing the fault. A second CPU was
  concurrently in `warp_sync_linmem_for_pid`, which is where to look first.

- [x] [FEATURE][P2] Back the VFS root with a real filesystem (a tmpfs), so a mount
  point is a DIRECTORY rather than a reserved top-level name. DONE.

  `fs-manager` holds a mount as an absolute canonical PATH and routes a request to
  the longest such path prefixing it on a whole-segment boundary, so `/` is the
  owner of last resort, `/wfs` does not own `/wfsx`, and `/mnt/usb` outranks
  `/mnt` with no special case. A mount POINT is a directory `fs-manager` creates
  in the filesystem covering it, ancestors included, which retired
  `send_virtual_root_listing`: `ls /` is an ordinary forwarded readdir whose
  entries the root filesystem holds.

  A mount SHADOWS the directory it covers, as on Linux: the mount point is an
  empty directory in the covering filesystem and a path under it reaches the
  mount, so the covered contents are unreachable while the mount stands. That
  keeps mounting a property of the namespace rather than of the covered
  filesystem's state, so a mount cannot fail because someone left a file behind.

  Covered by `tests/unit/test_fs_manager_path.c` (routing, 28 cases),
  `tests/unit/test_fs_manager_backends.c` (mount-path normalization) and
  `tests/test_vfs_root_mount.py` (end to end, filesystem battery). The write side
  is driven by `src/utils/mkdir/`, added because the CLI had no way to create a
  directory at all -- so nothing could demonstrate that `/` was writable rather
  than merely listable.

  REMAINING:
  - `FSMGR_CWD_MAX` is 128 bytes, which bounds the working directory a client can
    hold for EVERY mount -- a path of maximum-length components is unreachable on
    WFS for the same reason. Widen it.
  - DONE: a boot rule carries `ENV{MOUNT}` now, so a filesystem with no backing
    device can be placed by rule. `parse_always_spawn_rule_line` reads it and the
    spawn delivers it as a `mount=` startup argument over the PATH opcode, which
    is the only one that carries arguments. Two instances are placed this way,
    `/home/user` and `/wfs/nested`.
  - The tmpfs `NAME_MAX` is 255 and its names live in the node record, so the
    table costs 50 KiB whether names are long or not. Variable-length names in a
    cell arena would not, at the cost of an allocator with reuse across rename and
    unlink.
  - DONE: mounting at DEPTH and mounting INSIDE another mount are both
    demonstrated, by two rule-placed tmpfs instances that need no disk.
    `/home/user` has its ancestors created as directories in the root filesystem;
    `/wfs/nested` has its mount point created inside the WFS VOLUME, which is the
    other branch of `fsmgr_ensure_mount_points`.
  - DONE: SHADOWING is demonstrated in BOTH directions. `scripts/wfs/nested/covered.txt`
    is in the WFS image, and the tmpfs mounted over `/wfs/nested` hides it --
    verified by a CONTROL run with the mount disabled, where the file is listed,
    so the case measures the mount rather than an absent directory.
    `FSMGR_IPC_UNMOUNT_REQ` removes the mount and the file comes back, which is
    the half that was previously only construction.
  - DONE: mounting is a REQUEST. `FSMGR_IPC_MOUNT_REQ` carries a
    `type=`/`mount=`/`source=` descriptor and has the process manager spawn the
    driver, so placement is one mechanism whether it comes from a boot rule or a
    request. Its reply is DEFERRED because the process manager reads the driver
    module through fs-manager -- see `docs/architecture/18-filesystem-stack.md`.
  - fs-manager still BLOCKS on other nested calls, and every one is the same
    latent deadlock the mount request had to be built around: `forward_request`,
    `backend_stat_dir`, `fsmgr_pull_backend` and `fsmgr_backend_mkdir` all park
    fs-manager on a reply while it is the service everything else needs to read a
    file. They are safe TODAY only because the peers they wait on (filesystem
    backends) do not themselves need the filesystem. The general fix is the async
    service runtime (`docs/architecture/32-*`), which fs-manager does not use.

    The PROTOCOL obstacle to that is gone: backends no longer hold a working
    directory, so no pair of requests has to stay adjacent and concurrency is no
    longer a correctness question. What remains is the conversion itself, and on
    a wasm guest that means stackless state machines -- doc 32 §52 (stackful
    coroutines for wasm guests, via suspension at the host-call boundary) is a
    spike, not implemented.
  - The backend PROCESS survives its unmount. fs-manager quiesces it
    (`WASMOS_IPC_SHUTDOWN_REQ` with `WASMOS_SHUTDOWN_REASON_UNMOUNT`) and drops
    the table entry, but the driver keeps running and holding a process slot, so
    repeated mount/unmount cycles leak slots. Exiting needs a process-exit path a
    driver can call after answering DONE, which no driver has today.
    (`src/services/fs_manager/fs_manager.c`, the TODO above `handle_unmount_req`.)

- [ ] [BUG][P2] fs-manager never releases a client's state. `client_state()`
  allocates a `fs_client_state_t` on first contact from a context and nothing
  ever clears `in_use`: the chunk list grows for the lifetime of the system, one
  entry per process that has ever touched the filesystem, each holding a cwd
  string and an fd table. A shell session that spawns utilities leaks one entry
  per spawn.

  It also costs a correctness property rather than only memory. `umount` refuses
  a mount that still has an OPEN FILE on it, and Linux additionally refuses one
  that a process is standing in; the second rule is deliberately NOT implemented
  here (`fsmgr_mount_busy_reason`) because a cwd recorded by a process that has
  since exited would refuse the unmount forever -- a single `cat` run inside a
  mount would make it permanently unremovable. The open-file rule has the same
  staleness and is kept only because a client that exits normally closes its fds.

  The blocker is that nothing tells fs-manager a client died: there is no exit
  notification opcode, and a service cannot ask whether a context is still alive.
  Either would do -- a PM broadcast on process exit that interested services
  subscribe to, or a liveness query fs-manager sweeps with. Once one exists,
  reap the state and count the working directory in the busy rule.

- [ ] [REFACTOR][P2] Remove the PROCESS_MAX_COUNT ceiling without giving up slot
  stability. The count is a compile-time guess a boot has to fit under, and it has
  already cost one regression: at 48 the ring3 boot tree sat exactly on it, so
  adding one long-lived driver broke `run-qemu-ring3-test`. Raising it to 64 buys
  time, not a fix.

  An id-map is NOT the replacement. `g_processes[]` is not a fixed array for
  lookup speed -- it is fixed so that a POINTER into it stays valid:
  `process_find_by_pid` runs LOCK-FREE in the dispatch hot path, and
  `cpu_local()->current_process` caches a raw `process_t*` across operations. A
  container that rehashes or reallocates breaks exactly that, which is the hard
  half; the lookup is the easy half.

  The reach is also wider than the process table. Nine files size a PARALLEL
  slot-indexed table from the same macro -- `wasm3/link.c` (`g_wasm_last_slots`,
  `g_wasm_block_slots`, `g_wasm_fs_peer_slots`), `wasm3/shim.c`, `wasm_driver.c`,
  `warp/link.cpp` (`PROCESS_MAX_COUNT * 32` overlay slots), `syscall.c`,
  `native_driver.c` -- so swapping the process table alone fixes the wrong half
  and leaves every one of those still bounded.

  The shape that keeps the invariants is CHUNKED slabs, the pattern
  `fs_manager`'s client-state list already uses in this tree: a chunk is
  allocated once from `kmem_alloc` and never moves, so pointers stay valid and the
  lock-free reader survives, while the table grows on demand. The per-runtime
  parallel tables want folding into the process record (or into their own chunk
  list) in the same pass, which is what makes this a piece of work rather than a
  one-liner.

  Mitigated meanwhile: `process_find_slot` reports the table filling at 3/4
  occupancy and reports exhaustion, so the ceiling warns before it bites.

- [ ] [BUG][P3] A refused service-class claim gets NO REPLY, so the registering
  driver blocks in its bring-up call forever instead of learning it lost. The
  class registry refuses a second owner claiming a live `(class, instance)`
  (`service_class_registry_add`), and `pm_handle_register_desc` answers that — and
  every other failure on the path, including the capability check — by returning
  without sending a message (`src/kernel/process_manager_services.c`). A driver
  calls this synchronously during bring-up (`driver.call`, `wasmos_svc_register`)
  and has no timeout, so a duplicate instance is a hang rather than a diagnosable
  failure. Answer with `SVC_IPC_ERROR` carrying a packed `WASMOS_ERR_PROC_*`.

  Reachable today only by a genuine instance collision, which every shipped
  provider derives so as to avoid; it becomes reachable the moment two providers
  disagree about an identity.

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
  half-started system — a since-retired IPC lifecycle test failed that way. Needs the
  readiness contract to stop keying off the first prompt first.

Other graphics/VT/UI:

- [x] [BUG][P1] Reclaim old libui font buffers when text buffers grow, and the
  compositor's title-glyph buffers on growth. DONE with the transfer-buffer
  migration: `ui_font_ensure_buffer` and the compositor's `ensure_font_buffer`
  both release the previous buffer before publishing the new one, and the
  release cascade-revokes its borrow, so neither an id nor a grant is stranded.
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
  `ui_menu_item_destroy_data` releases only its own surface, leaking the
  compositor window (`src/libui/include/wasmos/libui_menu_item.h` `FIXME`).
- [ ] [BUG][P3] Add the '9' glyph to `drawDigit3x5`; the table holds 0-8 and the guard
  rejects 9 (`src/libui/assemblyscript/libui.ts`).
- [ ] [CLEANUP][P3] Remove the no-op self-assignment `d->list.capacity = d->list.capacity`
  (`src/libui/include/wasmos/libui_dropdown.h:69` `FIXME`).
- [x] [BUG][P1] Flush after the tetris back-buffer blit under wasm3, or gate the
  app on WARP. MOOT. Tetris presents into a transfer buffer it owns, and
  `xfer_buffer_map` places that buffer's own frames in linear memory on BOTH
  runtimes (`tests/test_xfer_map_alias.py`), so the blit reaches the compositor
  with no write-back call on either. The runtime-specific hazard was the shmem
  overlay's, and it went with it.


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

  REOPENED. Third capture, run 33324196644 job 99291341629 (`warp_smp`,
  `a=0x0e`), and again on the rerun of the same commit, against a tree whose
  only delta from a green `warp_smp` job on main is documentation. Unlike the
  two earlier captures the panicking CPU resumed `rip=0`, not an address inside
  `g_cpus`: CPU 0 `pid=0 tid=0 rip=0000000000000000`, CPU 1 inside the WARP JIT
  (`vb::Frontend::startCompilation` under `wasm_driver_start` <- `pm_app_entry`),
  CPU 2 in `process_trampoline`, CPU 3 spinning in `spinlock_lock`. A null rip
  is a dispatch that resumed a context whose saved rip was never written, which
  the claim fix does not cover. Not reproducible on a fast 8-core host: 3/3
  clean stress boots there, so a slower 4-vCPU runner is part of the trigger.

  Reproducing needs a Linux x86 runner; MTTCG on Apple Silicon masks it. Force
  TCG (`WASMOS_QEMU_ACCEL=tcg`) and reset the OVMF vars file between boots:

      cmake -S . -B build-warp_smp -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig \
            -DWASMOS_SCHED_SMP_STRESS=ON
      cmake --build build-warp_smp --target run-qemu-sched-stress-test

  A `[sched] claim lost` line means the race fired and the claim resolved it.

- [ ] [BUG][P0] The guest reaches no CLI prompt on roughly one CI run in two, and
  it is ONE defect with several faces, not several flakes. Every capture is a
  whole-session stall: the tests that report it differ only by which one happened
  to open the session first, so the battery that goes red is arbitrary.

  Faces seen so far, all on the same code:
  - `warp_smp`: `FAIL: calculator did not fully initialise`. The calculator is not
    involved -- the log's last line is `[net-stack] tls: no CA trust store` and
    nothing after net-stack is ever spawned (no `gfx-smoke`, no `Calculator`, no
    `cli`). Runs 32625904585 (`main` at 78018f8191) and 32634831416.
  - `boot-and-init`: `ERROR: setUpClass ... RuntimeError: CLI prompt not detected`
    (`tests/test_init_smoke.py:24`). Run 32636373451. This is the capture that
    carries a `[stall-dump]`; see the wedge item below.
  - `networking`: `ERROR: setUpClass ... RuntimeError: CLI prompt not reached`
    (`tests/test_net_stack_udp_echo_e2e.py:43`, run 32636373451) and
    `test_host_resolves_localhost` timing out for 120 s on its FIRST prompt wait
    (`tests/test_net_stack_dns_resolve_e2e.py:40`, run 32634831416). Neither is a
    DNS or UDP defect: both fail before their subject matter runs.

  An ERROR in `setUpClass` and an assertion failure are different animals, and
  the distinction is the fastest triage available: `setUpClass` failing means the
  session never came up, so the named test is a bystander. Read the battery name
  as "which suite drew the short straw", never as the subsystem at fault.

  Attribution is settled and is worth stating because it looks like a regression
  from the scheduler work: PR #18 was DOCUMENTATION ONLY, and `warp_smp` went from
  passing on PR #17's merge to failing on #18's. The trigger is runner timing.

  The mechanism is the wedge item below, whose second cause now has a persistent
  signature again. Fix that; this item is the symptom's index.

  Rate, on four CI runs whose kernels are identical in every respect that could
  matter: `warp_smp` failed, failed, passed, failed. The battery that reports the
  stall moves between runs; the stall itself is close to a coin flip.

  Where to look for evidence, because the arms are not equivalent: only the
  BATTERY tests produce a `[stall-dump]`, because the dump comes from the Python
  framework's stall handling. The `warp_smp` build+boot arm runs the plain
  `run-qemu-test` halt check, so it reports `FAIL: calculator did not fully
  initialise` and nothing else -- three captures of it carry no thread state at
  all. Chase this through `boot-and-init` or `networking` failures, not through
  `warp_smp`, however tempting its name is.

  Reproduces on CI and probably not on an Apple Silicon box: QEMU there runs
  `thread=single`, which masks memory-ordering races. Validate on Linux x86 with
  `-smp 4` over repeated boots.

  A FIFTH face, and the first one that is not silence: the session stays fully
  alive and the boot spins on a filesystem read it can never complete. Two
  captures, both `warp_smp`, on `feat/vfs-tmpfs-root`:

  - Local (Linux, TCG, `-smp 4`), `boot-and-init`: 20,449 consecutive
    `[pm] spawn_path fs read failed:` lines, first for
    `/boot/system/drivers/fs_wfs.wap` immediately after `[fat] fs.backend
    registered`, then for `/boot/system/services/sysinit.wap` forever. The
    thread table is HEALTHY -- 27 live, 22 blocked on their own endpoints,
    `stranded(ready,no-rq)=0`, no `[diag]!    refused`, every `wait=select:`
    line `q=0`. So none of the four causes above is present. init (tid=2) is
    READY on a run queue with `disp=874454` and `ticks=3781`: it is not stuck,
    it is retrying.
  - CI run 33436808726 job 99635195798, `filesystem`: 14,829 of the same line,
    and this one carries the cause immediately before the first failure --
    `[pm] fs reply timed out; the filesystem never answered`
    (`PM_FS_REPLY_TIMEOUT_MS`, 15 s) on `/boot/system/drivers/serial.wap`, while
    volume-manager was probing a GPT on `block:virtio-blk:40`.

  So the symptom's shape is: ONE filesystem read gets no answer, and the caller
  retries the spawn forever at full speed. That the local capture reaches the
  same storm WITHOUT a timeout line means at least one fast failure path gets
  there too, which is why the log line now reports which of its four outcomes
  fired and, for `FS_IPC_ERROR`, the backend's packed code
  (`pm_fs_read_blob_for_spawn`); before that every one of those 20,449 lines was
  the same unattributable string. Take the next capture with that in hand rather
  than re-deriving it.

  What this does NOT establish: why the filesystem stops answering, and whether
  the unbounded retry is merely amplifying a transient or is itself holding the
  system away from recovery. The retry has no backoff and no give-up.
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

- [ ] [BUG][P0] Confirm the whole-session wedge stays fixed. It has had FOUR
  causes, each fixed, and finding each only because the previous one was gone is
  why this item is never closed on a green run. The fourth was diagnosed and fixed
  on 2026-08-23 and needs the confirmation runs described at the end.

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
  4. `sched_wake_thread`'s claim-lost arm marked the target READY and returned
     without leaving a CLAIM. When `sched_wake_claim_enqueue` reports that the
     completion path owns the enqueue, that only holds while the completion path
     has not yet made its decision; once it has -- it clears
     `blocking_transition`, takes the token, reads the state, sees BLOCKED and
     correctly declines to enqueue a blocked thread -- the mark lands after the
     last thing that would have acted on it. A mark is not a message, which is
     the identical defect the enqueue-current path in `cpu_sched_enqueue` already
     carried a comment about, fixed the same way: publish `sched_owe_enqueue`
     alongside the mark. Its signature is a persistent `stranded(ready,no-rq)`
     whose `[diag]   ready_by=` line names `sched_wake_thread`.

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

  That test is now MET, in run 32636373451 (`boot-and-init`, WARP SMP):

      live=28 ready=3 blocked=21 stranded(ready,no-rq)=1 running-unclaimed=2
      [diag]! tid=46 pid=37 gfx-compositor st=ready rsn=- rq=0 wake=0 btrans=0
              ev=0 cpu=2 ticks=4 disp=1241

  Both lines are IDENTICAL in all FIVE samples: the strand persists and
  gfx-compositor's `disp` never leaves 1241, so it is not an unsettled claim. The
  thread is READY, on no run queue, with no wake token (`wake=0`) and no blocking
  transition (`btrans=0`) -- nothing owes it an enqueue, so nothing will ever
  perform one. Behind it sysinit parks on `endpoint:84` for a reply to its
  `type=0x209 req=4` sent to `ep:5`, and the boot never reaches the CLI.

  The other two causes read CLEAN in that capture, which is what points at the
  second: no `[diag]!    refused` line anywhere, and no parked select waiter
  watching a `q>0` endpoint (every `wait=select:` line shows `q=0`).

  What is NOT established: which site left the strand. The capture does carry an
  `[sched] enqueue current ... caller=ffffffff802229ef` line, which is the field
  that distinguishes the two copies of the running-elsewhere guard -- but it names
  `tid=18`, not the stranded `tid=46`, so it does not attribute this strand and
  must not be read as if it did. Resolving that caller address needs the `nm`
  output of the CI build's `kernel.elf`, which the run does not retain; a local
  `-smp 4` reproduction on Linux x86 is the way in.

  Start from `thread_t::enqueue_owed` and the claim protocol in
  `sched_wake_claim_enqueue` / `sched_settle_deferred_enqueue`: a thread that is
  READY with `wake=0` and `enqueue_owed` clear is precisely the "a mark is not a
  message" failure those were written to close.

  The dump now reports that field as `owed=` on every thread line, and two further
  captures have read it. Both say `owed=0`:

      run 32637549686, scheduler-and-ipc (bystander: test_work_stealing)
      [diag]! tid=46 pid=37 gfx-compositor st=ready rq=0 owed=0 wake=0 btrans=0
              ev=0 cpu=0 ticks=3 disp=1223            -- identical in 4 samples

      run 32637549686, language-runtimes (bystander: test_hello_as)
      [diag]! tid=46 pid=37 gfx-compositor st=ready rq=0 owed=0 wake=0 btrans=0
              ev=0 cpu=2 ticks=2 disp=679             -- identical in 5 samples

  ESTABLISHED by those two plus the earlier one: the strand is always the SAME
  thread -- gfx-compositor, tid=46, pid=37 -- across three captures in three
  different batteries; it is READY, on no run queue, with no wake token AND no
  owed enqueue; and `disp` never moves across a capture's samples, so it is not an
  unsettled claim caught mid-flight. Nothing owes it an enqueue, so nothing will
  ever perform one.

  CORRECTED, and the correction matters more than the original claim: the strand
  does NOT by itself wedge a session. Run 32640235473 (`networking`, on
  c62da74c64) reports the identical strand in two dumps -- `disp=31` and `disp=47`
  -- with ZERO `setUpClass` errors and 85 `wamos>` prompts in the log. That session
  booted and ran its suite with the compositor already stranded, failing only on
  two ordinary assertion flakes (`test_lwip_arp_roundtrip`, the documented ~1-in-5
  tx_slots framing case, and `test_link_down_up_preserves_netif`).

  So the strand is necessary but not sufficient, and that resolves the shape of the
  whole symptom: the compositor is stranded EARLY and fairly often (`disp` in the
  30s-40s), and whether the session wedges depends on whether anything waits on the
  compositor before the test's prompt. A battery that never touches graphics
  finishes; one that does, stalls. That is why the failure moves between batteries
  and why it looks like a coin flip.

  Do not write "the strand causes the stall" again without a capture showing a
  waiter on the compositor -- an earlier revision of this entry did, reasoning from
  four correlated captures with no counter-example. Run 32640235473 is that
  counter-example.

  REFUTED, and recorded because the refutation is the useful part. Two mechanisms
  have been proposed; neither survived.

  1. The claim consumers (`sched_settle_deferred_enqueue`,
     `sched_sweep_owed_enqueues`) take the claim before validating and destroy it
     when the validation fails. THIS WAS REFUTED IN ERROR AND IS THE LIVE
     SUSPECT -- see the correction below. The refutation argued that everything
     downstream enqueues unconditionally, which is true of the DISPATCH exits (the
     `PROCESS_RUN_BLOCKED` branch enqueues on READY and even repairs a
     still-RUNNING legacy yielder; the yield branch enqueues with only an
     `is_idle` guard) and false of the consumers themselves, which is where the
     debt is destroyed. Refuting a mechanism by checking the wrong code path is
     the single most expensive mistake made in this investigation.
  2. An aborted dispatch. `dispatch_done` never re-enqueues, and every path
     reaching it has already had the thread unlinked, so an abort does leave
     exactly the captured state -- demonstrated on the host by
     `s_aborted_dispatch_leaves_its_thread_reachable`, which measures
     `state=1 on_rq=0 owed=0 wake=0`. But the `SCHED_DEBUG_DISPATCH_LEFT_STRANDED`
     tripwire added for precisely this did NOT fire in run 32640235473, which
     carries the strand. Reproducing a state is not the same as being its cause.

  DIAGNOSED AND FIXED, 2026-08-23, in three steps each of which needed the
  previous one's instrumentation. The chain is recorded because two hypotheses
  were refuted along the way and the refutations cost less than the guesses would
  have:

  1. `owed=` on every thread line said the strand was owed nothing, ruling out a
     dropped hand-off.
  2. `SCHED_DEBUG_DISPATCH_LEFT_STRANDED` said `rc=7` -- the NORMAL dispatch exit,
     via `PROCESS_RUN_BLOCKED` -- with no enqueue ever attempted (every skip
     reason logs on its first hit, and none did). So the promotion happened
     outside the dispatch, after that branch had read BLOCKED and correctly
     declined.
  3. `thread_t::ready_by` named the promoter outright:
     `ready_by=ffffffff80227a12 (sched_wake_thread)`, on the ata driver's thread
     at `disp=85`, identical across four samples.

  That is cause 4 in the wedge item above, and the fix is one line:
  `sched_wake_thread`'s claim-lost arm now publishes `sched_owe_enqueue` alongside
  its READY mark, which is what the enqueue-current path in `cpu_sched_enqueue`
  has always done for the same reason. Pinned by "a wake that declines to enqueue
  left a claim rather than only a mark" in `tests/unit/test_process_lifecycle.c`,
  red before the fix and green after.

  READ THE TRIPWIRE CAREFULLY, because it has a known false positive and an
  earlier revision of this entry over-read it. It samples at `dispatch_done`, so a
  waker on another CPU that promotes a thread and enqueues it a moment later is
  seen as READY-and-unqueued in between and reported. Counts therefore OVERSTATE
  strands: one capture reported three hits of which only one matched the dump's
  persistent strand. The real signal is the dump's `[diag]!` line holding across
  every sample with `disp` frozen -- a tripwire hit alone proves nothing. The
  claim that the strand "fires several times per boot" came from reading the
  counts as strands and is withdrawn.

  CORRECTION, 2026-08-23, and it reinstates suspect 1. The strand that survives
  the boundary repair names a third promoter:

      [diag]! tid=31 pid=22 ata st=ready rq=0 owed=0 wake=0   (four samples)
      ready_by=ffffffff80226d91 (sched_mark_ready_if_live)

  All THREE callers of `sched_mark_ready_if_live` pair it with
  `sched_owe_enqueue` -- the two enqueue-current arms in `cpu_sched_enqueue` and,
  since 1e46fac0aa, `sched_wake_thread`'s deferral arm. A thread promoted by that
  function therefore always has a debt published. Observing it with `owed=0`
  means the debt was published and then DESTROYED by a consumer that did not act
  on it, which is suspect 1 above.

  The mechanism, end to end: a waker marks the thread READY and owes the enqueue
  (correct); a consumer takes the debt -- `sched_take_owed_enqueue` exchanges it
  to 0 and decrements `g_enqueue_owed_count` -- and then finds the thread
  momentarily not enqueueable (RUNNING on another CPU, or already queued) and
  returns; the thread later settles as READY with no debt, no token and no queue.
  Nothing can recover it, because the sweep's own gate is the counter the
  consumer just decremented.

  READ THIS BEFORE TRUSTING ANY NEGATIVE BELOW. Two independent reviews found that
  the reasoning in this entry rested on inferences that are not sound, and the
  errors matter more than the conclusions did:

  1. EVERY "the tripwire never fired for X" and "no skip line ever names X" claim
     is void. `sched_debug_note` rate-limits on a GLOBAL per-event counter and logs
     only at powers of two (`sched_thread.c`, `sched.h`), so with 13-28 hits per
     boot roughly six print. "Never printed" was never "never happened", and a
     victim-specific hit is unlikely to be among the six. `SCHED_R_STALE` was
     "ruled out" on exactly this basis and is NOT ruled out.
  2. The kernel is built with NO `-O` flag, i.e. -O0 (`CMakeLists.txt`,
     `CFLAGS_COMMON`), so nothing is inlined and every static function has its own
     out-of-line symbol. The hedge that a promoter was "consistent with either
     after inlining" was false. Worse, FIVE distinct READY-promotion sites live
     inside `process_schedule_once_impl` and symbolize to that one name; the
     recorded OFFSET discriminates them and was ignored in favour of the symbol.
     `0x...80222ada` resolves locally to the `thread_set_state(READY)` call in the
     PROCESS_RUN_BLOCKED completion path -- a site whose next instructions DO call
     `sched_enqueue_thread`. Re-resolve against the CI `kernel.elf` before acting,
     since the layouts differ.
  3. `ready_by` structurally cannot name a stranding site of the "already READY,
     then nobody enqueued" shape: `thread_note_ready_by` is only called when the
     state actually CHANGES, and `sched_mark_ready_if_live` returns 1 without
     touching it for a thread already READY. So it names the earlier, innocent
     promoter. The "decisive experiment" of watching `ready_by` move after
     changing `process.c:928`/`:3537` was therefore invalid -- neither site writes
     READY, so it could not move even if those were the bug.
  4. `kpanic_symbolize` has no upper bound on a match, so any higher-half garbage
     resolves to a confident function name. At least one quoted value
     (`0x...80227a12`) is mid-prologue and cannot be a return address. Validate a
     `ready_by` as a post-call address before using it as evidence.
  5. "Persistent across all samples with `disp` frozen" does NOT distinguish a
     permanent strand from a pick -> abort -> re-enqueue loop: `dispatch_count` is
     incremented only after `context_switch_high` returns, so an aborting loop
     leaves `disp` frozen too. A livelock and a strand need different fixes.

  The two-site hypothesis (`process.c:928`, `:3537`) is REFUTED, by the seq-cst
  total order: both mark READY BEFORE calling `sched_wake_claim_enqueue`, so a
  claim that returns 0 guarantees the completion path observes both the token and
  READY, and `process.c:2601` tests `state == READY` OUTSIDE the token branch and
  enqueues. It is rescued. Corroborating: that family would leave `wake_pending=1`
  and every capture shows `wake=0`, and its `ready_by` would name
  `process_wake_waiters`/`process_set_ready`, never observed.

  TWO LIVE CANDIDATES, both of which produce the captured state exactly. Both are
  now INSTRUMENTED, and candidate A is no longer a hypothesis -- see the two
  entries after this list:

  A. An unlink-then-drop. `cpu_sched_pick_next` and `cpu_sched_steal_pick` unlink
     the thread and clear `on_rq` BEFORE the caller holds any claim; then
     `process.c` returns `SCHED_R_STALE` on a failed `dispatch_ref` CAS without
     re-enqueueing, and the neighbouring comment claiming "nothing has been touched
     yet" is false. Same shape on the steal/owner-gone exit. Neither path counts or
     logs anything.
  B. An enqueue attempted and skipped. If the promoter really is the
     PROCESS_RUN_BLOCKED completion path, the enqueue two instructions later must
     have declined, and the candidates are enumerable in `cpu_sched_enqueue`: idle,
     bad prio, non-READY, `on_rq` already set, and the double-link bail, which
     releases `on_rq` and returns -- yielding READY, rq=0, owed=0 exactly.

  THE DISCRIMINATOR NOW EXISTS, 2026-08-23, and reading it is the next step. Three
  pieces, all in place:

  1. Per-thread run-queue forensics on `thread_t`: `rq_enq_result` (the outcome of
     the last enqueue attempt), `rq_link_count` (times this thread was actually
     linked), `rq_unlink_site` (who last released `on_rq`) and `rq_enq_by` (the
     call site of that attempt, carried in through the new
     `cpu_sched_enqueue_from`). The stall dump prints them for a strand as
     `enq= links= unlink= enq_by=`, beside `ready_by=`.
  2. A counter and a one-shot log at each of the two formerly blind exits in
     `process.c` -- `SCHED_DEBUG_DISPATCH_DROPPED_SLOT_LOST` (the `dispatch_ref`
     CAS failed) and `SCHED_DEBUG_DISPATCH_DROPPED_STEAL_REAPED` (the stolen
     thread's owner was gone). The `slot claim lost` line carries the OBSERVED
     `ref=` value, which separates "another CPU raced this pick" (1, DISPATCH) from
     "a reaper is tearing the slot down" (2, FROZEN).
  3. Every tripwire's running total on one line in the stall dump
     (`[diag] sched counters: ...`), printed even when zero. This is what makes a
     negative reading mean anything: the per-event logs suppress roughly four hits
     in five, and four conclusions in this entry were drawn from absent log lines.

  How to read a capture: `links>0` with `unlink=pick_next`/`steal` is candidate A
  and points at the dispatch exits. `enq=skip:*` is candidate B and names which
  guard in `cpu_sched_enqueue_from` declined -- `skip:double-link`,
  `skip:already-queued`, `skip:non-ready`, `skip:bad-prio`, `skip:idle`.
  `links=0` cannot be reached by candidate A at all. `enq=deferred` means a claim
  was published, which `owed=` then says whether anything still holds.

  Pinned by the X1-X7 cases in `tests/unit/test_sched_runqueue.c`: that the two
  histories read differently, that each refusal outcome is distinct from every
  other, that the record describes the last attempt rather than the last failure,
  that each picker and remover stamps its own site, and that re-init clears the
  record. All seven were mutation-checked against four deliberate breakages of the
  recording and each one caught its mutant.

  DO NOT ATTRIBUTE A REGRESSION FROM BATTERY COUNTS. Not because the counts are
  noise -- they are not -- but because they are a weak, probabilistic OBSERVABLE of
  a deterministic race, and reasoning from them the wrong way has already misled
  once in this investigation.

  The mechanism below either lands on a thread's last enqueue or it does not, and
  that depends on interleaving. So a battery is red when the race lands somewhere
  fatal and green when it lands somewhere survivable -- the same code, the same
  bug, a different interleaving. Calling a green battery a "flake" or saying it
  "flipped" describes the measurement and hides the cause.

  What follows for attribution, and it is the important part: a change that alters
  the PROBABILITY of hitting the window is a real regression, and comparing run
  counts cannot detect one. The last eight runs on this branch were red at 1, 2, 3,
  3, 2, 4, 3 and 4 jobs, several on documentation-only commits; run 32663402965
  rerun on the IDENTICAL commit gave 4 then 3. Two runs cannot distinguish p=0.3
  from p=0.4, so "the rerun gave fewer failures" is not evidence of no effect. It
  is not evidence of anything.

  Measure the RACE, not the outcome. That is what the counters are for:
  `dispatch-dropped-slot-lost` and `dispatch-left-stranded` count the event itself,
  per boot, and they are comparable across commits in a way battery counts are not.
  Compare those (and the persistent red SET, and the failure SIGNATURE -- here
  `setUpClass` errors with no panic, fault or invariant failure anywhere) rather
  than how many jobs went red.

  AND HERE IS THE SAME MISTAKE THIS ENTRY KEEPS MAKING, caught this time before it
  was acted on. The change that added those counters has one behavioural edit,
  `thread_reset_slot`'s claim restore, and the captures report
  `thread-reap-refused=0` / `owner-reap-leftover=0`. Reading that as "the changed
  path never ran, so it cannot have moved the race probability" is WRONG: those
  zeros have two explanations, and the second is the fix working. Under the old
  code a refused reset leaked FROZEN, and `thread_reap_owner` then burned all 64
  passes and reported a leftover -- so `owner-reap-leftover=0` is exactly what a
  working fix produces, and it does not distinguish that from a cold path.

  What IS established: no reap is currently giving up, so no slot is being leaked.
  What is NOT: whether returning those slots and threads to circulation changes how
  often the dispatch race below is hit. The fix can only make reaps SUCCEED where
  they previously gave up, which puts more live threads in front of the picker, and
  that is a plausible way to move the probability. It is untested, and these
  counters cannot test it -- the honest comparison needs
  `dispatch-dropped-slot-lost` per boot measured across several runs on each side.

  RESOLVED, 2026-08-23, by the first capture read through the new fields (CI run
  32663402965, four QEMU batteries red). Candidate A is CONFIRMED and candidate B
  is REFUTED, and for once both by positive evidence rather than by log silence.

  Every strand in that run, paired with its own forensic line:

      tid=46 pid=37 gfx-compositor || enq=linked links=818 unlink=steal
      tid=46 pid=37 gfx-compositor || enq=linked links=517 unlink=steal
      tid=46 pid=37 gfx-compositor || enq=linked links=491 unlink=steal
      tid=31 pid=22 ata           || enq=skip:already-queued links=133 unlink=pick_next
      tid=31 pid=22 ata           || enq=linked links=189 unlink=pick_next

  `links` is 133-818 and never 0; `unlink` is always a picker, never an enqueue
  guard. `links=0` is the candidate-B signature and appears nowhere. The counters
  line settles the rest without relying on any absent log line: `double-link=0`
  (B's prime suspect never fired at all), `bad-prio=0`, `enqueue-idle=0`,
  `ghost-head=0`, `init-on-queued=0`.

  And the exit is named. `dispatch-dropped-slot-lost` is non-zero in every boot
  (1-2), `dispatch-dropped-steal-reaped` is 0, and its log line carries the
  observed claim value:

      slot claim lost tid=46 owner=37 state=1 ref=1 cpu=0
      slot claim lost tid=31 owner=22 state=1 ref=1 cpu=1

  `ref=1` is THREAD_SLOT_DISPATCH on every single line -- another CPU racing the
  same pick, never a reaper (which would read 2, FROZEN). The victim set
  {10, 20, 24, 31, 46} is a superset of the strand victims {31, 46}.

  THE INTERLEAVING, and it is provable from the source rather than inferred. Every
  re-enqueue in the result handling (`process.c:2640`, `:2653`, `:2687`) runs
  BEFORE `dispatch_done` releases `dispatch_ref` (`:2695`, `:2696`). So for a
  winner A and a second CPU B:

    1. A finishes its dispatch and RE-ENQUEUES the thread (2640/2653/2687).
    2. B picks it -- it is linked and READY -- so B's picker unlinks it and
       releases `on_rq`.
    3. B CASes `dispatch_ref` from FREE and LOSES, because A has not reached 2696
       yet. B returns SCHED_R_STALE and drops the thread without re-enqueueing.
    4. A stores FREE and returns. Its enqueue already happened, at step 1.

  Nothing enqueues the thread again: READY, on no run queue, owed nothing, no wake
  token. The window is exactly the instructions between A's enqueue and A's claim
  release, which is why the strand is rare, why it lands on different victims, and
  why `unlink` names both pickers (gfx-compositor via `steal`, ata via
  `pick_next`).

  CONFIRMED TWICE, independently. The rerun of the same commit reproduced the same
  reading on different boots with different numbers -- `enq=linked links=2148
  unlink=steal`, `links=1481 unlink=steal`, `links=356 unlink=steal`,
  `enq=skip:already-queued links=66 unlink=pick_next`. Never `links=0`, never an
  enqueue guard. Two independent captures agreeing on the discriminator is what
  this entry has lacked at every previous stage.

  Why the exit fires far more often than it strands: steps 1-3 are survivable
  whenever the thread receives a later wake, which re-links it. tids 10, 20 and 24
  took this exit and did NOT end stranded. It is fatal only when it lands on the
  last enqueue a thread was going to get -- the same "necessary but not
  sufficient" shape already recorded for the strand overall.

  WHAT THE FIX MUST DO, and the trap in it. Dropping at that exit is sound only
  when the CAS winner still has its own re-enqueue ahead of it, and the ordering
  above shows it may not. So the exit must re-enqueue (or publish a claim) for a
  thread whose owner is LIVE -- `sched_enqueue_thread` is the right call, being
  idempotent through the `on_rq` claim, refusing a non-READY thread, and deferring
  with a claim when the thread is still current somewhere. The live-owner condition
  is NOT optional: `ref=2` (a reaper) reaches the same exit, and re-enqueueing a
  dying owner's thread is the non-converging repair loop already measured at
  `dispatch_done` (the host suite did not terminate at all with that exclusion
  forced off). Owes its own red-first test; the existing "a lost slot claim strands
  a live owner's thread" case asserts the CURRENT behaviour and must be rewritten
  to assert the repair, not relaxed.

  The alternative -- release `dispatch_ref` BEFORE the result handling's enqueue --
  is worse and should not be reached for: the claim exists to keep the thread and
  process slots un-recyclable across exactly that handling.

  CANDIDATE A IS FIXED, 2026-08-28. The losing side of the `dispatch_ref` CAS now
  leaves an owed-enqueue claim for the thread its pick unlinked
  (`sched_owe_enqueue_for_dropped_pick`, called from the `SCHED_R_STALE` exit in
  `process_schedule_once_impl`), so the holder or an idle CPU's
  `sched_sweep_owed_enqueues` re-links it. It publishes the debt WITHOUT linking:
  the holder is still writing that thread's context, and linking from the loser is
  the variant that panicked with `rip` inside `g_threads` (see `sched_owe_enqueue`).
  Only a claim held by another DISPATCH owes anything; a FROZEN slot is
  `thread_reset_slot` mid-teardown, whose thread is meant to end unqueued.

  Evidence, all at `5ec6f59ef` on linux x86_64 with `-smp 4`:

      unit      s_a_lost_slot_claim_leaves_the_thread_reachable -- red before the
                fix (state=1 on_rq=0 owed=0), green after; whole host suite green
      tcg MTTCG battery scheduler-and-ipc 580s failures=1 errors=1 stranded=1
                -> 234s (documented shmem flake only) stranded=0, then 53s 7/7 OK
      tcg single  1738s failures=1 errors=4 stranded up to 3 -> 95s 7/7 OK

  The wall-clock collapse is part of the evidence: those runs were slow because
  sessions wedged and waited out timeouts.

  NOT closed by this, and the reason this item stays open: with the strand gone
  the sessions stop stalling, so no `[diag]` dump prints and
  `dispatch-dropped-slot-lost` becomes unobservable. "The race fired and was
  harmless" is therefore shown by the unit case, not by a capture. A CI run that
  dumps with `slot-lost>0` and `stranded=0` would close it.

  CORRECTION to "the strand is always the SAME thread -- gfx-compositor, tid=46,
  pid=37": run-dependent, not fixed. Local reproduction stranded `ata` tid=31 with
  no compositor strand at all under MTTCG, and `net-stack` tid=29, `ata` tid=31,
  `fs-fat` tid=32 AND gfx-compositor tid=46 under single-threaded TCG. A repair
  aimed at anything compositor-specific would be aimed wrong.

  CORRECTION to "`-smp 4` reproduction on Linux x86 is the way in", which is true
  but for the wrong reason: this is an INTERLEAVING race, not a memory-ordering
  one. The orderings are already correct (`__ATOMIC_ACQ_REL`/`ACQUIRE` on the CAS,
  `RELEASE` on the store), and it reproduces under `-accel tcg,thread=single`,
  where one host thread round-robins the vCPUs and no cross-thread reordering
  exists -- more often than under MTTCG, in fact. Single-threaded TCG cannot rule
  this class of bug out, and an apple-silicon host's silence is a probability
  difference, not a concurrency-model one.

  CANDIDATE A WAS DEMONSTRATED, 2026-08-23, with a LIVE owner and no race. Pinned
  by the case now named "a lost slot claim leaves the thread reachable" in
  `tests/unit/test_process_lifecycle.c`, which holds `thread_t::dispatch_ref` at
  `THREAD_SLOT_DISPATCH` before driving a real dispatch -- exactly what a second
  CPU that won the same pick, or a reaper mid-teardown, presents. Measured:

      state=1 on_rq=0 owed=0 wake=0 links=2 unlink=pick_next

  which is the CI signature field for field, with the owner still live and
  therefore never reaped. `cpu_sched_pick_next` had already unlinked the thread and
  released `on_rq`; the exit returns `SCHED_R_STALE` without re-enqueueing; nothing
  can recover it, because `sched_sweep_owed_enqueues` is gated on the global debt
  counter and this thread carries no debt.

  This SUPERSEDES, for this exit only, the note in
  `s_aborted_dispatch_leaves_its_thread_reachable` that calls the live-owner abort
  "not constructible from here". That note stands for the
  `SCHED_DEBUG_SET_RUNNING_EXITING` route it was written about.

  It also explains why `rc=7` on every `SCHED_DEBUG_DISPATCH_LEFT_STRANDED` report
  never contradicted candidate A: this exit returns BEFORE `dispatch_done`, where
  that tripwire lives, so it is structurally invisible to it. The test asserts that
  silence so it is a documented property rather than another negative read as
  evidence.

  What that does NOT settle: whether this exit is the mechanism behind the CI
  strands, only that it can produce them. The remaining work is one capture read
  through the new fields. A naive repair -- re-enqueueing at the exit -- makes the
  host case go green, and is NOT the fix to reach for: the same exit is taken for a
  slot a reaper has FROZEN, and re-enqueueing a dying owner's thread is the
  non-converging repair loop already measured at `dispatch_done` (the host suite
  did not terminate at all with the live-owner exclusion forced off). Any fix here
  needs the same owner-liveness condition.

  FIXED in the consumer, 2026-08-23, and it IS a real defect regardless of the
  above: `sched_settle_deferred_enqueue` now reads the
  state BEFORE taking the claim, so a thread that is momentarily not enqueueable
  keeps its debt for whoever can honour it. Pinned by "a consumer that declined to
  enqueue left the claim outstanding" in `tests/unit/test_process_lifecycle.c`,
  which publishes the claim through the real protocol (an enqueue refused because
  another CPU still names the thread) rather than by poking the field: red at
  `owed=0`, green at `owed=1`.

  Validate-first rather than re-publish-on-failure, deliberately. Re-publishing
  keeps a debt alive for a thread that is legitimately blocked, which leaves
  `g_enqueue_owed_count` non-zero and makes the sweep scan on every idle pass.
  Validate-first costs one thing instead: the state can change between the read and
  the exchange, so a claim may be taken for a thread that has just stopped being
  READY -- `cpu_sched_enqueue` re-checks and skips, a logged no-op rather than a
  lost wake.

  `sched_sweep_owed_enqueues` deliberately keeps take-then-validate. It is the
  DEFINITIVE resolver: it runs only when a CPU has nothing else to do, so it sees
  settled state rather than the transients the settle path meets the instant a
  dispatch ends, and something has to be able to retire a debt whose thread is
  never coming back. If a capture ever shows a strand whose debt the sweep
  discarded, that is the next thing to change.

  The boundary repair added in a83b9d4d35 is REVERTED to report-only, and the
  measurement is why: 28 firings in a single clean boot. A synchronous check at
  `dispatch_done` cannot separate "stranded" from "in flight" -- the only
  difference is elapsed time, and a waker that promotes then enqueues a statement
  later, or a stealer that has unlinked but not yet claimed, both present exactly
  that state. It also could not see the case above at all, since the final
  promotion happens outside any dispatch. The tripwire stays; it is what found all
  of this, and `rc` is the field that names the exit.

  CAUSE 4 IS FIXED AND A FIFTH WAS SUSPECTED, which is this item's usual pattern.
  The first post-fix capture (`graphics-and-vt`, on 1e46fac0aa, failing
  `test_tty_switch_stress_with_output_spam`) still shows a persistent strand --
  `stranded(ready,no-rq)=1` across five samples, tid=31 `ata`, READY, unqueued,
  owed nothing -- but the breadcrumb has MOVED:

      ready_by=ffffffff80222ada (process_schedule_once_impl)

  It is no longer `sched_wake_thread`, so that path is closed. The new promoter is
  the dispatcher itself. `SCHED_R_STALE` was suspected and is now RULED OUT: with
  the tripwire's owner resolution fixed it reports this case, and every one of the
  seven reports in a clean boot carries `rc=7`, the normal exit, not `rc=8`. The
  suspect text below is retained only to show what was checked:
  `thread->tid != picked_tid || thread->owner_pid != proc->pid` transits
  RUNNING -> READY and jumps to `dispatch_done`, which never enqueues. That is the
  aborted-dispatch mechanism demonstrated on the host by
  `s_aborted_dispatch_leaves_its_thread_reachable`, previously set aside as benign
  -- it was a SECOND cause, not a refuted one, and the note above that reads it as
  refuted should be read as "not the cause of the sched_wake_thread strand".

  The tripwire was blind to exactly that case -- no `[sched] dispatch left
  stranded` line appears anywhere in that run despite the strand -- because its
  exclusion consulted `proc`, the process the dispatch STARTED with. A STALE exit
  means the slot was recycled: `proc` is the old process, typically already ZOMBIE,
  while the thread belongs to a live one, so the report was suppressed for the one
  exit that needed it. It now re-resolves the owner from `thread->owner_pid`.

  And `dispatch_done` now REPAIRS the invariant rather than only reporting it: a
  thread that is READY, unqueued, owed nothing and whose live owner wants it
  runnable is handed to `sched_enqueue_thread`, which links it, defers it with a
  claim, or declines -- it cannot double-link. Justified by the invariant, not by
  knowing which exit produced it: a READY thread with a live owner is runnable by
  definition, so leaving it on no run queue is wrong however it got there. The
  report runs FIRST, because the repair publishes a claim and the tripwire tests
  for the absence of one.

  The live-owner exclusion on that repair is load-bearing, and verified rather
  than assumed: with it forced off, the host suite does not terminate at all
  (killed at 25 s against a normal 1.5 s), because a dying owner's thread is
  re-enqueued, re-picked, refused at `process_set_running` and repaired again. A
  repair that ignores the owner's state does not converge.

  What would confirm a fix, once that is done: several consecutive full-suite runs
  with no persistent `stranded(ready,no-rq)` in any dump. The strand is present in
  runs that still boot (it is fatal only when it lands on the last wake a thread
  was going to receive), so a green run proves less here than usual, and dumps are
  only emitted on failure -- so the confirmation has to be the absence of the
  marker in the captures that do appear, not the absence of failures.

- [x] [BUG][P1] `test_shmem_grant_revoke_pair` fails intermittently in the
  `scheduler-and-ipc` battery. OBSOLETE: the test and the apps it drove were
  deleted with the shmem subsystem, so the flake has no subject. Worth keeping
  the finding it produced, because it outlived its test: the failure was NOT the
  whole-session wedge. The 2026-08-16 thread dump showed no endpoint had refused
  a send (the wedge's signature) and cli was RUNNING rather than stranded, with
  the marker missing at the early-boot stage. Whatever it was, it is its own
  fault and still uncaptured.
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
