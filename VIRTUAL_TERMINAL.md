# Virtual Terminal Concept

This document describes the design intent for WASMOS's virtual terminal (VT)
subsystem. The goal is a clean, incrementally extensible architecture: start
with the simplest thing that puts text on the framebuffer, then grow into
ANSI color, escape handling, multi-TTY, and richer editing without rewiring
the fundamentals each time.

Keep this document aligned with `README.md`, `ARCHITECTURE.md`, and `TASKS.md`
as the implementation evolves.

---

## Design Goals

1. **Start minimal.** Phase 1 must be small enough to land cleanly and not
   break the boot flow.
2. **Mechanism vs. policy split.** Framebuffer rendering is a driver concern;
   terminal emulation is a service concern. The current text handoff boundary
   is a shared-memory console ring.
3. **IPC-first where it helps.** VT control paths use IPC; high-volume console
   text uses shared memory to avoid queue pressure.
4. **Prepared for growth.** Cell model, escape parser slot, and TTY abstraction
   are chosen up front so later phases do not require structural rewrites.
5. **Framebuffer-native.** All pixel writes go directly to the mapped physical
   framebuffer. No intermediate blitting surface initially.
6. **Serial stays.** The serial console remains the debug/fallback path. The VT
   does not replace serial; it augments it.

---

## Component Split

Current VT-related runtime path:

```
kernel/services/apps ─▶ wasmos_console_write ─▶ serial_write
                                               │
                                               ├─▶ COM1 serial output
                                               │
                                               └─▶ shared console ring (1 page)
                                                       │
                                                       ▼
                                         framebuffer driver (native)
                                         drains ring + renders text cells

keyboard driver ──▶ vt (WASM service) ──▶ kernel input ring
  (notification)      escape parser
                      VT_IPC_WRITE_REQ handling
```

### `framebuffer` — Extended Native Framebuffer Driver

**Type:** native ELF WASMOS-APP (`FLAG_DRIVER | FLAG_NATIVE`)

The existing framebuffer driver is the single owner of the physical framebuffer.
Extending it with cell/text rendering keeps hardware ownership consolidated in
one place — a second native driver mapping the same physical memory would be
wasteful and confusing.

The driver already exposes raw pixel primitives (`framebuffer_info`,
`framebuffer_map`, `framebuffer_pixel`). Current text rendering is fed from a
kernel-owned shared console ring that the driver maps through the native driver
API (`console_ring_id` + `shmem_map`). Control operations (cell write/cursor/
scroll/clear) remain IPC-based.

### `vt` — Terminal Emulator Service

**Type:** WASM WASMOS-APP (service)

`vt` currently implements terminal-policy pieces that are already landed:
- strips ANSI/VT100 escape sequences from `VT_IPC_WRITE_REQ` payload
- routes keyboard notifications into the kernel input ring
- forwards printable output through `wasmos_console_write` (which feeds serial
  and the shared console ring)

`vt` now owns a framebuffer control endpoint and can switch between system-
console mode (`tty0`) and VT-managed virtual terminals (`tty1+`).

Splitting terminal logic into a WASM service means it can be updated, replaced,
or crashed without touching the framebuffer driver, and it cannot directly
corrupt display hardware.

### Implementation Status (March 2026)

- Landed:
  - native framebuffer text renderer
  - early-log replay in framebuffer driver
  - kernel shared-memory console ring (serial writer, framebuffer reader)
  - native driver shared-memory API (`shmem_create/map/unmap`)
  - WASM shared-memory syscalls (`wasmos_shmem_create/map/unmap`)
  - `vt` keyboard subscribe + input routing + escape stripping
  - `vt` core CSI/SGR decode subset:
    cursor move (`A/B/C/D/H/f`), erase (`J/K`), and 16-color SGR (`m`)
  - keyboard `KBD_KEY_NOTIFY` path now sends as strict fire-and-forget
    (`request_id = 0`)
  - VT→framebuffer and CLI→VT output paths now cap queue-full retries and drop
    stale updates on persistent backpressure instead of spinning forever
- Not landed yet:
  - cooked/raw line discipline history
  - full ANSI cursor/color feature set
  - service-registry-based VT endpoint discovery
  - deferred debug pass for an intermittent framebuffer-only prompt
    duplication/misalignment artifact seen during rapid `Ctrl+Shift+Fn`
    switching (currently not reproducible in recent runs)

---

## Architecture Overview

```
                        ┌─────────────────────────────────────────┐
                        │             vt  (WASM service)          │
  keyboard driver ─────▶│  escape parser · TTY mux · line disc.   │
  (scancode IPC)        │  VT_WRITE / VT_READ  (public endpoint)  │
                        └─────────────────┬───────────────────────┘
                                          │  FBTEXT_CELL_WRITE (IPC)
                        ┌─────────────────▼───────────────────────┐
                        │      framebuffer driver  (native)       │
                        │  cell grid · 8×16 font · dirty repaint  │
                        │  framebuffer_map → pixel writes         │
                        └─────────────────────────────────────────┘

  kernel / services ───▶ vt  (VT_WRITE_REQ)
  (console_write route)
```

---

## Framebuffer Driver Extensions

### Font

An 8×16 bitmap font stored as a static C array covering the printable ASCII
range (0x20–0x7E) plus a handful of box-drawing characters. No runtime font
loading; the font is compiled directly into the driver binary. A PSF2-compatible
layout is acceptable as a future upgrade path, but Phase 1 uses a raw array.

**Font choice: Terminus** (SIL Open Font License 1.1). Terminus is a clean,
widely used bitmap font with an 8×16 variant, good ASCII and Latin Extended
coverage, and a licence that imposes no restrictions on embedding in an OS
image. The Phase 1 glyph array is extracted from the Terminus PSF2 source at
build time by a small helper script; the PSF2 file itself is vendored under
`third_party/terminus/` with its licence intact.

For Phase 3 Unicode expansion the same Terminus source covers Latin Extended,
box-drawing, and block elements. A different font or supplementary table can
be added for any ranges Terminus does not cover.

Font storage: `src/drivers/framebuffer/font_8x16.h` (generated)

### Cell Grid

The display is divided into a grid of fixed-size cells:

```
cols = framebuffer_width  / FONT_W   (e.g. 1024 / 8  = 128)
rows = framebuffer_height / FONT_H   (e.g. 768  / 16 = 48)
```

Each cell holds:

```c
typedef struct {
    uint32_t ch;    /* Unicode codepoint; Phase 1: ASCII only      */
    uint8_t  fg;    /* 4-bit palette index                         */
    uint8_t  bg;    /* 4-bit palette index                         */
    uint8_t  attr;  /* reserved for bold/underline/blink flags     */
    uint8_t  _pad;
} fbtext_cell_t;
```

The cell buffer is a flat `fbtext_cell_t cells[rows * cols]` array in the
driver's heap. Rendering is cell-dirty-flag driven: only changed cells are
repainted.

### Cursor

The cursor position is tracked by the framebuffer driver as (col, row). Phase 1
renders a solid block by XOR-inverting the cell's fg/bg on every write. No
blinking yet.

### Scroll

When the cursor advances past the last row, the cell buffer is shifted up one
row (`memmove`), the bottom row is cleared, and the full grid is repainted.
Phase 2 replaces this with a ring-buffer approach that repaints only dirty
rows.

### Palette

A fixed 16-color CGA-style palette is embedded in `fb-text`. Phase 1 uses only
index 0 (black background) and index 15 (white foreground).

### Framebuffer Driver Control IPC Interface

The framebuffer driver exposes a minimal internal IPC endpoint for use by `vt`.
Clients outside the VT subsystem should not send to this endpoint directly.

| Constant                  | Value  | Direction        | Meaning                                        |
|---------------------------|--------|------------------|------------------------------------------------|
| `FBTEXT_IPC_CELL_WRITE_REQ` | 0x600 | vt → framebuffer | Write one cell: col, row, codepoint, fg, bg  |
| `FBTEXT_IPC_CURSOR_SET_REQ` | 0x601 | vt → framebuffer | Move cursor to (col, row)                     |
| `FBTEXT_IPC_SCROLL_REQ`     | 0x602 | vt → framebuffer | Scroll up N rows, clear bottom N rows         |
| `FBTEXT_IPC_CLEAR_REQ`      | 0x603 | vt → framebuffer | Clear region or full screen                   |
| `FBTEXT_IPC_RESP`           | 0x680 | framebuffer → vt | Acknowledgment                                |
| `FBTEXT_IPC_ERROR`          | 0x6FF | framebuffer → vt | Error, `arg0` = error code                    |

`FBTEXT_IPC_CELL_WRITE_REQ` packs col in `arg0`, row in `arg1`, codepoint in
`arg2`, and `(fg << 8) | bg` in `arg3`.

---

## Keyboard Input Routing

Key press events flow from the keyboard driver to `vt` via the kernel
notification mechanism, not through request/response IPC. The model is:

1. When `vt` starts it registers interest in key-press notifications with the
   keyboard driver (a `KBD_SUBSCRIBE_REQ` IPC call during initialization).
2. The keyboard driver records `vt`'s notification endpoint.
3. On each scancode the keyboard driver delivers a `KBD_KEY_NOTIFY` notification
   to every registered subscriber. The notification carries the raw scancode and
   a decoded codepoint (or zero if not printable).
4. `vt` receives the notification in its main loop, feeds the codepoint into the
   active TTY's input queue (or the escape parser if in raw mode), and echoes
   back to the framebuffer driver as appropriate.

This is a pub/sub model: the keyboard driver does not know or care what `vt`
does with the event. Multiple subscribers (e.g. a future compositor or debugger)
can coexist without changes to the keyboard driver.

The keyboard driver's existing `serial_write` echo path is removed once `vt` is
active and takes over echo responsibility under its line discipline.

### Notification Message

```
KBD_SUBSCRIBE_REQ    0x800  vt → keyboard driver   register notification endpoint
KBD_SUBSCRIBE_RESP   0x880  keyboard → vt          acknowledgment
KBD_KEY_NOTIFY       0x801  keyboard → subscriber  scancode in arg0, codepoint in arg1
```

`KBD_KEY_NOTIFY` is fire-and-forget (no response expected) to keep the keyboard
driver's hot path free of blocking IPC waits.

---

## `vt` Service

### TTY State

The current baseline has four TTY slots (`tty0..tty3`):
- `tty0` is the system console mirror (serial + console ring)
- `tty1..tty3` are VT-managed virtual terminals

The per-TTY state in `vt` is:

```c
typedef struct {
    uint16_t    rows, cols;
    uint16_t    cursor_row, cursor_col;
    uint8_t     fg, bg, attr;       /* current SGR attribute           */
    uint32_t    scrollback_top;     /* Phase 3: ring buffer head       */
    /* Phase 3: scrollback ring  */
    /* Phase 3: input queue      */
} vt_tty_t;
```

Switching TTYs (Phase 2) saves the active `vt_tty_t`, sends `FBTEXT_CLEAR_REQ`
to blank the screen, then replays the new TTY's cell buffer via
`FBTEXT_CELL_WRITE_REQ`. Alt+F1–F4 are the intended key combos.

### Escape Sequence Parser (Phase 2)

A small state machine sits between the IPC write handler and the cell emitter:

```
NORMAL → ESC_SEEN → CSI_SEEN → PARAM_COLLECT → DISPATCH
              └──────────────────────────────────────────┘
                        (on any non-CSI sequence)
```

Priority target sequences for Phase 2:

| Sequence          | Meaning                            |
|-------------------|------------------------------------|
| `ESC[A/B/C/D`     | Cursor movement                    |
| `ESC[H` / `ESC[f` | Cursor position                    |
| `ESC[J`           | Erase in display                   |
| `ESC[K`           | Erase in line                      |
| `ESC[m`           | SGR: color and attribute selection |
| `ESC[s` / `ESC[u` | Save / restore cursor              |
| `ESC[?25h/l`      | Show / hide cursor                 |

Unrecognized sequences are silently consumed (no corruption of cell state).

The parser lives in `src/services/vt/escape.c` and is exercised by unit tests
with no framebuffer or IPC dependency.

### `vt` Public IPC Interface

`vt` exposes the public endpoint that kernel, services, and the CLI write to.

| Constant              | Value  | Direction      | Meaning                                      |
|-----------------------|--------|----------------|----------------------------------------------|
| `VT_WRITE_REQ`        | 0x700  | client → vt    | Write bytes to active TTY                    |
| `VT_WRITE_RESP`       | 0x780  | vt → client    | Acknowledgment, `arg0` = bytes written       |
| `VT_READ_REQ`         | 0x701  | client → vt    | Request next input byte                      |
| `VT_READ_RESP`        | 0x781  | vt → client    | `arg0` = byte, `arg1` = 0 ok / -1 empty      |
| `VT_SET_ATTR_REQ`     | 0x702  | client → vt    | Set fg/bg/attr for subsequent writes         |
| `VT_SET_ATTR_RESP`    | 0x782  | vt → client    | Acknowledgment                               |
| `VT_SWITCH_TTY_REQ`   | 0x703  | client → vt    | Switch active TTY (`arg0` = tty index)       |
| `VT_SWITCH_TTY_RESP`  | 0x783  | vt → client    | Acknowledgment                               |
| `VT_ERROR_RESP`       | 0x7FF  | vt → client    | Error, `arg0` = error code                   |

`VT_WRITE_REQ` carries up to 4 bytes inline in `arg0`–`arg3` (Phase 1). For
longer writes the client loops. A shared-memory bulk path is a Phase 3 item.

### Registration

Once the framebuffer driver signals readiness, `vt` resolves its endpoint and
begins forwarding. `vt` then registers itself (e.g. `ipc_register_service("vt0")`)
so clients can resolve its endpoint by name. Until a service registry exists,
Phase 1 uses a well-known fixed endpoint constant.

---

## Early Kernel Log Buffer

Between ExitBootServices and the moment `vt` is ready, all output goes to the
serial console. Without an early log buffer, that output is invisible on the
framebuffer — the VT would only show text produced after it started.

The early kernel log buffer is a fixed-size circular buffer in kernel BSS that
captures every byte written through `serial_write` (and therefore through
`wasmos_console_write`) from the very first kernel message onward. It acts as
a passive tap: serial output is unaffected, the buffer just accumulates a copy.

### Buffer Design

```c
#define EARLY_LOG_SIZE 8192   /* tunable; fits comfortably in BSS */

typedef struct {
    uint8_t  data[EARLY_LOG_SIZE];
    uint32_t head;   /* next write position (wraps)           */
    uint32_t len;    /* bytes written, capped at EARLY_LOG_SIZE */
} early_log_t;
```

A simple ring: when full, oldest bytes are overwritten. 8 KiB is enough for a
typical boot log; it can be tuned up if verbose drivers produce more output.

Writing is done inside `serial_write` with interrupts already gated by the
existing spinlock, so no additional locking is needed.

### User-Space Access

`vt` reads the buffer through two kernel WASM imports, following the same
pattern as `wasmos_boot_config_copy`:

```c
uint32_t wasmos_early_log_size(void);
int32_t  wasmos_early_log_copy(char *ptr, uint32_t len, uint32_t offset);
```

These expose the linearized contents of the ring (oldest byte first) as a
flat read-only view. `vt` calls these once during startup, replays the bytes
through its normal character processing path, and then begins consuming live
output via `VT_WRITE_REQ`.

### Handoff Sequence (Current)

1. kernel writes to serial + early_log ring buffer throughout boot
2. framebuffer driver initializes (maps FB, builds cell grid)
3. framebuffer driver starts, maps the console ring, replays early-log ring
4. framebuffer driver drains shared ring continuously in its main loop
5. vt starts independently for input/escape handling; output still flows
   through `wasmos_console_write` → `serial_write`

The buffer is never freed; it remains readable after handoff for debuggers or
a future `dmesg`-style command.

### Native Driver Access

Native drivers (including the framebuffer driver itself) access the buffer
through a new entry in `wasmos_driver_api_t`:

```c
uint32_t (*early_log_size)(void);
void     (*early_log_copy)(uint8_t *dst, uint32_t offset, uint32_t len);
int      (*shmem_create)(uint64_t pages, uint32_t flags, uint32_t *out_id, void **out_ptr);
void    *(*shmem_map)(uint32_t id);
int      (*shmem_unmap)(uint32_t id);
uint32_t (*console_ring_id)(void);
```

---

## Console Path Integration (Current)

`wasmos_console_write` routes output to `serial_write`, and `serial_write` now:
- writes to COM1 for debug/fallback
- appends bytes into the shared console ring

This removes the previous serial→framebuffer text IPC path while preserving a
serial-first debug console.

---

## Unicode / Non-ASCII (Phase 3)

Phase 1 is ASCII-only. Phase 3 introduces UTF-8 decoding in `vt` and a wider
glyph table in `framebuffer`. The `fbtext_cell_t.ch` field is already `uint32_t`
for this reason. Wide characters (CJK, emoji) occupy two adjacent cells; the
right cell carries a sentinel (`ch = 0xFFFF`) so erase and cursor logic handle
them correctly.

Font coverage grows incrementally: Latin Extended, box-drawing, block elements,
and common symbols first; full CJK is a much later item and may require a
compressed glyph store in the framebuffer driver.

---

## Line Editing (Phase 3)

Phase 3 adds a line discipline layer inside `vt`:

- Input queue per TTY (ring buffer of codepoints from the keyboard driver).
- A cooked-mode line buffer with backspace, Ctrl+U (kill line), and Ctrl+C
  (interrupt) at minimum.
- History ring buffer (up/down arrow) stored per TTY.
- Raw mode flag per TTY for programs that want uncooked input (future shells,
  editors).

The line discipline is accessible through the same `VT_READ_REQ` IPC path; the
mode flag is set via a new `VT_SET_MODE_REQ` message.

---

## Scrollback (Phase 3)

A per-TTY ring buffer in `vt` holds N full rows of cells beyond what is
currently visible. Page-Up / Page-Down shift the viewport and trigger a full
`FBTEXT_CELL_WRITE_REQ` replay of the visible window. Ring size is fixed at
compile time (e.g. 500 rows) to avoid dynamic allocation complexity.

---

## Phase Summary (Updated)

| Phase | Deliverables |
|-------|-------------|
| **1 (done)** | Native framebuffer text rendering, early-log replay, shared console ring drain, serial mirror retained. |
| **2 (in progress)** | VT escape parsing, keyboard routing, multi-TTY switching, and per-tty CLI homes landed; remaining: richer ANSI handling and line discipline. |
| **3 (planned)** | UTF-8 expansion, scrollback, richer VT client API, and tighter shared-memory conventions for bulk text/data paths. |

---

## File Layout (planned)

```
third_party/terminus/       ← vendored Terminus PSF2 source + OFL licence

src/drivers/framebuffer/    ← extended native driver
  CMakeLists.txt
  framebuffer_native.c      existing entry point, driver lifecycle
  render.c                  cell-to-pixel blitting, scroll, dirty tracking (new)
  font_8x16.h               generated glyph array (extracted from Terminus PSF2)
  include/
    fbtext_ipc.h            FBTEXT_* message constants (used by vt service, new)
    fbtext_internal.h       fbtext_cell_t, cursor, palette internals (new)

src/services/vt/            ← WASM service (wired up in Phase 2)
  CMakeLists.txt
  vt_main.c                 entry point, IPC loop, framebuffer driver client
  tty.c                     vt_tty_t state, attribute tracking
  escape.c                  escape sequence parser (Phase 2)
  include/
    vt_ipc.h                VT_* message constants (public API for clients)
    vt_internal.h           vt_tty_t, cursor, escape parser state
```

`vt_ipc.h` is the only header that external clients (kernel, services, libc
shims) should include. `fbtext_ipc.h` is an internal contract between the
framebuffer driver and `vt` only.

---

## Open Questions

- **Rendering path.** Phase 1 may write pixels one at a time via
  `api->framebuffer_pixel`. Phase 2 should switch to bulk row writes through
  the mapped framebuffer pointer directly (as the gradient demo does). Measure
  first.
- **Service registry.** `ipc_register_service("vt0")` assumes a name→endpoint
  resolution mechanism that does not exist yet. Phase 1 uses a well-known fixed
  endpoint constant until the service registry lands.
- **Framebuffer cell endpoint visibility.** The `FBTEXT_*` endpoint on the
  framebuffer driver should not be discoverable by arbitrary services. Until
  capability-based endpoint access exists, document the convention that only
  `vt` sends to it. 
