## Graphics, Framebuffer, and Compositor

This document describes the implemented graphics stack: component topology,
IPC opcode contracts, compositor internals, font service, and the libui toolkit.

---

### Component Topology

```
app (WASM) ──GFX IPC (0x200–0x2FF)──► gfx-compositor (native Zig)
                                            │
                                 FBTEXT IPC (0x600–0x6FF)
                                            │
                                    framebuffer driver
                                  (framebuffer_pci or framebuffer)
                                            │
                                    kernel native driver ABI
                                    (wasmos_driver_api_t)

gfx-compositor ──FONT IPC (0xA00–0xAFF)───► font-service (native Zig)
gfx-compositor ──KBD IPC (0x800–0x8FF)────► keyboard driver (subscription)
gfx-compositor ──MOUSE IPC ───────────────► mouse driver (subscription)
```

Registered endpoint names: `gfx` (compositor), `font` (font service).
Both services are native ELF binaries using `wasmos_driver_api_t`.

Service manifests (`linker.metadata`):

| Field                  | gfx-compositor | font-service |
|------------------------|----------------|--------------|
| kind                   | service        | service      |
| entry                  | initialize     | initialize   |
| native                 | true           | true         |
| stack_pages            | 16             | 16           |
| heap_pages             | 16             | 16           |
| required_endpoint_name | proc           | proc         |
| capabilities           | ipc.basic      | ipc.basic    |

---

### ABI Constants

Defined in `src/libc/include/wasmos/gfx_ipc.h` and
`src/drivers/include/wasmos_driver_abi.h`:

```c
FB_IPC_ABI_MAGIC   = 0x46424950u   /* 'FBIP' */
FB_IPC_ABI_VERSION = 1u

GFX_IPC_ABI_MAGIC   = 0x47465850u  /* 'GFXP' */
GFX_IPC_ABI_VERSION = 1u
```

Validation helper (`gfx_ipc.h`):
```c
// gfx_ipc_header_pack(version, opcode) → arg3 in CREATE_WINDOW request
// gfx_ipc_header_valid(magic, ver_opcode) → 1 if magic and version match
```

---

### FBTEXT IPC (0x600–0x6FF)

These opcodes are used by the VT service and the compositor to control the
framebuffer text/graphics plane. Both `framebuffer` (UEFI-backed) and
`framebuffer_pci` (PCI-enumerated) drivers handle this namespace.

#### Opcodes

| Opcode                          | Value | Args / Reply                                      |
|---------------------------------|-------|---------------------------------------------------|
| `FBTEXT_IPC_CELL_WRITE_REQ`     | 0x600 | arg0=col arg1=row arg2=codepoint arg3=(fg<<8)\|bg |
| `FBTEXT_IPC_CURSOR_SET_REQ`     | 0x601 | arg0=col arg1=row                                 |
| `FBTEXT_IPC_SCROLL_REQ`         | 0x602 | arg0=n_rows                                       |
| `FBTEXT_IPC_CLEAR_REQ`          | 0x603 | (no args)                                         |
| `FBTEXT_IPC_CONSOLE_MODE_REQ`   | 0x604 | arg0: 0=ring off, 1=ring on                       |
| `FBTEXT_IPC_GEOMETRY_REQ`       | 0x605 | reply: arg0=cols arg1=rows                        |
| `FBTEXT_IPC_GFX_OVERLAY_REQ`    | 0x606 | arg0: 0=unlock, 1=lock                            |
| `FBTEXT_IPC_QUERY_CAPS_REQ`     | 0x607 | reply: arg0=FBTEXT_CAP_* bitmask                  |
| `FBTEXT_IPC_QUERY_MODES_REQ`    | 0x608 | req: arg0=index; reply: arg0=w arg1=h arg2=stride |
| `FBTEXT_IPC_SET_RESOLUTION_REQ` | 0x609 | req: arg0=w arg1=h                                |
| `FBTEXT_IPC_RESP`               | 0x680 | success reply                                     |
| `FBTEXT_IPC_ERROR`              | 0x6FF | error reply                                       |

Capability flags (`FBTEXT_CAP_*`, `wasmos_driver_abi.h`):
```c
FBTEXT_CAP_SET_RESOLUTION = 1u << 0
FBTEXT_CAP_QUERY_MODES    = 1u << 1
```

#### Driver Variant Differences

| Opcode         | `framebuffer_pci`                                     | `framebuffer` (UEFI) |
|----------------|-------------------------------------------------------|----------------------|
| QUERY_CAPS     | `FBTEXT_CAP_SET_RESOLUTION \| FBTEXT_CAP_QUERY_MODES` | 0 (no caps)          |
| QUERY_MODES    | returns mode list                                     | `FBTEXT_IPC_ERROR`   |
| SET_RESOLUTION | applies mode switch, reborrow                         | `FBTEXT_IPC_ERROR`   |

#### GFX Overlay Lock

`FBTEXT_IPC_GFX_OVERLAY_REQ` sets `g_gfx_overlay_lock` on both drivers.
While locked, `CELL_WRITE`, `CURSOR_SET`, `SCROLL`, and `CLEAR` requests
are silently dropped. The compositor locks the overlay when any window is
presenting (`active_presented_window_count() > 0`) and unlocks when none are.

---

### GFX IPC (0x0200–0x02FF)

App-facing compositor interface. Defined in
`src/libc/include/wasmos/gfx_ipc.h`.

#### Opcodes and Argument Contracts

| Opcode                          | Value  | Request args                                                                   | Reply args                                      |
|---------------------------------|--------|--------------------------------------------------------------------------------|-------------------------------------------------|
| `GFX_IPC_CREATE_WINDOW`         | 0x0200 | arg0=content_width arg1=content_height arg2=GFX_IPC_ABI_MAGIC arg3=header_pack(version,opcode) | arg1=window_id arg2=content_width arg3=content_height |
| `GFX_IPC_DESTROY_WINDOW`        | 0x0201 | arg0=window_id                                                                 | —                                               |
| `GFX_IPC_RESIZE_WINDOW`         | 0x0202 | arg0=window_id arg1=content_width arg2=content_height                          | arg1=content_width arg2=content_height          |
| `GFX_IPC_ALLOC_SHARED_BUFFER`   | 0x0203 | arg0=window_id (0=unbound) arg1=width arg2=height                              | arg1=buffer_id arg2=shmem_id arg3=stride        |
| `GFX_IPC_SUBMIT_COMMANDS`       | 0x0204 | (not yet dispatched)                                                           | —                                               |
| `GFX_IPC_PRESENT_WINDOW`        | 0x0205 | arg0=window_id arg1=buffer_id arg2=damage_count arg3=damage_shmem_id           | —                                               |
| `GFX_IPC_POLL_EVENT`            | 0x0206 | —                                                                              | arg1=event_type arg2=event_arg1 arg3=event_arg2 |
| `GFX_IPC_RELEASE_SHARED_BUFFER` | 0x0207 | arg0=buffer_id                                                                 | —                                               |
| `GFX_IPC_SET_DISPLAY_MODE`      | 0x0208 | arg0=width arg1=height                                                         | arg1=width arg2=height                          |
| `GFX_IPC_RESP`                  | 0x0280 | success reply; arg0=GFX_STATUS_*                                               | —                                               |
| `GFX_IPC_ERROR`                 | 0x02FF | error reply                                                                    | —                                               |

#### Status Codes

```c
GFX_STATUS_OK          =  0
GFX_STATUS_INVALID     = -1   // bad argument or malformed request
GFX_STATUS_PERMISSION  = -2   // window/buffer not owned by caller
GFX_STATUS_UNSUPPORTED = -3   // operation not available
GFX_STATUS_BUSY        = -4   // resource in use; retryable
GFX_STATUS_IO          = -5   // device or shmem failure
```

#### Event Types (`GFX_POLL_EVENT` replies)

```c
GFX_EVENT_NONE          = 0
GFX_EVENT_FOCUS_GAINED  = 1
GFX_EVENT_FOCUS_LOST    = 2
GFX_EVENT_KEY           = 3   // arg2=translated key (ASCII/ctrl), arg3: bit0=keyup, bit1=extended
GFX_EVENT_POINTER       = 4   // arg2=x/y packed as u16 low16=x high16=y, arg3=button mask
GFX_EVENT_CLOSE_REQUEST = 5   // arg2=window_id
GFX_EVENT_RESIZE        = 6   // arg2=window_id, arg3=width/height packed u16 low16=w high16=h
```

Window geometry note:
- `CREATE_WINDOW` and `RESIZE_WINDOW` widths/heights are content dimensions.
- Compositor chrome is outside the client rect, so title-bar or border-size changes do not require app-side size adjustments.

---

### FONT IPC (0xA00–0xAFF)

Font service interface. Defined in `src/libc/include/wasmos/font_ipc.h`.

#### Opcodes

| Opcode                           | Value | Request args                                                      | Reply args                                            |
|----------------------------------|-------|-------------------------------------------------------------------|-------------------------------------------------------|
| `FONT_IPC_OPEN_FONT_REQ`         | 0xA00 | arg0=font_id arg1=px_size (1–256)                                 | arg1=handle_id                                        |
| `FONT_IPC_GET_METRICS_REQ`       | 0xA01 | arg0=handle_id                                                    | arg1=ascent(i32) arg2=descent(i32) arg3=line_gap(i32) |
| `FONT_IPC_RASTER_GLYPH_REQ`      | 0xA02 | arg0=handle_id arg1=codepoint                                     | arg1=shmem_id arg2=pack(w,h) arg3=pack(x0,y0)         |
| `FONT_IPC_MEASURE_GLYPH_REQ`     | 0xA03 | arg0=handle_id arg1=text_shmem_id arg2=text_len                   | arg1=pack(w,h) arg2=pack(x0,y0) arg3=advance_x        |
| `FONT_IPC_RASTER_GLYPH_INTO_REQ` | 0xA04 | arg0=handle_id arg1=text_shmem_id arg2=text_len arg3=dst_shmem_id | arg1=pack(w,h) arg2=pack(x0,y0) arg3=advance_x        |
| `FONT_IPC_RESP`                  | 0xA80 | success reply; arg0=FONT_STATUS_OK                                | —                                                     |
| `FONT_IPC_ERROR`                 | 0xAFF | error reply; arg0=FONT_STATUS_*                                   | —                                                     |

`pack(w,h)` uses `sys.packU16Pair` (w in low 16, h in high 16);
`pack(x0,y0)` uses `sys.packS16Pair`.

#### Font IDs

```c
FONT_ID_ROBOTO      = 1   // /boot/system/fonts/roboto.ttf
FONT_ID_ROBOTO_MONO = 2   // /boot/system/fonts/roboto_mono.ttf
FONT_ID_NOTO_SERIF  = 3   // /boot/system/fonts/roboto_serif.ttf
```

#### Status Codes

```c
FONT_STATUS_OK          =  0
FONT_STATUS_INVALID     = -1
FONT_STATUS_PERMISSION  = -2
FONT_STATUS_UNSUPPORTED = -3
FONT_STATUS_IO          = -4
FONT_STATUS_BUSY        = -5
```

---

### Native Driver API (`wasmos_driver_api_t`)

Defined in `src/drivers/include/wasmos_native_driver.h`. Both graphics
services are native ELF drivers and receive the full API table:

```c
typedef struct {
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_stride;       // pixels per row
    uint32_t framebuffer_gop_pixel_format;  // from UEFI GOP; 0=BGRA
} nd_framebuffer_info_t;

// Buffer borrow (native driver side)
ND_BUFFER_KIND_FS          = 1u
ND_BUFFER_KIND_FRAMEBUFFER = 2u
ND_BUFFER_BORROW_READ      = 0x1u
ND_BUFFER_BORROW_WRITE     = 0x2u

// ABI versioning
WASMOS_NATIVE_ABI_MAGIC   = 0x574E4150u  /* 'WNAP' */
WASMOS_NATIVE_ABI_VERSION = 3u
```

The compositor calls `api().buffer_borrow(ND_BUFFER_KIND_FRAMEBUFFER, 0,
ND_BUFFER_BORROW_READ | ND_BUFFER_BORROW_WRITE, fb_size)` to get a direct
pointer to the linear scanout surface.

---

### Compositor Internals (`gfx_compositor.zig`)

#### Key Constants

```zig
GFX_MAX_WINDOWS    : usize = 32
GFX_MAX_BUFFERS    : usize = 64
GFX_MAX_DAMAGE_RECTS : u32 = 256
GFX_MAX_EVENTS     : usize = 128
GFX_MAX_GLYPH_CACHE: usize = 64
GFX_MAX_GLYPH_BYTES: usize = 4096
GFX_WINDOW_MIN_DIM : u32 = 1
GFX_WINDOW_MAX_DIM : u32 = 8192

// Window chrome geometry
CHROME_BORDER      : i32 = 1
CHROME_TITLE_H     : i32 = 24   // title bar height in pixels
CHROME_CLOSE_SZ    : i32 = 14   // close button size
CHROME_MAX_SZ      : i32 = 14   // maximize button size
CHROME_RESIZE_HANDLE_SZ : i32 = 12
CHROME_TITLE_FONT_PX    : u32 = 14
CURSOR_W : i32 = 9
CURSOR_H : i32 = 14
```

#### Core Data Structures

```zig
const window_slot_t = struct {
    in_use: bool,
    owner_endpoint: u32,
    window_id: u32,          // monotonically assigned from g_next_window_id
    x: i32, y: i32,          // screen-space top-left
    width: u32, height: u32, // content area (excluding chrome)
    z: u32,                  // z-order (lower = behind)
    generation: u32,         // incremented on resize; invalidates buffer bindings
    current_buffer_id: u32,  // buffer presented via PRESENT_WINDOW
    is_maximized: bool,
    restore_x/y/w/h,         // pre-maximize geometry
};

const buffer_slot_t = struct {
    in_use: bool,
    owner_endpoint: u32,
    buffer_id: u32,          // opaque random 32-bit ID
    shmem_id: u32,           // shmem backing, granted to owner at alloc
    width: u32, height: u32,
    stride_bytes: u32,       // width * 4 (BGRA32)
    state: buffer_state_t,   // .allocated or .acquired
    bound_window_id: u32,
    bound_window_generation: u32,
};

const gfx_event_t = struct {
    endpoint: u32,     // destination endpoint for delivery
    event_type: u32,
    arg1/arg2/arg3: u32,
};

const glyph_cache_entry_t = struct {
    valid: bool,
    codepoint: u32,
    shmem_id: u32,
    w: i32, h: i32,
    x0: i16, y0: i16,
    mask_len: usize,
    mask_data: [4096]u8,   // alpha mask, copied from font service shmem
};
```

#### Initialization Sequence

`initialize()` performs these steps in order:

1. Validate `abi_magic`/`abi_version` against `WASMOS_NATIVE_ABI_MAGIC` / version 3.
2. Create `g_gfx_endpoint`, initialize `NativeEventLoop`, register IPC handlers.
3. `svc_register("gfx", 1)` — publish endpoint name.
4. `lookup_fb_endpoint()` via proc IPC; if found:
   - `lookup_vt_endpoint()`, subscribe keyboard and mouse.
   - Probe geometry (`FBTEXT_IPC_GEOMETRY_REQ`) and capabilities (`FBTEXT_IPC_QUERY_CAPS_REQ`).
   - Emit `[test] gfx compositor handshake ok`.
5. `refresh_framebuffer_mapping()` — query `framebuffer_info`, borrow
   `ND_BUFFER_KIND_FRAMEBUFFER` read+write into `g_fb_pixels`.
6. `ensure_backbuffer_capacity(fb_stride * fb_height * 4)` — allocate
   shmem-backed backbuffer at `g_backbuffer_pixels`.
   TODO: backbuffer shmem is not reclaimed on mode change (shmem-destroy not yet exposed).
7. Initialize pointer to screen center.
8. `proc_notify_ready()`, enter event loop.

#### Event Loop

```
while true:
    handled = eventLoopPoll(&ipc_loop, 32)
    if handled > 0:
        every 256 delivered events:
            refresh_input_subscriptions_runtime()
    if handled == 0:
        ensure_font_title_ready_lazy()   // lazy font open for chrome titles
        prime_title_glyph_step()         // pre-rasterize one title character
        flush_repaint_if_pending()       // flush dirty rect or full repaint
        every 64 idles:
            cleanup_orphaned_state()     // reap windows/buffers with dead endpoints
        sched_yield()
    else:
        flush_repaint_if_pending()
```

#### Dirty Tracking and Repaint

Two functions accumulate damage before composition:

- `request_repaint_rect(r)` — unions `r` into `g_dirty_rect`; sets `g_dirty_pending`.
- `request_repaint_full()` — sets `g_dirty_full = true`.

`flush_repaint_if_pending()`:
- If `g_dirty_full`: call `compose_full()` (= `compose_region` over full screen).
- Else: call `compose_region(g_dirty_rect)`.
- Clears `g_dirty_pending`, `g_dirty_full`, `g_dirty_rect`.

#### Composition Pipeline (`compose_region`)

```
1. fill_rect(region, 0x101820)               // dark-blue desktop background
2. collect all in_use windows → order[]
3. insertion sort order[] by z (ascending)   // lowest z drawn first
4. for each window in z order:
   - clip window rect to compose region
   - if current_buffer_id set → draw_window_buffer (shmem pixel blit)
   - else → draw_window_placeholder (solid color)
   - draw_window_chrome (title bar, close/max buttons, border, resize handle)
5. if g_overlay_locked:
   - draw_cursor_overlay(region)             // 9×14 pixel arrow cursor
6. blit completed backbuffer[region] → g_fb_pixels[region]  // scanout write
```

The final step is a row-by-row copy from `g_backbuffer_pixels` into the
borrowed scanout surface, clipped to the compose region. Only the dirty
region is written to the scanout buffer on each compose cycle.

#### Present Window Logic

`handle_present_window` enforces:
1. `window_id` and `buffer_id` non-zero, both owned by caller.
2. Buffer dimensions ≥ window content dimensions.
3. Buffer not already acquired by a different window.
4. Damage rects: if `damage_count == 0` or `damage_shmem_id == 0` or any rect
   is invalid/out-of-bounds → `request_repaint_full()`.
5. Valid damage rects are translated to screen space and enqueued via
   `request_repaint_rect()`.
6. Sets `buffer.state = .acquired`; emits `[test] gfx damage present path ok`
   on first valid damage present.

#### Display Mode Switch (`handle_set_display_mode`)

1. Query `FBTEXT_IPC_QUERY_CAPS_REQ`; reject with `GFX_STATUS_UNSUPPORTED` if
   `FBTEXT_CAP_SET_RESOLUTION` is absent.
2. Send `FBTEXT_IPC_SET_RESOLUTION_REQ` with requested width/height.
3. On success: re-call `refresh_framebuffer_mapping()`, rebuild backbuffer
   at new size, clamp pointer and window positions into new bounds,
   emit `[gfx] fb mode <W>x<H>`, call `request_repaint_full()`.

---

### Font Service Internals (`font_service.zig`)

#### Data Structures

```zig
const loaded_font_t = struct {
    available: bool,
    font_id: u32,
    shmem_id: u32,
    ptr: ?[*]const u8,
    len: usize,
    font_info: stbtt_fontinfo,   // stb_truetype parsed state
    font_info_ready: bool,
    units_per_em: u16,
    ascent: i16,    // design-space, scaled to px at request time
    descent: i16,
    line_gap: i16,
};

const font_handle_t = struct {
    in_use: bool,
    owner_endpoint: u32,
    handle_id: u32,
    font_id: u32,
    px_size: u32,
};
```

Limits: `MAX_FONTS = 3`, `MAX_HANDLES = 16`, `RASTER_SCRATCH_BYTES = 4096`.

#### Initialization Sequence

1. Create `g_font_endpoint`, initialize `NativeEventLoop`, register handlers.
2. `svc_register("font", 1)`.
3. `svcLookupRetry("fs.vfs", ...)` up to 64 retries → `g_fs_endpoint`.
4. If fs found: `load_builtin_fonts()` — reads each TTF from
   `/boot/system/fonts/{roboto,roboto_mono,roboto_serif}.ttf` into right-sized
   shmem after an `FS_IPC_STAT_REQ` size probe, then loads the file through
   the shared native `fsReadPath()` helper (`FS_IPC_READ_PATH_REQ` via
   `fs.vfs`) before calling `stbtt_InitFont` + `parse_ttf_metrics`. Emits
   `[font] loaded ok` per font or `[font] load failed` / `[font] stb init failed`.
5. Emit `[font] service ready`.
6. `proc_notify_ready()`, enter event loop.

#### IPC Handler Contracts

**OPEN_FONT** (`arg0=font_id`, `arg1=px_size 1–256`):
- Allocates a `font_handle_t` slot; returns `handle_id` in `arg1`.
- Handle is owner-scoped: `owner_endpoint == req.source`.

**GET_METRICS** (`arg0=handle_id`):
- Returns `ascent`/`descent`/`line_gap` as signed `i32` scaled to `px_size`.
- Scaling: `(design_value * px_size) / units_per_em`.

**RASTER_GLYPH** (`arg0=handle_id`, `arg1=codepoint`):
- Calls `stbtt_GetCodepointBitmapBox` + `stbtt_MakeCodepointBitmap`.
- Returns alpha mask in a shmem scratch buffer (≤ 4096 bytes).
- Reply: `arg1=shmem_id` (granted to caller), `arg2=pack(w,h)`,
  `arg3=pack(x0,y0)`.

**MEASURE_GLYPH** (`arg0=handle_id`, `arg1=text_shmem_id`, `arg2=text_len`):
- Reads text from caller-provided shmem, calls stb measure APIs.
- Returns `arg1=pack(w,h)`, `arg2=pack(x0,y0)`, `arg3=advance_x`.

**RASTER_GLYPH_INTO** (`arg0=handle_id`, `arg1=text_shmem_id`,
`arg2=text_len`, `arg3=dst_shmem_id`):
- Rasterizes text into caller-provided destination shmem.
- Calls `shmem_flush` to ensure cache coherence before reply.
- Returns same layout as MEASURE_GLYPH.

---

### libui Toolkit (`src/libc/include/wasmos/libui.h`)

Immediate-mode component tree for WASM apps. Components are laid out by
`ui_render_component` on every dirty frame.

#### Component Types

```c
typedef enum {
    UI_COMPONENT_NONE        = 0,
    UI_COMPONENT_PANEL       = 1,
    UI_COMPONENT_LABEL       = 2,
    UI_COMPONENT_BUTTON      = 3,
    UI_COMPONENT_CHECKBOX    = 4,
    UI_COMPONENT_TEXT_INPUT  = 5,
    UI_COMPONENT_SCROLL_VIEW = 6,
    UI_COMPONENT_LIST_VIEW   = 7,
    UI_COMPONENT_DROPDOWN    = 8
} ui_component_type_t;
```

#### Context and Component

```c
typedef struct ui_context {
    // GFX endpoint, window handle, compositor buffer
    // font state (handle, metrics, shmem for text/mask)
    // event queue and dirty flag
    ui_component_t *components;
    int32_t component_count;
    int32_t component_capacity;
} ui_context_t;
```

Initial capacity: `UI_COMPONENTS_INITIAL_CAP = 16`.

#### Key API Functions

- `ui_loop_handle_ipc(ctx, msg)` — consume compositor event replies
  (POLL_EVENT responses, resize, close events).
- `ui_loop_drain(ctx)` — layout + render root component + `PRESENT_WINDOW`.
- `ui_mark_dirty(ctx)` — schedule repaint on next drain.
- `ui_font_measure_text(ctx, text, ...)` — measure string width/height
  via FONT IPC.
- `ui_draw_text_clip(ctx, x, y, text, color, clip)` — rasterize and
  alpha-blend text into compositor buffer within clip rect.

Scroll/list views support drag-based vertical scroll with clipped viewport
composition. Text inputs handle printable characters and backspace. Button
click callbacks are registered via `ui_button_click_cb_t`.

TODO: libui font shmem IDs are not reclaimed on buffer growth.

---

### Security Invariants

All window and buffer operations check `owner_endpoint == msg.source`
before mutation. First violation per session logs a one-shot marker and
returns `GFX_STATUS_PERMISSION`:

| Scenario                                     | Marker                               |
|----------------------------------------------|--------------------------------------|
| Non-owner destroys/resizes/presents a window | `[test] gfx window owner deny ok`    |
| Non-owner releases a buffer                  | `[test] gfx buffer owner deny ok`    |
| Successful handshake with framebuffer        | `[test] gfx compositor handshake ok` |
| First valid damage-rect present              | `[test] gfx damage present path ok`  |

Buffer generation tracking: `buffer.bound_window_generation` is validated
against `window.generation` at present time. A buffer bound to a window
before resize is rejected after the resize increments `generation`.

---

### Structural Invariants

1. **IPC carries only control; pixels go through shmem.** The GFX IPC message
   fields carry `buffer_id`, `shmem_id`, and `damage_count` — never raw pixel
   data. Pixel payloads are allocated via `shmem_create` and accessed by
   pointer through `shmem_map`.

2. **Backbuffer before scanout.** The compositor always renders into
   `g_backbuffer_pixels` first, then copies only the compose region to
   `g_fb_pixels`. This prevents partial-frame scanout artifacts.

3. **One active buffer per window.** `buffer.state = .acquired` prevents a
   second `PRESENT_WINDOW` for a different buffer while one is active,
   unless it is the same buffer already acquired for that window.

4. **Overlay lock gates text plane.** When any window is presenting, the
   compositor sends `FBTEXT_IPC_GFX_OVERLAY_REQ(1)` to suppress VT text
   rendering and owns the scanout surface exclusively.

5. **Font handles are owner-scoped.** Every `OPEN_FONT` records
   `owner_endpoint`; `GET_METRICS`, `RASTER_GLYPH`, and related calls
   reject requests from non-owner endpoints with `FONT_STATUS_PERMISSION`.

6. **Runtime input recovery survives event floods.** Every 256 delivered IPC
   events, `refresh_input_subscriptions_runtime()` re-checks keyboard/mouse
   subscriptions so continuous client polling cannot starve late-service
   input recovery.

7. **Orphan cleanup remains idle-triggered.** Every 64 idle ticks,
   `cleanup_orphaned_state()` checks whether window and buffer owner endpoints
   are still alive (`endpoint_alive`) and reclaims slots for dead owners.
   This handles app crashes without explicit
   `DESTROY_WINDOW`/`RELEASE_SHARED_BUFFER`.
