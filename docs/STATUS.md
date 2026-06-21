# Current Status

- This status file is a snapshot, not a release changelog.
- **Language runtime entry shims now terminate processes via `proc_exit` instead of relying on a normal `wasmos_main` return path.** Several graphical WARP apps could run normally, then fault with `vector=6` at the same user RIP when a close request let their event loop return. The same broken export-return tail could also truncate short-lived utility output such as `ps`. The common C, Zig, Rust, Go, and AssemblyScript WASM startup wrappers now store startup args, run the language `main`, and then invoke the explicit process-exit hostcall with the returned status instead of depending on the runtime/export epilogue to unwind cleanly.
- **WARP console writes now stay contiguous through the kernel hostcall, and `ps` buffers its full report before flushing.** The intermittent `ps` corruption was not bad process metadata: WARP-backed apps were still allowed to be preempted in the middle of `console_write`, unlike the wasm3 path, so shell prompt redraws or other log writers could splice bytes into a running table dump. The WARP hostcall now brackets console emission with `preempt_disable()` / `preempt_enable()`, and the Zig `ps` utility now assembles its summary/table/tree output into one fixed buffer before writing. That keeps repeated `ps` runs from dropping the header/table or embedding stray escape/control bytes inside the final rows.
- **Explorer tree navigation no longer clobbers its own current-path state while rebuilding the sidebar.** The tree view rollout had exposed an older coupling bug in explorer: the helper used for absolute filesystem `chdir()` also reset the UI-side `g_current_path` back to `/`, and the tree rebuild path called that helper repeatedly while probing directories. That left the right pane showing the actual directory contents while `.wap` launches were still constructed as root-relative paths like `/minesweeper.wap`. Explorer now separates raw FS cwd changes from UI path-state updates, so tree navigation, tree rebuilds, and app spawns keep using the correct full path.
- **`libui` now has a reusable `TreeView`, and explorer uses it instead of faking hierarchy with an indented `ListView`.** The new widget keeps per-row depth metadata alongside the existing list-style label storage, reuses the same scroll/selection/activation model as `ListView`, and renders indentation/guide lines in the component itself. Shared `libui` hit-testing and pointer-gesture routing now treat tree views like list views for drag scrolling, double-click activation, and secondary-click callbacks, so explorer’s left sidebar is now a real reusable widget rather than app-specific flattened text formatting.
- **Explorer tree/sidebar startup is now stable, and shared user-space IPC calls tolerate out-of-order endpoint traffic.** The new split-view explorer had been rendering only its first-frame chrome because the initial tree rebuild allocated a large directory scratch array on the WASM stack and blew up during `reload()`. Explorer now keeps that tree scratch buffer in static storage, so the post-reload frame reaches `Path: /`, the tree list, the file list, and the status bar correctly. In parallel, the generic user-space `wasmos_ipc_call()` helper now keeps draining an endpoint until it sees the matching `request_id` from the expected source endpoint instead of failing on the first unrelated message, which hardens `libui` and other apps that multiplex synchronous requests with asynchronous traffic on the same endpoint.
- **Pointer gestures are now a compositor-level contract, and explorer consumes them through `libui` list-view callbacks.** The gfx compositor still emits the low-level `GFX_EVENT_POINTER` stream for hover/state updates, but it now also normalizes higher-level `GFX_EVENT_POINTER_GESTURE` events for left/right button down/up/click, left double-click, and drag start/move/end. `libui` list views use those shared gestures for row activation and drag scrolling, so `/boot/system/utils/explorer.wap` no longer reconstructs double-click timing in-app. The same list-view work also gives list/scroll views a dedicated scrollbar gutter with a contrasting track/thumb and a capped thumb size, and the shared vertical layout now always runs child component layout hooks even for leaf widgets. That last fix is what lets `ListView` and `ScrollView` compute nonzero `scroll_max` and actually show/use their scrollbars when content overflows.
- **Explorer list sizing, drag direction, and FAT directory markers are now aligned with desktop expectations.** The explorer window now budgets its list height against the fixed path/status/button stack so the list stays inside the window content rect instead of overrunning the bottom edge. Shared `libui` drag scrolling for list/scroll views now follows desktop-style mouse dragging rather than touch-style content panning. FAT-backed `READDIR` listings also now append `/` for directory entries, which lets explorer correctly label `/boot` children such as `EFI/`, `apps/`, and `system/` as directories instead of files.
- **Explorer activation and list rendering are now closer to a real desktop file browser.** Drag scrolling in shared `libui` list/scroll views now moves at a faster rate, list-row text clips against the content gutter instead of painting under the scrollbar, and double-clicking a `.wap` file in explorer now writes the selected boot path into the FS buffer and issues a synchronous `PROC_IPC_SPAWN_PATH_SYNC` request so packaged apps launch directly from the browser.
- **Explorer `.wap` launches now use a dedicated PM reply endpoint instead of the UI event channel.** Explorer was previously issuing synchronous `PROC_IPC_SPAWN_PATH_SYNC` requests on the same reply endpoint used for compositor event polling, so unrelated GFX replies could race the expected PM response and make a real double-click look like a no-op. `.wap` launches now use their own reply endpoint, which keeps process-manager spawn replies isolated from UI traffic.
- **AssemblyScript window teardown now releases compositor buffers in the right order, and explorer rows no longer waste width on type prefixes.** The AssemblyScript `libui` window wrapper now destroys the window before releasing its shared buffer, so failed/closed graphical apps do not leave compositor buffer slots permanently busy after a retry or early exit. The shared AssemblyScript IPC call helper also now waits for the matching `request_id` instead of accepting the first reply on its endpoint. In explorer, list rows now render as `name` or `name/` instead of `[FILE] name` / `[DIR] name`, which gives long `.wap` filenames substantially more room inside the current list width.
- **Shared `libui` drag scrolling now scales to the scrollbar travel instead of using a fixed pixel multiplier, and explorer reserves more width for filenames.** List and scroll views now convert pointer drag distance into content scroll distance based on viewport height versus scrollbar-thumb travel, so dragging behaves like moving the scrollbar itself rather than a slow touch-style pan. Explorer also now opens in a wider window, trims list padding, and stores longer names, which reduces `.wap` filename clipping in `/boot/apps`.
- **AssemblyScript graphics now treat compositor buffer IDs as opaque nonzero handles instead of signed-positive integers.** The compositor can return any random nonzero 32-bit `buffer_id`, but the AssemblyScript `libui` shim had been rejecting IDs with the high bit set because it stored them in `i32` and tested `> 0` before the first `PRESENT_WINDOW`. That made apps such as Minesweeper appear to start and then immediately fail their first frame. The shim now treats buffer IDs as valid whenever they are nonzero, while still resetting released/uninitialized state to `0`.
- **Explorer now uses a split view with a real folder tree sidebar, backed by a generic horizontal `libui` row container.** `libui` now has a reusable row-layout primitive for side-by-side panes and toolbars, and explorer uses it to render a path bar, horizontal toolbar, left-hand directory tree, right-hand file list, and status bar. The tree is rebuilt from the current path’s expanded branch rather than a flat mount list, so navigating into `/boot/apps` or deeper keeps ancestors expanded and lets the sidebar act as the primary folder navigator instead of a static shortcut list.
- **A minimal graphical explorer now exists as a real utility under `/boot/system/utils`.** `src/utils/explorer/` adds a libui-based single-window browser that resets to the VFS root, lists newline-streamed `listdir()` entries in a `ListView`, and uses explicit `Open Selected`, `Up`, and `Refresh` actions to move through directories or inspect file sizes. It intentionally stays within the current FS ABI limits: directory changes still use `FS_IPC_CHDIR_REQ`'s packed short-name path form, so this first version is a pragmatic browser baseline rather than a full two-pane commander or long-path-capable shell replacement.
- **Shutdown now uses a shared kernel power-control path across wasm3 and WARP, and halt smoke now requires actual VM exit.** The previous `halt`/menu-bar shutdown behavior diverged by runtime: wasm3 attempted legacy ACPI/QEMU poweroff ports but could fall into an interruptible `hlt` loop, while WARP skipped the poweroff ports entirely and always hung in place. Both runtimes now call shared kernel helpers for poweroff/reboot, WARP now enforces the same `system.control` policy gate as wasm3, and the QEMU halt smoke scripts now fail if `halt` does not actually terminate the VM within a short timeout.
- **Compositor pointer events now identify the hovered target window instead of assuming the focused one.** `GFX_EVENT_POINTER` now carries the target `window_id` plus packed local coordinates/buttons, and the compositor emits pointer events for the topmost window under the cursor rather than for the keyboard-focused window only. This restores submenu hover behavior in `menu_bar` while keeping popup routing correct when one process owns multiple popup windows.
- **libui menu hover previews now route pointer events to the focused popup instead of the deepest open submenu.** Nested `UI_COMPONENT_MENU_ITEM` popups are rendered as preview-only when opened from hover, while top-level menu-bar popups and explicit click-opened child submenus still take focus. The shared libui popup router now tracks compositor focus per popup window and dispatches pointer events to the popup that actually owns focus, which fixes the `menu_bar` Apps menu regression where hovering `gfx_smoke` opened its window-list submenu and subsequent moves across single-window entries like `Calculator` or `Minesweeper` were interpreted against the wrong submenu rows.
- **WARP ring-3 startup no longer burns seconds re-growing VT linear memory.** The long pause between `sysinit` spawning `vt` and the later `font-service` startup was not a generic ring-3 cost: a controlled `wasm3` ring-3 shadow build reached `font-service` about 1.2 seconds after `Starting system services...`, while WARP ring-3 had been spending roughly 15 seconds in the same interval. The culprit was WARP's active linear-memory allocator path: large `warp_krealloc` growth repeatedly allocated-and-copied the whole backing block in 4 KiB-ish steps, and the ring-3 linmem mapper reinstalled the full user mapping after each move. The fix gives large WARP reallocations explicit usable-capacity tracking so growth within slack returns the same pointer, and the ring-3 mapper now only maps the newly added tail pages when the backing allocation has not moved. Validation: `cmake --build build-warp-noaot --target run-qemu-test` and `cmake --build build --target run-qemu-test` pass; in the validated WARP non-AOT run the `vt -> font-service` gap dropped from roughly 15.3 seconds to about 1.8 seconds.
- **GFX window sizing contract now uses logical content dimensions.** `GFX_IPC_CREATE_WINDOW` and `GFX_IPC_RESIZE_WINDOW` widths/heights are now interpreted as client-content size, with compositor chrome derived outside that rect. This fixes the calculator window-height regression without requiring apps to know title-bar or border sizes, keeps shared-buffer and damage-rect validation aligned with client pixels, and preserves close/maximize/title-drag/resize hit regions in outer-window space. The Zig calculator app now requests its original `280x350` content size again. Validation: `run-qemu-test` passes.
- **AssemblyScript graphics now have a reusable immediate-mode `libui` layer plus a Minesweeper example.** The repo still lacks a real AssemblyScript Wasm link step for compiling `libui_shim.c` into the same module, so the new `src/libui/assemblyscript/libui.ts` wrapper keeps service-lookup/gfx/shmem transport private and instead exposes a small `libui`-style surface centered on `Context`, `Surface`, `Rect`, and `Button`, while preserving the same content-size window contract used by `libui`. `examples/assemblyscript/minesweeper/` uses that layer to render a small 8x8 board with reveal/flag/reset interactions and title-bar status updates, giving AssemblyScript its first reusable graphical app baseline without depending on font-service text rendering. Validation: `run-qemu-test`.
- **Calculator app no longer depends on WARP `f64` execution for button operations.** The ring-3 WARP build could raise `#UD` (`vector=6`) in the calculator app as soon as `=` evaluated `8 * 2`, with the fault RIP landing inside the user JIT region. The app already carried several WARP-specific float workarounds, so the pragmatic fix was to remove runtime `f64` arithmetic from the app entirely: calculator state now uses scaled fixed-point integers for parse/format/add/subtract/multiply/divide, and startup runs a tiny self-test for the exact `8 * 2 = 16` path before the UI loop starts. Validation: `zig_calculator` builds successfully and the regression path is now exercised during calculator startup under `run-qemu-test`.
- **WARP ring-3 shared-buffer mapping now refreshes the user linear-memory view before overlaying shmem pages.** The `gfx_smoke` serial-garbage regression was not a stray logger write: `warp_shmem_map_auto()` returned a valid offset in the kernel, but ring-3 callers kept running against stale user linear-memory mappings after `ensureLinearSize()` committed or relocated the backing allocation. The first libui framebuffer auto-map therefore left app-side state reading as zeroed/stale, and the later pixel fill wrote through a bogus `mapped_base`, which streamed framebuffer bytes to serial. The fix refreshes the active ring-3 low linear-memory mapping after the probe/commit step and then overlays the shared-memory pages into both the higher-half kernel alias and the ring-3 user VA window; `shmem_unmap()` likewise refreshes the user view after restoring the direct-mapped backing. The same debugging pass also fixed the WASM libc heap bootstrap to start from `&__heap_base` instead of the byte value stored there. Validation: `run-qemu-test` passes, `gfx-smoke` keeps nonzero mapped bases across repeated buffer allocations, and the boot reaches the CLI prompt without the binary serial burst after `[test] gfx smoke main start`.
- **Watchdog stall fix: scheduler `in_scheduler` guard + preempt-guard drain moved before native check.** Two independent sources of false `[watchdog] resched stall` messages for the `vt` process were found and fixed. (1) Scheduler cleanup races: `critical_section_enter()` only calls `preempt_disable()` with no `cli`, so the timer can fire after a context switch returns to the scheduler dispatch (line ~1810) while `current_pid` still points to the yielded process and `in_scheduler=0`. The timer ISR then sees `current_pid=vt`, `need_resched=1`, `pdc=1` and incorrectly accumulates watchdog stall ticks. Fix: set `cpu_local()->in_scheduler=1` BEFORE every `critical_section_enter()` in the scheduler cleanup paths (`process_schedule_once_impl` lines 1734, 1758, 1810 and `process_trampoline` line 590), and add an `in_scheduler` early-return guard to `process_tick()` mirroring the existing check in `process_preempt_from_irq`. (2) Preempt-guard drain placed after the native-process early-return: native processes (elf drivers like `vt`) took an early return before hitting the pdc drain, running their entire initialization with `pdc=1`. Fix: moved the `while (preempt_disable_depth() > 0) preempt_enable()` drain to before the `WASMOS_APP_FLAG_NATIVE` check so all spawn paths drain pdc. Validation: `run-qemu-test` passes with zero watchdog stall messages.
- **Watchdog stall fix (prior): drain preempt guard before WARP JIT compilation.** When `WASMOS_ENABLE_PREEMPT_GUARD` is active, `pm_app_entry` calls `preempt_disable()` at entry to hold pdc=1 through the entire app setup and call sequence. The WARP JIT compilation path (`wasmos_app_start` → `wasm_driver_start` → `initFromBytecode`) can take multiple seconds in QEMU HVF. During that window, the timer fires every 4 ms and observes pdc=1 each tick, accumulating 512+ stall ticks and triggering repeated `[watchdog] resched stall` reports for the `vt` process (pid=30). Fix: drain pdc to 0 (via `while (preempt_disable_depth() > 0) preempt_enable()` + `process_clear_resched()`) immediately before `wasmos_app_start` is called in `pm_app_entry`. The paired `preempt_enable()` calls at every exit path safely become no-ops when pdc is already 0. The secondary drain in `wasm_driver_call_unlocked` is retained as a belt-and-suspenders guard for the call path. Temporary diagnostic logs (`[dbg-r3] call_unlocked pre/post-drain pdc=`, `[wd-dbg] pdc=1 tick=`, `[wd-dbg] stall_ticks=`, and the `pdc=` field in the watchdog stall message) have been removed. Validation: `run-qemu-test` passes with no watchdog stall messages.
- **WARP ring-3 preemption: driver spinlock released before ring-3 IRET; IPC spurious-wake yield added.** `wasm_driver_call_entry` and `wasm_driver_call` in `warp_driver.cpp` held `driver->lock` (via `spinlock_lock`) across the ring-3 IRET, which incremented `preempt_disable_count` to 1 on the executing CPU and permanently blocked timer-based preemption for the duration of the ring-3 module's lifetime. The fix: once `warp_driver_ensure_started` succeeds and the module is fully initialised, the spinlock and the `warp_runtime_enter` heap binding are released before `call_export_mod` is invoked with a non-zero `r3_root`. The `wasm_driver_call_unlocked` path already holds no spinlock and is unaffected. Separately, `warp_ipc_select_one` in `src/kernel/warp/link.cpp` tight-looped on `IPC_EMPTY` (spurious sched_event_wait wake) without yielding; a `process_yield(PROCESS_RUN_YIELDED)` on `IPC_EMPTY` was added to avoid spinning while `in_hostcall=1`. Both changes together restore timer-driven preemption for long-running ring-3 services (notably `vt`) and allow the halt test to complete. Validation: `run-qemu-test` passes.
- **WARP ring-3 execution baseline now reaches both default boot CLI and strict ring-3 smoke.** Internal WARP-loaded drivers/services/apps now run exports through per-module ring-3 state (`user_root`, stack backing, linmem VA) instead of the earlier shared global call state, while the same-CPU return path keeps the transient `setjmp`/CR3 state in `cpu_local_t`. The basedata patching layer now rewrites the stack proxy, stack fences, memory-helper pointer, runtime/custom context pointers, and compiled-binary aliases needed by the user JIT view. Ring-3 linear-memory growth is now serviced by a dedicated syscall trampoline (`WASMOS_SYSCALL_WARP_MEMORY_HELPER`) that lets the kernel wrapper call WARP's `MemoryHelper::extensionRequest`, remap the resized linmem into the active user CR3, and refresh the stack proxy base pointer. Validation on the current tree: `run-kernel-unit-tests`, `run-qemu-test`, and `run-qemu-ring3-test` pass.
- **Kernel ring-3 WARP hostcall entry now preserves ABI-required stack alignment.** The IRQ0, generic IRQ, and syscall assembly entry paths now enter C via a shared aligned-call macro instead of the earlier `subq $8` shim. That fixes repeated WARP hostcall crashes where compiler-generated `movaps` spills in kernel C/C++ code faulted because the interrupt/syscall stubs reached C with a misaligned stack. Several WARP wrapper call sites were also hardened to avoid large zero-initialized stack aggregates in the hottest hostcall/memory-management paths, but the alignment fix in `cpu_isr.S` is the systemic change.
- **Ring-3 preempt and startup timing hardening closed the remaining boot stalls.** IRQ-driven preemption from user mode now records the live CR3 into the saved thread context and rewrites the full privilege-return frame when redirecting back into the kernel scheduler trampoline, preventing stale user `SS:RSP` values from faulting on `iretq`. WARP-backed `vt` startup also now yields between large TTY-grid clears so early boot no longer spends an entire startup slice in one uninterrupted linear-memory touch burst. The remaining implementation debt is that `warp_driver.cpp` still uses a local `#define private public` include shim to patch WARP's `runtime_` pointer without modifying the vendored `libs/warp` tree.
- **Physical memory partition: shmem vs WARP linear-memory zones.** All WARP modules run in ring 0 with a shared kernel page table. WARP linear memory is mapped via `phys | kHalfBase`. If a shmem object and a WARP module's linear memory happened to share the same 8 MiB physical window, WARP's `ensureLinearSize()` zero-fill would alias and corrupt the shmem — specifically, `menu_bar`'s growing committed range would eventually reach gfx-smoke window-2's framebuffer shmem VA and zero it. Fix: physical address space is now partitioned: shmem allocates from `[0, 64 MiB)` via `pfa_alloc_pages_below(WASMOS_SHMEM_PHYS_LIMIT)`, WARP linear memory and large kmalloc allocate from `[64 MiB, 512 MiB)` via the new `pfa_alloc_pages_above(WASMOS_SHMEM_PHYS_LIMIT)`. The two canonical VA ranges are now disjoint, eliminating the aliasing. Long-term fix: run WARP in ring 3 with per-process page tables (as wasm3 does), which eliminates the shared-VA aliasing class entirely.
- **WARP higher-half remap hardening for committed and overlaid pages.** The kernel-side WARP allocators no longer assume that the shared higher-half direct alias always stays present after page-table splits, guard-page holes, or temporary linear-memory overlays. `MemUtils::commitVirtualMemory()` now rebinds committed pages back to their original `phys | KERNEL_HIGHER_HALF_BASE` backing before `ensureLinearSize()` zero-fills them, `warp_shmem_unmap()` restores the overlaid linear-memory window to its direct-mapped backing instead of leaving shared-buffer mappings behind, and the page-backed large-object allocator in `warp/shim.cpp` now explicitly repairs higher-half page mappings before handing memory to WARP runtime objects. This closes the `gfx_smoke` crash where a later WARP commit/copy touched an unmapped higher-half page after the AOT-heavy boot path had exercised more shared-memory and allocator churn.
- **WARP AOT pre-compilation for internal modules.** All drivers, services, and utilities (20 modules, non-native) now carry a WARP-compiled native binary embedded in their `.wap` file (`aot = true` in their `linker.metadata`). At boot, `warp_driver` loads the pre-compiled binary via `initFromCompiledBinary` (DYNAMIC_LINK symbols), skipping JIT entirely. If the AOT binary fails to load for any reason, the driver falls back to JIT automatically. Example apps (`examples/`) and graphical apps (`gfx_smoke`, `menu_bar`) are excluded — they continue to use JIT and are not version-bound to the WARP ABI the way internal modules are. The host-native `warp_aot` tool (built from `src/tools/warp_aot/`) performs the compilation at cmake build time via a `wasmos_maybe_aot_pack()` cmake function. Halt test passes with 14+ `[warp-driver] using AOT binary` messages.
- **WARP linear-memory `ensureLinearSize` zeroing fix.** `ActiveMemoryManager::ensureLinearSize()` zero-initialises newly committed WASM pages before marking them usable. Any hostcall that writes to an uncommitted linear-memory region before the JIT first probes it would have its write overwritten by the subsequent zeroing. Fixed by calling `getLinearMemoryRegion(last_byte_offset, 1)` (triggering `probe()` → `ensureLinearSize()` → zeroing) **before** writing in `warp_phys_map`, `warp_shmem_map`, `warp_shmem_map_auto`, `warp_shmem_refresh`, and `warp_acpi_rsdp_info`. This fix enabled the ACPI bus to parse ACPI tables correctly under WARP, which in turn allows keyboard, mouse, serial, and RTC drivers to start. The `env.abort` symbol (AssemblyScript runtime) was also added to WARP's symbol table.
- **Script engine documentation.** `docs/SCRIPT_ENGINE.md` documents all `.rc` script commands (`start`, `spawn`, `exec`, `wait-svc`, `echo`, `export`, `set`, `script`, `source`, `if`/`else`/`endif`) with syntax, scoping rules, and examples.
- **`sysinit.rc` spawns `menu_bar` and `gfx_smoke` after compositor ready.** `wait-svc gfx` blocks until the compositor registers, then both graphical apps are spawned conditionally (`-f` guards). CLI starts last.
- **WARP JIT backend complete and merged to main.**  Both `wasm3` (default) and WARP (`-DWASMOS_WASM_RUNTIME_WARP=ON`) reach WAMOS CLI in `run-qemu-test`.  See `docs/architecture/31-warp-jit-backend.md` for the full design.  Known remaining gaps: `console_read` unimplemented (CLI stalls on stdin read but boot succeeds), ~30 wasmos.* host-call TODOs in `src/kernel/warp/link.cpp`, and multi-threaded WASM under WARP not yet functional.
- WARP JIT runtime bring-up now gets through the earlier process-manager/app-slot failure and into full user-space startup. The WARP backend lazily runs module `start()` before first export call (matching the wasm3 startup contract for app/service entry paths), hostcall wrappers unwrap the runtime context through `WasmModule::getContext()` before accessing `WarpCallContext`, IPC send/last-field metadata match the WASMOS ABI (`source`/`destination` and arg field numbering), and WARP executable-page allocation explicitly maps higher-half aliases with EXEC permissions instead of assuming the shared 2 MiB kernel window stayed RWX after later page-table splits. Combined with the newer ring-3 linmem-growth, syscall/IRQ stack-alignment, and preempt-frame fixes above, the WARP build now completes the default `run-qemu-test` path to the CLI prompt and also passes `run-qemu-ring3-test`.
- GFX window flags are now split into composable bits instead of the old overloaded single "system" meaning. Shared IPC headers now define `GFX_WINDOW_FLAG_TOPMOST`, `GFX_WINDOW_FLAG_NO_CHROME`, `GFX_WINDOW_FLAG_INVISIBLE`, `GFX_WINDOW_FLAG_PASSTHROUGH_ZERO`, `GFX_WINDOW_FLAG_NO_ACTIVATE`, and `GFX_WINDOW_FLAG_NO_CONTENT`; in-tree callers request the exact combination they need. The compositor now applies those behaviors independently: topmost controls z-order, no-chrome controls content/chrome/drag-resize handling, invisible suppresses rendering and pointer hit-testing/focus acquisition, passthrough-zero only affects zero-pixel compositing, no-activate suppresses implicit activation on click/present while still allowing explicit `GFX_IPC_FOCUS_WINDOW`, and no-content means the window has no compositor-managed client surface (shared-buffer allocation/present are rejected and placeholder body rendering is skipped). Damage rect translation now uses `window_content_rect()` so no-chrome windows repaint in the correct screen location. The menu-bar popup path now opens a dedicated no-chrome/topmost window that includes the owning header row plus submenu body, and popup interaction uses release-based toggle/pick semantics so first click opens, second header click closes, and outside-click dismissal remains clean.
- Base struct + vtable step: `ui_component_t` is now the pure base (tree, bounds, common styling, clickable/pressed, on_click + `void *component_data`). All component-specific state moved into per-type data allocated behind component_data (ui_text_data_t / ui_list_data_t reused where helpful; full structs like ui_checkbox_data_t, ui_dropdown_data_t, ui_menu_item_data_t etc. defined in their headers). Introduced `ui_component_ops_t` vtable (render/layout + the handle_* + popup_contains + destroy_data). Core populates a static table from the functions the component headers provide. All dispatch (layout, render, ipc press/release/key/drag, find_* popup tests) now uses vtable lookup instead of type switches. Core alloc/destroy/setters updated. Smoke apps updated for the few direct field pokes they had. `run-qemu-test` passes (full UI exercised under the new model).
- Further component extraction (after single src/libui tree): the large inline menu release block (full scan over menu items on pointer release to decide bar-item toggle vs popup pick, plus sibling close orchestration) moved to `ui_menu_item_handle_pointer_release(ctx, x, y)` in libui_menu_item.h. The "close every open dropdown on outside click" loop moved to `ui_dropdown_close_all_open(ctx)` in libui_dropdown.h. Core `ui_loop_handle_ipc` now delegates those to the owning components (matching the existing pattern for list/dropdown press, scroll drag, text/dropdown key, button/checkbox release). Generic post-release work (clear all pressed flags, clear active scroll) stays in core. The small on-click dispatch for button/checkbox (to get their pre-on_click action) remains as the lightweight central dispatcher. `run-qemu-test` (including menu_bar exercising the newly extracted paths) passes.
- libui is now its own standalone tree (`src/libui/include/wasmos/libui.h` + the per-component `libui_*.h` headers that own their rendering, layout, popup hit-testing, and event handlers). The prior duplicated copies under the libc and libsys trees (and the thin forwarder stubs that were briefly left there) have been removed completely — "just have a libui". The two current consumers (examples/c/gfx_smoke and menu_bar) continue to use the exact same `#include "wasmos/libui.h"` with zero source changes; the `-I${CMAKE_SOURCE_DIR}/src/libui/include` (placed first for wasm app targets) makes the wasmos/ namespace resolve to the canonical for both the app includes and the inner component includes inside libui.h. Cross-headers such as wasmos/api.h and wasmos/ipc.h continue to be found via the subsequent -I entries (libc + libsys). <stdbool.h> + forward prototypes were added in the canonical for clean C99 compilation of the component headers. Component ownership model (core owns generic tree/clip/dispatch + routing; components own their specifics) is unchanged. `run-qemu-test` (gfx_smoke + menu_bar built + full boot/halt smoke) passes.
- libui split pilot: `libui.h` is now the parent/generic aggregator. Component-specific rendering (starting with label and button) lives in their own headers (`libui_label.h`, `libui_button.h`) included by the main header in both the libc and libsys/wasm trees. The core render path now delegates to the component-owned functions (mechanical extraction + header split). Public APIs and direct `ui_component_t` usage unchanged. `run-qemu-test` (including gfx_smoke + menu_bar) passes. More components (the already-factored render helpers for checkbox/list/etc.) and layout/event ownership can follow the same pattern.
- Extended (larger step): on_click dispatch for button/checkbox now fully through component handlers (ui_button_handle_pointer_release, ui_checkbox_handle_pointer_release) added to their headers (both trees); core on_click block dispatches to them (removing inline type-specific toggle). Event pilot now covers key/pointer reactions + release for main interactive components (dropdown, list, text, menu, button, checkbox) with core owning only routing/orchestration (finds, focus, pressed clear, scroll capture, close-outside, menu id). Added scroll drag handlers to scroll/list headers and dispatch in drag block. Finished extract for other (scrollable) components' event logic. All per design proposal for component responsibility. Tests pass after each.
- Native `libsys` event loops now expose explicit intent cancellation, and the
  current stack-backed synchronous IPC helpers in both `gfx-compositor` and
  `font-service` cancel pending intents before timeout/error exits. This
  closes the stale-reply use-after-return path where a later reply could
  resolve into freed stack state. `gfx-compositor` also now refreshes runtime
  keyboard/mouse subscriptions on a periodic handled-event cadence instead of
  only during idle housekeeping, so continuous `POLL_EVENT` traffic no longer
  starves input re-subscription. Trace markers now also distinguish overlay
  lock/restore transitions and whether a successful `PRESENT_WINDOW` happened
  while the compositor still believed overlay lock was off, which makes
  invisible-cursor regressions easier to diagnose.
- Current SMP crash instrumentation now also watches the live main
  `thread->ctx` for `process-manager` / `native-call-min` instead of the stale
  mirrored `proc->ctx`, and AP bring-up no longer clears the global watch
  state after the BSP arms it. The shared ready queue now additionally halts
  immediately if any caller tries to enqueue a `THREAD_STATE_RUNNING` thread,
  tightening the current race hunt around duplicate-run versus post-save
  context stomp failures.
- SMP scheduler hardening now also removes two concrete cross-CPU bootstrap
  races uncovered by `native-call-min` crash trapping. The scheduler fallback
  trampoline stack is now per-CPU instead of one shared global buffer, so
  concurrent AP low-stack scheduler ingress can no longer scribble over the
  same temporary stack frames. The generic paging helpers also stop using the
  shared `g_current_pml4_phys` as their implicit current-root source and now
  read the actual local CPU `cr3` when mapping/unmapping in the current
  address space. Parked PM children are now born blocked instead of being
  spawned READY and "parked" afterward, closing the SMP race where another CPU
  could dispatch the main thread before `process_spawn_as_parked()` retracted
  it from the queue and later `process_unpark_pid()` would requeue the still-
  running thread on a second CPU.
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
  `OPEN_FONT` + `GET_METRICS` IPC; glyph raster IPC is still TODO. Native
  `libsys` buffer copy/write helpers now round their borrow windows up to
  page size before calling the kernel ABI, which fixes sub-page native FS
  staging/copy operations such as `fsReadPath`. `font-service` now uses
  `FS_IPC_STAT_REQ` to size its SHMEM to the real TTF length and loads each
  built-in font through the shared `fs.vfs` read-path helper instead of
  manual open/read loops. The low-level native `buffer_borrow` ABI now also
  documents and logs invalid non-page-aligned/zero-sized requests explicitly,
  while the higher-level native helper APIs remain byte-range wrappers that
  round their internal borrow windows up before calling the kernel hook.
  `fs_manager` now relays `FS_IPC_READ_PATH_REQ` payloads back to callers in
  4 KiB chunks instead of 256-byte chunks, reducing bounce-copy overhead for
  eager native asset loads such as built-in TTF startup.
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
