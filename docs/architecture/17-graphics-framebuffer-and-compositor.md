# Graphics, Framebuffer, and Compositor (Microkernel Design)

## 1. Scope and Goals

This document defines a minimal, enforceable graphics architecture for WASMOS
that aligns with the existing microkernel model:

- IPC carries control metadata, never bulk pixel payloads.
- Bulk graphics data is shared via kernel-managed borrowed buffers.
- Display policy (windowing, composition, clipping, z-order) is isolated in
  user-space compositor service(s), not in the framebuffer driver.
- The framebuffer driver owns hardware and mode setting.

Primary goal for initial rollout: a deterministic software compositor path that
can present multiple independent app surfaces on a single scanout.

## 2. Component Model

Runtime topology:

- GUI app/toolkit process
- `gfx-compositor` service (window server + software compositor)
- `fb-display` driver/service (native display driver)
- kernel (IPC, scheduling, mapping, capability checks, interrupts)

Control path:

- app -> compositor via IPC (window lifecycle, command submission, present)
- compositor -> framebuffer driver via IPC (get info, mode, present, vblank)

Data path:

- app command buffers and app pixel buffers are shared objects
- compositor render target and scanout buffers are shared objects
- driver maps hardware VRAM/MMIO and optionally exposes scanout buffers as
  borrowable framebuffer-class buffers

## 3. Trust and Isolation Boundaries

- Regular apps never access physical framebuffer mappings directly.
- Only `gfx-compositor` receives framebuffer-class mapping grants.
- App-submitted command buffers are untrusted and must be validated fully.
- Buffer ownership and grant checks use existing borrow semantics.
- Driver capability profile is spawn-time constrained (MMIO/IRQ + optional DMA).

Security invariants:

- Any invalid command batch is rejected fail-closed for that submit.
- Window ownership is checked on every request that references a `window_id`.
- Buffer handles in commands are resolved only within the submitting process
  grant set.

## 4. Display/Framebuffer Driver ABI (Compositor-Facing)

All display IPC messages share a fixed header:

```c
#define FB_IPC_ABI_MAGIC 0x46424950u /* 'FBIP' */
#define FB_IPC_ABI_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t request_id;
    int32_t status; /* reply: 0 success, negative errno-style failure */
} fb_ipc_hdr_t;
```

Opcodes:

```c
enum {
    FB_IPC_GET_INFO = 0x0100,
    FB_IPC_SET_MODE,
    FB_IPC_MAP_SCANOUT,
    FB_IPC_ALLOC_BUFFER,
    FB_IPC_PRESENT,
    FB_IPC_WAIT_VBLANK,
};
```

Core payloads:

```c
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t pixel_format;   /* e.g. XRGB8888 */
    uint32_t refresh_millihz;
    uint32_t flags;          /* capability bits */
} fb_display_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t refresh_millihz; /* 0 = default */
} fb_set_mode_req_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t usage_flags;     /* CPU_WRITE, SCANOUT_CANDIDATE, etc */
} fb_alloc_buffer_req_t;

typedef struct {
    uint32_t buffer_id;       /* compositor-owned buffer id */
    uint32_t damage_count;
    /* clipped to fixed upper bound in message */
} fb_present_req_t;
```

Behavior notes:

- `FB_IPC_MAP_SCANOUT` returns a handle to a framebuffer-class borrowed object.
- `FB_IPC_ALLOC_BUFFER` returns a mapped-capable shared buffer handle/id pair.
- `FB_IPC_WAIT_VBLANK` blocks until the next retrace or timeout.

## 5. Compositor ABI (App-Facing)

All compositor IPC messages use:

```c
#define GFX_IPC_ABI_MAGIC 0x47465850u /* 'GFXP' */
#define GFX_IPC_ABI_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t request_id;
    int32_t status;
} gfx_ipc_hdr_t;
```

App-facing opcodes:

```c
enum {
    GFX_IPC_CREATE_WINDOW = 0x0200,
    GFX_IPC_DESTROY_WINDOW,
    GFX_IPC_RESIZE_WINDOW,
    GFX_IPC_ALLOC_SHARED_BUFFER,
    GFX_IPC_SUBMIT_COMMANDS,
    GFX_IPC_PRESENT_WINDOW,
    GFX_IPC_POLL_EVENT,
};
```

Core request/reply payloads:

```c
typedef uint32_t gfx_window_id_t;
typedef uint32_t gfx_buffer_id_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t flags;
} gfx_create_window_req_t;

typedef struct {
    gfx_window_id_t window_id;
    uint32_t width;
    uint32_t height;
} gfx_create_window_rsp_t;

typedef struct {
    gfx_window_id_t window_id;
    uint32_t command_buffer_handle;
    uint32_t offset;
    uint32_t size;
} gfx_submit_commands_req_t;

typedef struct {
    gfx_window_id_t window_id;
    gfx_buffer_id_t buffer_id;
    uint32_t damage_count;
} gfx_present_window_req_t;
```

Constraints:

- `offset + size` bounds must be validated against the mapped command buffer.
- Every command must satisfy `hdr.size >= sizeof(cmd_hdr)` and remain within
  submit bounds.
- Unknown command IDs are hard reject for the batch.

## 6. Command Buffer ABI (Immediate-Mode v1)

Command stream is packed, little-endian, and pointer-free.

```c
typedef struct {
    uint16_t type;
    uint16_t size;
} gfx_cmd_hdr_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} gfx_rect_t;
```

Command IDs:

```c
enum {
    GFX_CMD_SET_CLIP = 0x0001,
    GFX_CMD_FILL_RECT,
    GFX_CMD_BLIT,
    GFX_CMD_DRAW_TEXT,
};
```

Payloads:

```c
typedef struct {
    gfx_cmd_hdr_t h;
    gfx_rect_t clip;
} gfx_cmd_set_clip_t;

typedef struct {
    gfx_cmd_hdr_t h;
    gfx_rect_t rect;
    uint32_t color_argb;
} gfx_cmd_fill_rect_t;

typedef struct {
    gfx_cmd_hdr_t h;
    gfx_buffer_id_t src_buffer;
    gfx_rect_t src_rect;
    int32_t dst_x;
    int32_t dst_y;
} gfx_cmd_blit_t;

typedef struct {
    gfx_cmd_hdr_t h;
    int32_t x;
    int32_t y;
    uint32_t color_argb;
    uint32_t font_id;
    uint32_t text_offset;
    uint32_t text_len;
} gfx_cmd_draw_text_t;
```

Validation rules:

- coordinates are clipped to target window bounds before rasterization
- integer overflow on region math is reject-path
- text/blob offsets must resolve inside declared shared text data object

## 7. Buffer and Memory Model

Buffer classes:

- app-private shared buffer: owned by app, grantable to compositor
- compositor render target buffer(s): owned by compositor
- framebuffer scanout buffer(s): owned by `fb-display`

Borrow/mapping lifecycle:

1. owner creates or receives buffer handle
2. owner grants mapped/read/write rights to consumer endpoint
3. consumer maps and uses buffer
4. owner may revoke grant; consumer must observe fail-closed on future access

Preferred initial format:

- `XRGB8888` scanout/composition format for deterministic software path

## 8. Scheduling, VBlank, and Present Policy

Frame policy v1:

- compositor maintains one back buffer per active display mode
- app presents only mark window regions dirty
- compositor performs composition on a frame tick or vblank wake
- `fb-display` present is called once per composed frame

Timing goals (initial):

- minimize tearing by vblank-aligned presents when supported
- enforce max frame cadence (e.g. 60 Hz nominal) to avoid starvation of
  non-graphics workloads

## 9. Error Semantics

Status conventions:

- `0` success
- `-1` invalid argument / malformed payload
- `-2` permission/ownership/grant failure
- `-3` unsupported operation or mode
- `-4` busy/retryable
- `-5` internal I/O or device failure

Retry guidance:

- busy or vblank-timeout style errors are retryable by caller
- malformed command batches must not be retried unchanged

## 10. Phased Implementation Plan

### Phase 0: ABI and Scaffolding

Tasks:

- define shared header(s) for `fb_ipc_*`, `gfx_ipc_*`, and `gfx_cmd_*`
- add ABI magic/version checks in message dispatch paths
- add strict parser/validator for command buffers
- add boot/service registration entries for `gfx-compositor` and `fb-display`

Validation:

- unit tests for parser bounds/overflow/unknown-cmd rejection
- boot smoke marker for compositor-driver endpoint handshake

### Phase 1: Single-Mode Software Composition

Tasks:

- implement framebuffer info/mode query and scanout map in `fb-display`
- implement compositor window table and per-window surface tracking
- implement `SET_CLIP`, `FILL_RECT`, `BLIT` commands
- add dirty-rect tracking and full-frame fallback compose

Validation:

- QEMU smoke with two demo windows and deterministic overlap behavior
- negative tests: invalid buffer handle, out-of-range command payload, forged
  window id

### Phase 2: Present and VBlank Control

Tasks:

- implement vblank wait/present IPC on framebuffer driver
- gate present loop on vblank where available
- add compositor frame scheduler with coalesced dirty regions

Validation:

- marker for successful vblank wait/present cycle
- stress test with rapid app submits and stable compositor loop

### Phase 3: Text and Event Plumbing

Tasks:

- implement `DRAW_TEXT` with fixed font atlas path
- add basic input event queue ABI (`GFX_IPC_POLL_EVENT`)
- connect VT input focus ownership with compositor window focus

Validation:

- smoke for text draw correctness bounds and event delivery ownership

### Phase 4: Zero-Copy Candidates and Hardware Planes (Optional)

Tasks:

- add scanout-candidate metadata for eligible app buffers
- add compositor plane assignment policy hooks
- keep software fallback mandatory for unsupported hardware

Validation:

- explicit active/fallback markers for zero-copy path

## 11. Implementation Notes and Deferred Gaps

- TODO(phase-3): define stable font asset packaging and font-id namespace.
- TODO(phase-4): define per-display multi-head ABI (display id routing).
- FIXME(security): once event ABI lands, add explicit sequence-number anti-replay
  checks on compositor input event dequeue protocol.

## 12. Integration Points in Current Tree

Expected initial placement (subject to minimal naming adjustments):

- kernel ABI headers: `lib/libc/include/wasmos/` and matching kernel includes
- compositor service: `src/services/gfx-compositor/`
- framebuffer driver: `src/drivers/framebuffer/` (native path)
- tests: `tests/` with QEMU markers similar to existing runtime smoke gates

The implementation should preserve existing boot and ring3 contracts and avoid
new heavyweight dependencies.
