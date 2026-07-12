# WASMOS Debug Handover — WARP ring3 `ps` entry fault

> Scratch handover doc. **Do not commit.** Delete after reading / folding into commits.
> Written from the Linux VM (`root@10.96.1.78`). Repo there: `/root/wasmos` (rsync'd, **not** a git checkout).
> Mac repo: `/Volumes/git/wasmos` (git). Default branch `main`.

---

## 0. UPDATE 2026-06-27 (Linux VM) — path (1) chosen & committed; new finding

**DONE:** Implemented §5 path (1). Consumer fix committed to canonical as `8993096a`
(`src/utils/ps/ps.zig`): `show_table = args.len == 0 or args[0] != "tree"`. Build tree verified
**clean of all §3 kernel changes** (no zero-on-alloc in `xfer_buffer/store.c`, no `[r3-*]`
diagnostics, no `paging.c`/`cpu_x86_64.c` diffs) — `src/utils/ps/ps.zig` is the **only** source delta
vs `main` HEAD `1ee22821`. So the §4 entry fault is **not** reintroduced.

**Validation (Linux VM, qemu 8.x TCG, smp=1):**
- ps lottery (PS_N=30): `ok=11 silent=19 fault=0`. **0 faults, 0 table-skip** — table prints on every
  run where ps actually spawns. ✅ for the table-skip + fault goals.
- 3 `test_cli` ps tests **in isolation**: all **PASS** (`test_ps_lists_processes`,
  `test_ps_tree_lists_hierarchy`, `test_ps_all_lists_table_and_tree`). ✅
- Full `test_cli` module (14 tests, shared boot): **13 pass, `test_ps_tree_lists_hierarchy` FAILS**
  deterministically (2/2 reruns) — but with `[wasmos-app] start failed` (pm spawn failure *before*
  wasm main), NOT a table/fault issue.

**NEW follow-up — pm spawn-slot exhaustion (pre-existing, was masked):**
The lottery shows a hard cliff: spawns 0–10 OK, 11→ all `[wasmos-app] start failed`. In the full
`test_cli` module the cumulative app spawns (cat/exec/ps across tests in one boot) cross the ~11–12
cliff right at `test_ps_tree` (≈12th spawn), so it fails. This is a per-spawn slot/resource leak
(likely the 2 MB `pm_fs_slot`), independent of this change — previously **masked** because table-skip
already made those runs SILENT (cf. §4 fact #4 `ok=1 silent=39`). The ps fix converts table-skip→OK,
unmasking the exhaustion as the remaining SILENT cause. **The 3 ps tests pass in isolation; the literal
"3 ps tests green in the full suite" bar is blocked by this separate leak, not by the table-skip.**
macOS cross-check of the lottery requested (is it a TCG artifact or a real leak?).

**CONFIRMED + ROOT-CAUSED (cross-host):** macOS qemu (TCG, smp=1) lottery = `ok=11 silent=19 fault=0`,
**identical** split and identical cliff at spawn 11. Two independent TCG hosts, exact same boundary →
host-independent, NOT a TCG artifact. (No non-TCG sample: macOS hvf won't boot to prompt.)
Root cause (read-only trace of `src/kernel/xfer_buffer/store.c`):
- Each spawn allocates a **2 MB** FS xfer buffer (`PM_XFER_BUFFER_SIZE`, `process_manager_internal.h:14`)
  via `pfa_alloc_pages` in `pm_fs_slot_for_context`. Slots are a linked list (`g_pm_fs_slots`), no fixed cap.
- `xfer_buffer_release()` (which `pfa_free_pages` + unlinks the slot) is only invoked
  from **guest-initiated** link calls (`warp/link.cpp:719,1366`, `wasm3/link.c:761`, `native_driver.c`).
  There is **no process-exit/destroy hook** that releases it — grep for exit/destroy/terminate wiring in
  `process_manager.c` finds none. `ps` never explicitly releases, so its 2 MB slot **leaks on exit**.
- ~11 × 2 MB ≈ 22 MB exhausts the page-frame pool → `pfa_alloc_pages` returns 0 → the deterministic
  cliff at spawn 11. **Confirmed NOT a fixed cap** (macOS verified): `PROCESS_MAX_COUNT=48` with slots
  recycling (ps reaped each run, pid climbs 36→47, no zombie pileup), and `g_pm_fs_slots` is an
  array-chunk list (chunk=16) that grows — neither caps at 11. Deterministic only because the boot
  baseline allocation is identical each run.
- **Where it surfaces:** the failing call is `wasm_driver_start()` (`wasmos_app.c:528` →
  `warp_driver.cpp`: `new WasmModule`@659/691, JIT compile, `warp_r3_setup`@708, dual-map@716) — i.e.
  the *next* page consumer once the pool is drained, NOT the FS-buffer alloc itself. That's why the
  klog is `[wasmos-app] start failed` rather than a buffer message. The leak is upstream (the 2 MB
  `pm_fs_slot`); WARP start is just the first allocation to come up empty.
**Recommended follow-up (out of scope here, kernel change):** call
`xfer_buffer_release(PM_BUFFER_KIND_FILESYSTEM, ctx)` from the process reap/destroy
path so a process's FS slot is freed on exit. Verify it doesn't disturb the borrow lifecycle or re-trigger
the §4 WARP entry fault (it's a free-on-exit, not a write-at-alloc, so it should not — but A/B with the
lottery to be sure).

---

## 1. Where we are (one paragraph)

The wasm3 SMP boot deadlock is **fixed and merged** (`main` commit `841cc45da`, the 16-page
`WASM_LINEAR` region in `src/kernel/memory.c`). CI was set up (4 defconfigs + unit + integration).
The **integration suite (warp_smp) is red** because of two distinct things:
(a) a **reliable** `ps` failure — `ps` prints its summary but **drops the process table**, and
(b) a **rare** `ps` ring3 page-fault seen in CI's tty-stress test.
Both were chased to root causes. Fixing (a) the obvious way (zeroing the recycled FS buffer)
**uncovers a deep WARP ring3-entry fault** that we have *not* yet fixed. That fault is the open item.

---

## 2. Confirmed root causes

### (a) `ps` table-skip  (the reliable CI failure — 3 `test_cli` ps tests)
- `ps` reads its CLI args from the per-context **xfer buffer** (`src/libc/zig/wasmos.zig::parseCliArgs`
  → `xfer_buffer_read(FILESYSTEM, off 0)`).
- For a no-arg spawn the buffer is **never written**, and `pm_fs_slot_for_context`
  (`src/kernel/xfer_buffer/store.c`) allocates the 2 MB buffer with `pfa_alloc_pages` but
  **does not zero it** → `ps` parses **stale recycled bytes** as args → `args.len>0` →
  `show_table=false` → table dropped. ~99% reproducible on the VM.

### (b) WARP ring3 entry fault  (the OPEN bug — see §4)
- Surfaces the moment we make `ps` read *clean* args (so it actually runs its table path).
- This is **not** SMP/cross-CPU and **not** a TLB-flush problem (see §4 evidence).

---

## 3. Changes currently on the VM (uncommitted on top of `main`)

`main` already contains the committed wasm3 fix (`src/kernel/memory.c`, 16 pages). Everything below is
**uncommitted** working-tree state on the VM.

| File | Change | Disposition |
|---|---|---|
| `src/kernel/include/warp_ring3.h` | Remove singleton `g_warp_r3_state`; `warp_r3_setup`/`teardown` take per-driver root/stack params | **KEEP** — valid correctness fix (per-driver, not global). User confirmed keep. |
| `src/kernel/warp/ring3_trampolines.c` | Same refactor + **`[r3-setup]`/`[r3-tdn]` klog diagnostics** | KEEP refactor; **REMOVE the two `klog_printf` diagnostics**. |
| `src/kernel/warp_driver.cpp` | Same refactor + **`[r3-iret]` klog diagnostic** before `r3_do_iretq` | KEEP refactor; **REMOVE the `[r3-iret]` klog**. |
| `src/kernel/xfer_buffer/store.c` | **zero-on-alloc**: `memset(buffer_phys|KBASE, 0, page_size)` in `pm_fs_slot_for_context` after `pfa_alloc_pages` | **DECISION PENDING.** Fixes (a) but **triggers the §4 fault**. See §5. |
| `src/kernel/paging.c` | **DIAG only**: PML4 leak — `pfa_free_pages(root_table,1)` commented out (`DIAG(option2)`) | **REVERT** — it was dead code (teardown path not exercised; see §4). |
| `src/kernel/arch/x86_64/cpu_x86_64.c` | `pf_translation_permits()` + spurious-retry block (added during Codex collab, **not mine**); my **CR3-toggle** swapped in for `invlpg`; `[r3-term]` klog | Decide on the retry mechanism (it does **not** fix the fault — 4096 futile retries). **REMOVE `[r3-term]` + the CR3-toggle diag**; restore/remove the retry per decision. |
| `scripts/ps_lottery_repro.py` | Repro harness (boot once, send `ps` N times, classify OK/SILENT/FAULT) | KEEP as a tool (or move to scratch). |

> Quick scan markers: `grep -rn "r3-iret\|r3-setup\|r3-tdn\|r3-term\|DIAG(option2)\|DIAG: CR3" src/kernel`.

---

## 4. The OPEN bug — WARP ring3 entry fault (full evidence)

**Symptom:** `[fault] user-pf ... err=0x14 cr2=0x000000a080001020 rip=0x000000a080001020`
i.e. ring3 **instruction-fetch, not-present** at `WARP_R3_ENTRY_TRAMPOLINE`
(= `WARP_R3_RET_TRAMPOLINE` page `0xA080001000` + 0x20; see `src/kernel/include/warp_ring3.h`).
`ps` is terminated; in CI this shows as the tty-stress fault and (with the table fix) as ~90% `ps` failures.

**Lifecycle trace (instrumented), per faulting `ps` (pid 41), identical every spawn:**
```
[r3-setup] root=4206000 stack=10861000          # warp_r3_setup maps trampoline into root
[r3-iret]  cpu=1 pid=41 root=4206000            # warp_r3_call_export switches CR3=root, IRETs
[r3-term]  cpu=1 pid=41 cr3=4206000 permits=1 retries=4096 err=14
```

**Hard facts (each verified):**
1. **PTE is present** at fault time. `permits=1` (software walk of the faulting CR3 finds all levels
   present, U/S=1, NX=0). The #PF ISR (`isr_exception_14`) does **not** switch CR3, so the walk is of
   the true faulting context. Walk seen earlier: `pml4[1]=...027 pdpt=...027 pd=...007 pt=...005`.
2. **Flush-immune.** The retry path tried `invlpg`, same-value CR3 reload, and a **CR3 toggle**
   (kernel-root → back, a real flush) — **4096 retries all re-fault**. So not a stale-TLB-we-can-flush.
3. **Not cross-CPU.** `-smp 1` still faults (~46%); `warp_single` (CONFIG_WASMOS_SMP off) faults (~90%).
   Both `warp_smp`/`warp_single` have `CONFIG_WASMOS_RING3=y`; only SMP differs. CPU count *amplifies*
   the rate but is not required. (This overturns the earlier cross-CPU/TLB-shootdown theory.)
4. **Trigger = any FS-buffer write at slot-alloc.** A/B: zero-on-alloc ON → ~90% fault; OFF → **0
   faults** (`ok=1 silent=39 fault=0`, ps just table-skips). A 1-byte write triggers it as much as a
   full-page memset, so it's not memset size — it's that `ps` then runs its full table path.
5. **`warp_r3_teardown` is never called** — `[r3-tdn]` never appears. The WARP `user_root`
   (`0x4206000`) is **recycled every spawn**, so it must be freed by the *other* path:
   `mm_context_destroy → paging_destroy_address_space(ctx->root_table)` on process exit. **Strong
   implication: the WARP `user_root` IS the process's `mm_context` root**, not a separate root as the
   per-driver model assumes. That's why the option-2 PML4-leak (placed in `warp_r3_teardown`/
   `paging_destroy_address_space`) did nothing.

**Working hypothesis:** a QEMU **TCG soft-MMU / paging-structure-cache artifact tied to recycling the
page-table physical pages** (same root phys `0x4206000`, same stack `0x10861000`, same pid 41 every
spawn). The zero-on-alloc perturbs allocation/timing enough to make `ps` actually run (and re-touch
those recycled structures), tipping the entry IRET into a not-present fetch that no local flush clears.
Not proven — the flush-immunity is still not fully explained by pure x86 semantics.

---

## 5. Two ways forward (decision needed)

1. **Pragmatic / unblock CI (recommended first):**
   - **Revert** zero-on-alloc (no kernel FS-buffer write ⇒ entry fault never triggers).
   - Fix table-skip in the **consumer** `src/utils/ps/ps.zig` (~line 156-158): make `show_table`
     true for *unrecognized* args, i.e. `show_table = args.len==0 or args[0] != "tree"` (so "", "all",
     and garbage → table; only explicit "tree" hides it). Robust to stale/garbage args; touches no kernel.
   - Result: `ps` shows its table reliably **and** never hits the entry fault.
   - Caveat: leaves the general stale-args bug for *other* arg-taking apps (e.g. `cat <file>`); none of
     the currently-failing integration tests need that, but file it.

2. **Keep attacking the reuse (retargeted):**
   - First confirm WARP `user_root` == process `mm_context` root (instrument / compare values).
   - Then quarantine recycled page-table pages in the **real** free path
     (`mm_context_destroy` / `paging_destroy_address_space` / `alloc_table` in `paging.c`) so a fresh
     process gets a fresh root phys, and re-run the lottery. If the fault clears, reuse is confirmed and
     the real fix is a quarantine or a heavier flush on page-table-page recycling.
   - Also worth: figure out why `warp_r3_teardown` is never called (leak of trampoline/stack pages).

There is also a **separate intermittent boot stall** (~25% on `warp_smp`, clean main) at the
device-manager stage (`[device-manager] rule roots ...` then silence, "calculator did not fully
initialise"). Pre-existing; not yet root-caused. May or may not share machinery with the above.

---

## 6. Reproduce / test (on the VM)

```bash
export PATH=/usr/lib/llvm-20/bin:/opt/zig-x86_64-linux-0.14.1:/usr/local/go/bin:$HOME/.cargo/bin:$PWD/node_modules/.bin:$PATH
cd /root/wasmos
cmake --build build-warp_smp                              # build kernel+apps
cmake --build build-warp_smp --target run-qemu-test       # build + populate build-warp_smp/esp + 1 halt-boot
# ps lottery (boot once, send ps N times):
WASMOS_OVMF_CODE=/root/ovmf/OVMF_CODE.fd WASMOS_OVMF_VARS=/root/ovmf/OVMF_VARS.fd \
WASMOS_ESP=$PWD/build-warp_smp/esp WASMOS_USERFS=$PWD/userfs \
WASMOS_QEMU_ISOLATE_ESP=1 WASMOS_QEMU_SMP_COUNT=4 WASMOS_QEMU_ACCEL=tcg PYTHONDONTWRITEBYTECODE=1 \
PS_N=30 python3 scripts/ps_lottery_repro.py 2>&1 | tail
# classify: grep -aoE "OK|SILENT|FAULT" <log> | sort | uniq -c ; grep -ac user-pf <log>
```
Notes: **must** repopulate the ESP (`run-qemu-test`) after every kernel rebuild or the lottery boots a
stale kernel. `pkill` qemu via `pkill -9 qemu-system` (NOT `-f ...qemu-system-x86_64`, which matches the
tool's own shell). Boots intermittently NO-PROMPT (the §5 boot stall) — just retry.

---

## 7. Environments

- **VM** `root@10.96.1.78` (Ubuntu x86_64, 4 cores, qemu 8.2.2 TCG — matches CI). Toolchain:
  LLVM 20 `/usr/lib/llvm-20/bin`, zig `/opt/zig-x86_64-linux-0.14.1`, rust+wasm32 `~/.cargo`,
  tinygo 0.41.1, go `/usr/local/go`, asc via in-repo `npm install`. OVMF staged at `/root/ovmf/`.
  Builds present: `build-warp_smp`, `build-warp_single`. WIP backups in `/root/wip/`.
- **Mac** `/Volumes/git/wasmos` (git, branch `main`). OVMF auto-discovered; qemu arm64 (TCG). The
  entry fault reproduces there too but `pkill -9 qemu-system` and the same env vars apply
  (`WASMOS_OVMF_CODE=/opt/homebrew/share/qemu/edk2-x86_64-code.fd`, no OVMF_VARS needed).

---

## 8. Immediate next action when resumed on the Mac

1. Sync the VM working tree (or re-derive the changes in §3) to the Mac.
2. Pick path (1) or (2) from §5. If (1): revert zero-on-alloc, edit `ps.zig`, build `build-wasm3`/
   `build` appropriately, validate the ps lottery is green + no faults, then run the full
   `run-qemu-cli-test` integration suite.
3. Remove all `[r3-*]` diagnostics and the `paging.c` DIAG before committing.
4. Keep: WARP per-driver `g_warp_r3_state` refactor; wasm3 16-page fix (already committed).
