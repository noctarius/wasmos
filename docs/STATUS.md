# Current Status

- This status file is a snapshot, not a release changelog.
- Ring-3 strict isolation/hardening, threading phase rollout, DMA rollout,
  filesystem/PM service discovery, and CLI/runtime updates are tracked in the
  dedicated docs under `docs/architecture/`.
- CLI `ps` process diagnostics now also expose runtime kind (`wasm` true/false
  in table view and `wasm=true|false` annotations in tree view), sourced from
  process-manager spawn metadata.
- Threading is production-complete for the current single-core scope; final
  ABI/policy decisions and closure status are in
  `docs/architecture/15-threading-and-lifecycle.md` sections 15 and 17.
- Recent threading runtime hardening (user-thread kernel-stack setup for
  `THREAD_CREATE` and syscall frame/context synchronization for yield/block
  paths) is documented in `docs/architecture/15-threading-and-lifecycle.md`.
- Graphics/compositor Phase 0 scaffold (shared ABI constants and minimal
  native Zig `gfx-compositor` endpoint handshake path) is tracked in
  `docs/architecture/17-graphics-framebuffer-and-compositor.md`.
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
  covers two concurrent windows and close-event teardown of each window.
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
- Higher-level components may use C++, while low-level kernel boundaries stay
  C/ASM. WASM C++ build policy is no exceptions/RTTI and explicit C ABI at
  integration points.
- Process-manager test injection hooks are now behind a dedicated Kconfig/CMake
  switch (`WASMOS_PM_TEST_HOOKS`) and are no-op when disabled.
