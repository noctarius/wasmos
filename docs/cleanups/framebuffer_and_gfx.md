WASMOS Graphics Stack — Performance Analysis

TLDR: The remaining slowness is not primarily pixel copying — it's synchronous IPC amplification (especially text
rendering, which does 2–3 font-service round-trips per string per frame with zero caching at any layer) multiplied by
redraw-everything policies at every level (libui always presents full-window damage, the compositor full-repaints on
every click, damage collapses to one union rect). Double buffering fixed scanout artifacts, but the pipeline still
re-rasterizes, re-blends, and re-copies nearly everything on nearly every frame, and every one of those frames is gated
by a poll-and-yield scheduling dance between three processes.

---

Where the time actually goes (ranked)

1. Text: no caching anywhere, IPC per string per frame — the dominant cost

Every ui_draw_text_clip call does, unconditionally, every frame: copy string to shmem + flush, a synchronous
FONT_IPC_MEASURE_GLYPH round-trip, then a synchronous FONT_IPC_RASTER_GLYPH_INTO round-trip + mask refresh (libui.h:
493–552). Buttons add a third round-trip for centering (libui_button.h:26). There is no glyph cache, no string→mask
cache, and no "text unchanged" check in libui — and the font service has no cache either: it re-runs
stbtt_GetCodepointBitmapBox/stbtt_MakeCodepointBitmap for every character of every request and does
shmem_map/shmem_unmap per request (font_service.zig:613–729). A completely static label pays full TrueType rasterization
plus two cross-process round-trips per frame.

Worse, list and tree views render all items, including off-screen ones — the loops in libui_list_view.h:39–47 and
libui_tree_view.h:39–55 have no visible-range skip, and clipping happens per-pixel after the IPC round-trips (no bbox
early-out in ui_draw_text_clip). A 500-entry directory in explorer ≈ 1,000 font IPC round-trips per frame, ~980 of them
for invisible rows.

2. Redraw-everything policies defeat the damage machinery that already exists

- libui has a single global dirty bool. Any change → full relayout, full-tree repaint, shmem_flush of the entire
  buffer (~1.2 MB at 640×480), and PRESENT_WINDOW with damage_count=0 = full-window damage (libui.h:1878–1904). The
  damage-rect ABI exists (gfx_ipc.h:56–57) and the compositor honors it — libui just never uses it.
- Pointer release marks dirty unconditionally even on a miss (libui.h:1774), so every click = 2 full redraws; every drag
  event = one full redraw + flush + present (libui_list_view.h:131).
- Compositor side: handle_mouse_press_transition calls request_repaint_full() on every left-press over a window (
  gfx_compositor.zig:1241), as do focus/flags/destroy paths.
- Damage collapses to a single union rect (gfx_compositor.zig:1095–1104) — a cursor move plus a caret blink in opposite
  corners recomposes the whole span between them.

3. Everything is poll-driven

The app busy-loops on a synchronous GFX_IPC_POLL_EVENT per iteration with no yield on EVENT_NONE (explorer.c:848–858,
libui_shim.c:305–314) — an idle GUI app spins at 100% CPU hammering the compositor with no-op sync IPCs, stealing time
from the compositor itself. The compositor in turn spins on non-blocking ipc_recv + sched_yield (gfx_compositor.zig:
3060–3084), and repaints only when its poll batch goes idle — frame rate is coupled to IPC batching and scheduler
behavior. Every synchronous round-trip in items 1–2 pays this scheduling latency. This is exactly the problem your
planned async-IPC/futures work targets.

4. Per-pixel inner loops and scalar memcpy under all of it

- draw_window_buffer copies window content pixel-by-pixel even when formats match, because of the per-pixel
  passthrough_zero branch and | 0xFF000000 (gfx_compositor.zig:2267–2301). So every composited pixel is copied twice per
  frame (shmem→backbuffer per-pixel, backbuffer→FB via memcpy), plus a background fill_rect with no occlusion culling.
- libui's text blend does a 6-way bounds test and divide-by-255 read-modify-write per pixel (libui.h:575–590, 394–407);
  parent/child/row backgrounds overdraw the same pixels 3-deep.
- All bulk copies go through an "intentionally simple" scalar 8-byte memcpy (kernel libc.c:45–70, services string.c:
  200–228, duplicated nd_memcpy in render.c:19–41). Kernel memset already uses rep stosb with an ERMS rationale — memcpy
  never got the same treatment. The Zig shim's memcpy is byte-at-a-time (libui_shim.c:128–134).

. The legacy text path has its own pathologies

- fbtext_scroll_up memcpys ~3 MB of live framebuffer per newline at screen bottom (render.c:183–186), through the scalar
  memcpy.
- The VT service sends one IPC message per character cell (vt_main.c:555, 566); a full-screen replay ≈ 6,100 syscall
  round-trips with yield-retry loops.

What's not the problem (worth knowing)

- Cache attributes are fine on QEMU. The FB is mapped write-back everywhere (no PAT/WC exists in the kernel — paging.c:
  14–18, 567–584), but the default stdvga BAR is guest RAM, so WB is actually the fastest choice on this target. On real
  hardware WB scanout would be wrong and you'd need PAT/WC — worth a TODO, not a today-fix.
- The backbuffer→scanout blit is already row/bulk memcpy with a full-width fast path (gfx_compositor.zig:2381–2394),
  buffer shmem mappings are cached at alloc time, and damage plumbing exists end-to-end. The bones are good.
- One QEMU caveat: with -nographic (default test targets) there's no display listener, so FB write cost is invisible —
  perf only shows in run-qemu-ui, where full-surface writes force QEMU to reconvert the whole dirty surface each ~30 Hz
  refresh. Small damage rects help QEMU too, not just the guest.

---
Improvement roadmap (ranked by impact ÷ effort)

1. Glyph atlas + text caching — biggest single win. Client-side: cache rendered masks keyed by (text, font_px),
   invalidate on set_text; skip re-copy/flush of unchanged strings. Server-side: per-(font,px) glyph bitmap cache, or
   better, a glyph atlas delivered once via shmem so steady-state text drawing needs zero IPC. Also stop recomputing
   metrics twice per raster request (font_service.zig:675).
2. Visible-range culling in list/tree views — compute first/last visible row from scroll_y/item_h; add a rect-vs-clip
   early return at the top of ui_draw_text_clip. Turns O(total items) IPC into O(visible).
3. Use the damage-rect ABI end-to-end. libui: per-component dirty flags, accumulate a damage union during render, pass
   it to PRESENT_WINDOW, and restrict shmem_flush to damaged rows. Compositor: replace the single union rect with a
   small fixed damage list (4–8 rects); stop request_repaint_full() on click/focus (only the chrome of the two affected
   windows changes); drop libui's unconditional dirty-on-release.
4. Row-memcpy fast path in draw_window_buffer. Define an alpha contract (clients write opaque pixels, or the X channel
   is ignored at scanout — stdvga ignores it anyway) so the fmt==1 non-passthrough case becomes on @memcpy per row. Keep
   per-pixel only for the fmt==0 swizzle and passthrough windows. Add opaque-window occlusion so the desktop fill and
   lower windows skip covered regions.
5. Fix the event loops. Short term: drain all pending events, render once (the AssemblyScript wrapper's pump(limit=8)
   already does this); sched_yield on EVENT_NONE; coalesce drag events. Medium term: your planned async-IPC/futures
   rework, plus either event push to a per-client shmem event ring or a blocking poll — this removes both the idle CPU
   burn and the per-event round-trip.
6. One good rep movsb memcpy, shared by kernel libc and services libc (mirroring the existing rep stosb memset
   rationale, and per the keep-libc-in-sync rule), deleting the duplicated nd_memcpy/byte-loop nd_memset in render.c and
   the byte-loop memcpy in libui_shim.c. Cheap, benefits every path above.
7. fbtext/VT batching (matters for console, not GUI): scroll the cell grid and repaint from cells instead of memcpying
   the framebuffer per newline; replace per-cell IPC with row-span messages or a shared cell grid + damage doorbell.
8. Smaller items: cheap blend ((x + (x>>8) + 1) >> 8 instead of /255, skip blend at alpha 255); 2 MiB mappings for
   FB/backbuffer to cut TLB pressure (768 PTEs today for 3 MB); ui_component_by_id linear scan makes tree walks O(n²) as
   component counts grow.

A useful mental model for sequencing: items 1–3 remove work (IPC and redraws that shouldn't happen), items 4–6 make the
remaining work fast, and item 5's async-IPC part removes the latency floor under everything. I'd expect 1+2 alone to
transform text-heavy apps like explorer, since their cost currently scales with total item count × 2 round-trips ×
TrueType rasterization per frame.
