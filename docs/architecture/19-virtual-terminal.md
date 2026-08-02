## Virtual Terminal Service — I/O Multiplexer

> **Documentation status: Mixed reference and proposal.** The escape parser,
> line discipline, cell/TTY state, framebuffer cell IPC, and TTY switching are
> implemented reference. The multiplexer model (slot roles, two selectors,
> keyboard-through-VT single decoder, serial-over-IRQ into the VT, push-based
> input notifications, and klog routed to vt-1) is a redesign landing in phases;
> each redesign section is tagged **Shipped** or **Proposed (phase N)**, and the
> Rollout section tracks status.

This document describes the `vt` WASM service as the system's single
input/output multiplexer: the framebuffer driver's text-cell IPC, keyboard and
serial input routing, multi-slot switching, escape-sequence parsing, and the
line discipline. Implementation lives in `src/services/vt/vt_main.c`,
`src/services/vt/vt_types.h`, the framebuffer drivers
(`src/drivers/framebuffer_pci/framebuffer_pci_native.c`,
`src/drivers/framebuffer/framebuffer_native.c`), the serial driver
(`src/drivers/serial/serial.ts`), the CLI (`src/services/cli/cli.c`), and the
compositor (`src/services/gfx_compositor/gfx_compositor.zig`). See also
`17-console-io-and-character-device.md` (console hostcalls),
`20-graphics-framebuffer-and-compositor.md` (compositor),
`21-virtual-input-testing-via-virtio-serial.md` (input drivers), and
`09-process-and-ipc.md` (select-set / blocking primitives).

---

### Role: the VT is the sole I/O multiplexer

Every text console's input and output is routed through a VT **slot**. A slot is
rendered to the framebuffer only when it is the *visible* slot, and mirrored to
serial only when it is the *serial-bound* slot. Keyboard and serial input both
flow into the VT, which routes them to the active slot's reader. This makes the
VT the single point that owns keymap/modifier state, framebuffer arbitration,
and the serial console binding.

#### Slot model (Proposed — phase 5)

| Slot     | Role                                                                   |
|----------|------------------------------------------------------------------------|
| `vt-0`   | GUI — owned by the compositor. No CLI. Not a text slot.                |
| `vt-1`   | System console. Default *visible* and default *serial-bound* slot.     |
| `vt-2..N`| Additional text consoles, each owning its own CLI (spawned lazily).    |

`VT_MAX_TTYS` is 4 today (vt-0..vt-3).

**Shipped model (being superseded):** today `tty0` is the serial-mirrored,
read-only system console (`vt_put_char_tty0` → `wasmos_console_write`) which the
compositor overlays via the framebuffer overlay lock; `tty1..tty3` are text VTs.
Phase 5 moves the serial-mirrored console to **vt-1** and makes **vt-0** a pure
GUI slot, so the framebuffer is exclusively either the compositor (vt-0) or the
VT's text render of one vt-x.

#### Two independent selectors (Proposed — phase 5)

| Selector           | Default | Set by                                            |
|--------------------|---------|---------------------------------------------------|
| *visible* slot     | `vt-1`  | keyboard hotkeys / `tty N` issued on the keyboard  |
| *serial-bound* slot| `vt-1`  | `VT_IPC_BIND_SERIAL_REQ` / `tty N` issued on serial |

The compositor requests the visible slot switch to `vt-0` **once**, when the
first UI app appears (`try_switch_to_gfx_tty` → `VT_IPC_SWITCH_TTY 0`), and then
never auto-switches again: switching is user-driven from there (`Ctrl+Shift+Fn`
or `tty N`). `tty N` retargets whichever channel issued it: issued on the
keyboard it changes the visible slot; issued on serial it rebinds the
serial-bound slot. `tty 0` on serial is rejected (the GUI cannot render over a
serial line). vt-1 mirrors to serial even while vt-0 is visible.

**Framebuffer ownership (phase 5, Shipped).** The compositor owns the framebuffer
only while `vt-0` is the visible slot: it draws to it exclusively when visible
and relinquishes it (draws nothing) when a text slot is shown, so it never fights
the vt's text render. The vt is the authority — on every switch it sends
`VT_IPC_VIS_NOTIFY` (arg0 = whether `vt-0` is now visible) to the compositor,
which resumes drawing (with a full repaint) or stops accordingly. The "hidden"
notify is sent *before* the vt repaints the text slot, so the compositor stops
first. The compositor claims the notify/key-forward endpoint on its first switch
to `vt-0`; a later `tty 0` that only changes the visible slot does not reclaim it.

#### Per-slot output fan-out (Proposed — phase 4/5)

Each slot's output goes to: its own cell/scrollback buffer **always**; the
framebuffer **iff** it is the visible slot; serial **iff** it is the
serial-bound slot.

---

### Component topology

```
keyboard driver ─KBD_IPC_KEY_NOTIFY─▶ vt ◀─VT_IPC_SERIAL_INPUT_REQ─ serial driver (IRQ4)
                                       │  single scancode decoder
                                       │  slot mux · escape parser · line discipline
        ┌──────────────┬───────────────┼───────────────┐
        │ VT_IPC_KEY_  │ VT_IPC_INPUT_ │ FBTEXT_IPC_*   │ VT_IPC_WRITE_REQ (in)
        │ FORWARD      │ NOTIFY (push) │ (cell/cursor)  │
        ▼              ▼               ▼                └── kernel / CLI / services
   gfx-compositor    CLI (awaits)   framebuffer driver
   (vt-0 keys)       drains slot    (blit surface)
                                       ▲
   kernel console ring ────────────────┘  (klog → vt-1, phase 4)
```

`vt` is the sole owner of the framebuffer driver's text-cell endpoint. Clients
write bytes with `VT_IPC_WRITE_REQ`; `vt` parses escapes, updates per-slot cell
state, and forwards cell/cursor/scroll operations to the framebuffer driver only
for the visible slot.

---

### VT Service Manifest

Source: `src/services/vt/linker.metadata`

| Field                    | Value               |
|--------------------------|---------------------|
| `kind`                   | `service`           |
| `entry`                  | `initialize`        |
| `native`                 | `false` (WASM)      |
| `stack_pages`            | 16 (64 KB)          |
| `heap_pages`             | 16 (64 KB)          |
| `required_endpoint_name` | `proc`              |
| `entry_arg_bindings`     | `["proc.endpoint"]` |
| Capability               | `ipc.basic`         |

---

### TTY State

Source: `src/services/vt/vt_types.h`

```c
typedef enum { ESC_NORMAL = 0, ESC_ESC, ESC_CSI } esc_state_t;

typedef struct {
    uint32_t ch;   /* Unicode codepoint (ASCII-only in practice) */
    uint8_t  fg;   /* 4-bit palette index                        */
    uint8_t  bg;   /* 4-bit palette index                        */
    uint8_t  attr; /* bold and other attribute flags             */
    uint8_t  _pad;
} vt_cell_t;

typedef struct {
    uint16_t    cursor_row, cursor_col;
    uint16_t    cursor_saved_row, cursor_saved_col;
    uint8_t     fg, bg, attr;
    uint8_t     cursor_visible, cursor_saved_valid;
    uint8_t     input_echo, input_canonical;
    esc_state_t esc;
    uint16_t    input_q_head, input_q_tail;
    uint16_t    input_line_len, input_line_cursor;
    uint8_t     input_history_count, input_history_head;
    int8_t      input_history_nav;
    uint8_t     input_q[256];
    uint8_t     input_line[128];
    uint8_t     input_history[8][128];
    uint8_t     input_history_len[8];
    uint16_t    csi_params[8];
    uint8_t     csi_count;
    uint16_t    csi_current;
    uint8_t     csi_have_current, csi_private;
    vt_cell_t  *cells;  /* heap-allocated grid; rows × cols entries */
} vt_tty_t;
```

#### Constants

| Constant                    | Value | Meaning                                    |
|-----------------------------|-------|--------------------------------------------|
| `VT_MAX_TTYS`               | 4     | Total slot count (vt-0..vt-3)              |
| `VT_COLS_DEFAULT`           | 80    | Default columns (used when FB unavailable) |
| `VT_ROWS_DEFAULT`           | 25    | Default rows                               |
| `VT_MAX_COLS`               | 160   | Maximum columns after geometry clamp       |
| `VT_MAX_ROWS`               | 64    | Maximum rows after geometry clamp          |
| `VT_FB_SEND_RETRIES`        | 1024  | IPC retry limit for normal cell writes     |
| `VT_FB_SWITCH_CTRL_RETRIES` | 8192  | IPC retry limit for switch control ops     |
| `VT_FB_SWITCH_CELL_RETRIES` | 4096  | IPC retry limit for switch cell replay     |
| `VT_IPC_REPLY_RETRIES`      | 1024  | IPC retry limit for client replies         |
| `VT_GEOMETRY_QUERY_RETRIES` | 2048  | Retries for geometry query to FB           |

#### Global State

| Variable              | Init | Meaning                                           |
|-----------------------|------|---------------------------------------------------|
| `g_vt_ep`             | -1   | VT's own IPC endpoint                             |
| `g_fb_ep`             | -1   | Framebuffer driver endpoint                       |
| `g_kbd_ep`            | -1   | Keyboard driver endpoint                          |
| `g_active_tty`        | 0→1  | Visible slot index (default becomes 1 in phase 5) |
| `g_serial_tty`        | 1    | Serial-bound slot index (proposed, phase 5)       |
| `g_tty_writer_ep[4]`  | -1   | Registered writer endpoint per slot               |
| `g_tty_reader_ep[4]`  | -1   | Registered reader endpoint per slot (push target) |
| `g_switch_generation` | 1    | Monotonic counter, incremented on each switch     |
| `g_switch_barrier`    | 0    | Set to 1 during an in-progress switch             |
| `g_ctrl_down`         | 0    | Ctrl modifier state                               |
| `g_shift_down`        | 0    | Shift modifier state                              |
| `g_altgr_down`        | 0    | AltGr modifier state (proposed, phase 3)          |
| `g_vt_cols`           | 80   | Active column count (updated from FB geometry)    |
| `g_vt_rows`           | 25   | Active row count (updated from FB geometry)       |

---

### Framebuffer Driver IPC Interface

`vt` is the only caller of the framebuffer driver's text-cell endpoint. Under
the redesign the framebuffer driver becomes a pure blit surface: it renders the
cells `vt` sends and, in phase 5, will no longer drain the kernel console ring
itself. Phase 4 routes klog into vt-1 through a separate VT-owned ring
(additively); the framebuffer's console-ring drain is retired in phase 5.

| Opcode                          | Value | Arguments                                            |
|---------------------------------|-------|------------------------------------------------------|
| `FBTEXT_IPC_CELL_WRITE_REQ`     | 0x600 | arg0=col, arg1=row, arg2=codepoint, arg3=(fg<<8\|bg) |
| `FBTEXT_IPC_CURSOR_SET_REQ`     | 0x601 | arg0=col, arg1=row                                   |
| `FBTEXT_IPC_SCROLL_REQ`         | 0x602 | arg0=n (rows to scroll up)                           |
| `FBTEXT_IPC_CLEAR_REQ`          | 0x603 | (no args; clears full screen)                        |
| `FBTEXT_IPC_CONSOLE_MODE_REQ`   | 0x604 | arg0=0 (disable ring drain) / 1 (enable)             |
| `FBTEXT_IPC_GEOMETRY_REQ`       | 0x605 | Request; response: arg0=cols, arg1=rows              |
| `FBTEXT_IPC_GFX_OVERLAY_REQ`    | 0x606 | arg0=0 (unlock) / 1 (lock)                           |
| `FBTEXT_IPC_QUERY_CAPS_REQ`     | 0x607 | Request; response: arg0=FBTEXT_CAP_* bitmask         |
| `FBTEXT_IPC_QUERY_MODES_REQ`    | 0x608 | arg0=index; response: arg0=w, arg1=h, arg2=stride    |
| `FBTEXT_IPC_SET_RESOLUTION_REQ` | 0x609 | arg0=width, arg1=height                              |
| `FBTEXT_IPC_BLIT_ATTACH_REQ`    | 0x60A | arg0=buffer_id, arg1=borrow_id, arg2=cols, arg3=rows |
| `FBTEXT_IPC_BLIT_GRID_REQ`      | 0x60B | arg0=cols, arg1=rows (repaint the attached grid)     |
| `FBTEXT_IPC_RESP`               | 0x680 | Acknowledgment from framebuffer                      |
| `FBTEXT_IPC_ERROR`              | 0x6FF | Error from framebuffer                               |

**Bulk grid blit (phase 5).** `vt` shares a cell-grid xfer-buffer
(`fbtext_blit_cell_t[]`, layout-identical to the driver's `fbtext_cell_t` and
`vt`'s `vt_cell_t`) with the framebuffer driver: `BLIT_ATTACH` (once, at init)
grants the driver READ access to the buffer to map; `BLIT_GRID` (per repaint)
tells the driver to render `cols*rows` cells from it in one shot. This replaces
the per-cell `CELL_WRITE` loop on the full-screen replay path — that loop issued
one IPC per cell (thousands per switch), which overflowed the driver's IPC queue
and, under SMP, starved the driver so a tty switch spun `vt_fb_send_switch`'s
retry for ~80 s. `CELL_WRITE` remains for incremental single-cell updates. A per-
cell path is kept as a fallback when the blit buffer cannot be allocated.

Cell color packing: `arg3 = (fg & 0x0F) << 8 | (bg & 0x0F)`.

`FBTEXT_IPC_CONSOLE_MODE_REQ` toggles whether the framebuffer driver drains the
kernel console ring. **Shipped:** the framebuffer driver owns the console-ring
drain and `vt` toggles it around switches; **phase 4** adds a *separate*
VT-owned klog ring drained into vt-1 without disturbing this. **Proposed
(phase 5):** the framebuffer driver stops draining the console ring entirely,
becoming a blit surface only; `FBTEXT_IPC_GFX_OVERLAY_REQ` still gates whether
the compositor or the VT owns the framebuffer.

---

### VT Public IPC Interface

| Opcode                    | Value  | Caller → vt / vt → peer                                |
|---------------------------|--------|--------------------------------------------------------|
| `VT_IPC_WRITE_REQ`        | 0x700  | Write up to 4 bytes: arg0–arg3, each zero-terminated   |
| `VT_IPC_READ_REQ`         | 0x701  | Drain next input byte; arg0=slot index                 |
| `VT_IPC_SET_ATTR_REQ`     | 0x702  | Set fg/bg/attr; arg0=fg, arg1=bg, arg2=attr            |
| `VT_IPC_SWITCH_TTY`       | 0x703  | Switch visible slot; arg0=slot index                   |
| `VT_IPC_GET_ACTIVE_TTY`   | 0x704  | Query; reply: arg0=switch_generation, arg1=active_tty  |
| `VT_IPC_REGISTER_WRITER`  | 0x705  | Register caller as writer; arg0=slot index             |
| `VT_IPC_SET_MODE_REQ`     | 0x706  | Set input mode bits; arg0=mode flags                   |
| `VT_IPC_SERIAL_INPUT_REQ` | 0x707  | *(phase 1)* serial driver → vt: RX bytes for bound slot |
| `VT_IPC_BIND_SERIAL_REQ`  | 0x708  | *(phase 5)* bind serial to slot; arg0=slot (≥1)        |
| `VT_IPC_RESP`             | 0x780  | Success response                                       |
| `VT_IPC_INPUT_NOTIFY`     | 0x781  | *(phase 2)* vt → reader: input available on your slot  |
| `VT_IPC_KEY_FORWARD`      | 0x782  | *(phase 3)* vt → compositor: key event for vt-0        |
| `VT_IPC_VIS_NOTIFY`       | 0x783  | *(phase 5)* vt → compositor: arg0=1 if vt-0 is now visible, else 0 |
| `VT_IPC_ERROR`            | 0x7FF  | Error response                                          |

#### VT_IPC_WRITE_REQ

Bytes are packed one per arg word (arg0..arg3), high nibble of arg0 optionally
carrying a byte count; processing stops at the first zero byte. Kernel-originated
writes (source < 0) target `g_active_tty` unconditionally and bypass ownership
and generation checks. Client writes from registered writers are validated
against `g_switch_generation`; writes with a stale generation are dropped
silently.

#### VT_IPC_READ_REQ Response

- `VT_IPC_RESP`, arg0=0, arg1=byte: byte available
- `VT_IPC_RESP`, arg0=1, arg1=0: queue empty

The first client to read from a slot claims its reader slot
(`g_tty_reader_ep[slot]`). Under the redesign readers are notified via
`VT_IPC_INPUT_NOTIFY` when input arrives and drain with `VT_IPC_READ_REQ`
(no polling).

#### VT_IPC_SERIAL_INPUT_REQ *(Proposed — phase 1)*

The serial driver forwards COM1 RX bytes to `vt`, packed like
`VT_IPC_WRITE_REQ`. `vt` feeds them into the **serial-bound slot** (`g_serial_tty`,
default 1) through the same line discipline as keyboard input.

#### VT_IPC_INPUT_NOTIFY *(Proposed — phase 2)*

Fire-and-forget push from `vt` to `g_tty_reader_ep[slot]` when a byte is enqueued
for that slot. The reader (a CLI) blocks on its VT endpoint and drains via
`VT_IPC_READ_REQ` on receipt. Replaces the CLI's poll of `VT_IPC_READ_REQ`.

#### VT_IPC_KEY_FORWARD *(Proposed — phase 3)*

When `vt-0` is the active slot, `vt` forwards a decoded key event (keysym +
modifier bitmask + up/down + extended) to the compositor instead of injecting
bytes into a text slot. The compositor no longer subscribes to the keyboard
driver directly.

#### Input Mode Flags

| Constant                  | Value | Meaning                                 |
|---------------------------|-------|-----------------------------------------|
| `VT_INPUT_MODE_RAW`       | 0     | Raw; bytes pass directly to input queue |
| `VT_INPUT_MODE_CANONICAL` | 1<<0  | Line-buffered; commit on Enter          |
| `VT_INPUT_MODE_ECHO`      | 1<<1  | Echo input back to the slot cell grid   |

---

### Input Routing

#### Keyboard → VT → active slot *(single decoder; phase 3)*

The VT is the sole scancode decoder. `KBD_IPC_KEY_NOTIFY` (fire-and-forget) is
decoded once into a canonical key event using a 3-layer keymap
(`plain`/`shift`/`altgr`) plus modifier state (Ctrl/Shift/AltGr), up/down, and
the extended flag. The key event is then projected per active slot:

- **vt-0:** forwarded to the compositor as `VT_IPC_KEY_FORWARD`.
- **vt-x (text):** run through the line discipline into bytes / escape sequences,
  then the reader is push-notified.

**Shipped today:** both the VT and the compositor subscribe to the keyboard
driver and each decode scancodes independently (two keymaps that can drift). The
CLI voluntarily stops pulling VT input when it is background (`cli_is_foreground`)
to avoid double-consume. Phase 3 removes the compositor's subscription/keymap and
the `cli_is_foreground` read gate.

#### Serial → VT → serial-bound slot *(phase 1/2)*

The serial driver (`serial.ts`) enables the COM1 RX interrupt, routes IRQ4 to its
endpoint via `irq_register(ctx, 4, ep)` (the model `keyboard.ts` uses for IRQ1),
blocks on `ipc_recv`, and on each `IPC_IRQ_EVENT_TYPE (0xFF00)` reads the ready RX
bytes, forwards them to `vt` as `VT_IPC_SERIAL_INPUT_REQ`, and calls `irq_ack(4)`.
The kernel keeps COM1 **TX** for klog/panic but stops consuming COM1 RX for CLI
input.

**Shipped today:** COM1 RX is consumed by the kernel `console_read` syscall
(`serial_read_char` → remote serial driver if registered, else direct
`com1_serial_read_char`), which the CLI polls. Phases 1–2 move serial input onto
the IRQ→VT path and make the CLI event-driven.

---

### Output Routing

- **Per-slot fan-out (phase 4/5):** cell/scrollback buffer always; framebuffer
  iff the slot is visible (`render_now = (slot == g_active_tty) && !g_switch_barrier`);
  serial iff the slot is serial-bound.
- **Framebuffer exclusivity (phase 5):** the framebuffer is owned by exactly one
  of {compositor (vt-0), the VT's text render of one vt-x}. The compositor takes
  it by asking `vt` to switch to vt-0 and by locking the overlay
  (`FBTEXT_IPC_GFX_OVERLAY_REQ 1`); it releases by switching back to a text slot.
  Single-owner-per-frame removes the competing-writer flicker.
- **klog (phase 4, Shipped):** normal kernel log text reaches vt-1 through a
  VT-owned SPSC byte ring (`wasmos/ringbuf.h`) overlaid on a
  `BUFFER_KIND_TRANSFER` xfer-buffer — the same zero-copy transport the socket
  rings use. The VT acquires + maps the buffer, `wasmos_ringbuf_init`s it, and
  registers its id with the kernel (`klog_register_ring`); `serial_write` then
  publishes klog into that ring in addition to COM1 TX, and the VT drains it into
  vt-1 on each wake of its main loop. This is **additive**: the kernel still
  writes the legacy `console_ring` and the framebuffer driver still drains it for
  early-boot on-screen klog. Retiring that console-ring drain (making the
  framebuffer a pure blit surface) moves to phase 5, alongside the default-visible
  flip to vt-1 — until vt-1 is visible, dropping it would blank the boot screen.
  klog additionally always writes COM1 **TX** directly, so serial shows it
  regardless of the visible slot.
- **Panic bypass:** panic/fault output writes COM1 directly (`serial_printf_unlocked`)
  and paints the framebuffer directly (`src/kernel/framebuffer.c`,
  `panic_render_screen`), bypassing the ring and the VT — the kernel may be in an
  illegal state. This is unchanged by the redesign.

---

### Keyboard IPC Interface

| Opcode                   | Value | Direction            | Meaning                                  |
|--------------------------|-------|----------------------|------------------------------------------|
| `KBD_IPC_SUBSCRIBE_REQ`  | 0x800 | vt → keyboard driver | Register for key notifications           |
| `KBD_IPC_SUBSCRIBE_RESP` | 0x880 | keyboard → vt        | Acknowledgment                           |
| `KBD_IPC_KEY_NOTIFY`     | 0x801 | keyboard → vt        | arg0=scancode, arg1=keyup, arg2=extended |

`KBD_IPC_KEY_NOTIFY` is fire-and-forget (`request_id = 0`). After phase 3 the VT
is the sole subscriber.

---

### Escape Sequence Parser

The parser state machine operates per-slot on each byte fed by `vt_process_byte`.

```
ESC_NORMAL ──(0x1B)──▶ ESC_ESC ──('[')──▶ ESC_CSI ──(final 0x40–0x7E)──▶ ESC_NORMAL
                         └──(other)──▶ ESC_NORMAL
```

In `ESC_CSI`:
- `?` sets `csi_private = 1`
- `'0'–'9'` accumulates `csi_current` (decimal)
- `';'` pushes `csi_current` into `csi_params[csi_count++]` (max 8 params)
- Final byte (`0x40–0x7E`) pushes last param, dispatches to
  `vt_apply_private_csi` or `vt_apply_csi`, then resets to `ESC_NORMAL`

#### Implemented CSI Sequences (non-private)

| Sequence    | Final | Meaning                                                     |
|-------------|-------|-------------------------------------------------------------|
| `CSI n A`   | `A`   | Cursor up N (default 1)                                     |
| `CSI n B`   | `B`   | Cursor down N (default 1)                                   |
| `CSI n C`   | `C`   | Cursor right N (default 1)                                  |
| `CSI n D`   | `D`   | Cursor left N (default 1)                                   |
| `CSI r;c H` | `H`   | Cursor position (1-based row;col, clamped to grid)          |
| `CSI r;c f` | `f`   | Same as `H`                                                 |
| `CSI n J`   | `J`   | Erase in display: 0=cursor to end, 1=start to cursor, 2=all |
| `CSI n K`   | `K`   | Erase in line: 0=cursor to end, 1=start to cursor, 2=all    |
| `CSI ... m` | `m`   | SGR: multiple params, see table below                       |
| `CSI s`     | `s`   | Save cursor position                                        |
| `CSI u`     | `u`   | Restore saved cursor position                               |

#### Private CSI Sequences (`?` prefix)

| Sequence      | Meaning            |
|---------------|--------------------|
| `CSI ?25h`    | Show cursor        |
| `CSI ?25l`    | Hide cursor        |

#### SGR Parameter Codes

| Code(s)   | Effect                                |
|-----------|---------------------------------------|
| 0         | Reset: fg=15, bg=0, attr=0            |
| 1         | Bold                                  |
| 22        | Bold off                              |
| 30–37     | Foreground colors 0–7                 |
| 39        | Default foreground (15)               |
| 40–47     | Background colors 0–7                 |
| 49        | Default background (0)                |
| 90–97     | Bright foreground colors 8–15         |
| 100–107   | Bright background colors 8–15         |

Multiple SGR params in a single `m` sequence are processed left to right.

---

### Character Rendering

`vt_put_char_virtual` handles special characters for text slots:

| Character | Action                                                     |
|-----------|------------------------------------------------------------|
| `\r`      | cursor_col = 0                                             |
| `\n`      | cursor_col = 0; scroll or cursor_row++                     |
| `\b`      | cursor_col--; store space at old position; re-render       |
| `\t`      | advance to next multiple of 8 (recursive space expansion)  |
| other     | store in cell; advance cursor; wrap and scroll at edge     |

**Shipped:** `vt_put_char_tty0` tracks the cursor but routes tty0 output through
`wasmos_console_write` (serial + console ring). Phase 4/5 relocate the
serial-mirrored console to vt-1's fan-out; vt-0 becomes GUI-only.

Scroll: `vt_scroll_up` copies rows 1..N-1 down over rows 0..N-2, clears the last
row, and sends `FBTEXT_IPC_SCROLL_REQ(n=1)` to the framebuffer driver.

---

### TTY Switching

`vt_switch_tty(slot_index)` in `vt_main.c`:

1. If `g_fb_ep < 0` (framebuffer unavailable): logical switch only — increment
   `g_switch_generation`, update `g_active_tty`, return 0.
2. Set `g_switch_barrier = 1`.
3. If previous slot was a console slot: send `FBTEXT_IPC_CONSOLE_MODE_REQ(0)`.
   On failure: clear barrier, return `VT_SWITCH_ERR_MODE_OFF`.
4. Send `FBTEXT_IPC_CLEAR_REQ`. On failure: restore console mode, clear barrier,
   return `VT_SWITCH_ERR_CLEAR`.
5. `vt_replay_tty(slot_index, reliable=1)`: repaint the target slot. When the
   shared blit buffer is available (the common case) this copies the slot's grid
   into it and sends a single `FBTEXT_IPC_BLIT_GRID_REQ` — no per-cell IPC. The
   fallback path replays cells row by row (skipping blanks that already match the
   cleared framebuffer), yielding between rows. On failure: restore console mode,
   clear barrier, return `VT_SWITCH_ERR_REPLAY`.

   When leaving vt-0 for a text slot the gfx overlay is unlocked *before* this
   repaint, so the framebuffer driver actually renders the cells (it drops cell
   ops while the overlay is locked).
6. Console-mode re-enable / overlay handshake for the target slot. On failure:
   return `VT_SWITCH_ERR_MODE_ON`.
7. Increment `g_switch_generation`. Set `g_active_tty = slot_index`.
8. Emit `VT_TRACE_SWITCH`. Clear `g_switch_barrier`.
9. Switching to vt-0 locks the gfx overlay (`FBTEXT_IPC_GFX_OVERLAY_REQ 1`);
   switching away unlocks it.

#### Switch Error Codes

| Code                        | Value | Meaning                                     |
|-----------------------------|-------|---------------------------------------------|
| `VT_SWITCH_ERR_INVALID_TTY` | -1    | slot_index ≥ VT_MAX_TTYS                    |
| `VT_SWITCH_ERR_MODE_OFF`    | -11   | CONSOLE_MODE_REQ(0) failed on fb endpoint   |
| `VT_SWITCH_ERR_CLEAR`       | -12   | CLEAR_REQ failed on fb endpoint             |
| `VT_SWITCH_ERR_REPLAY`      | -13   | Cell replay returned error                  |
| `VT_SWITCH_ERR_MODE_ON`     | -14   | CONSOLE_MODE_REQ(1) failed on fb endpoint   |

#### Stale Write Detection

Every `VT_IPC_WRITE_REQ` from a registered client carries `request_id` set to the
switch generation at write time. If `msg.request_id != g_switch_generation` the
write is dropped with `VT_TRACE_DROP_STALE`, preventing cell writes queued before
a switch from corrupting the newly displayed slot.

---

### Writer and Reader Registration

**Writer** (`VT_IPC_REGISTER_WRITER`): a client registers its endpoint as the
owner of a slot's write path. Conflicts replace the previous owner rather than
rejecting, keeping CLI recovery robust when a prior process exits without an
explicit unregister.

**Reader** (`VT_IPC_READ_REQ`): the first client to read from a slot claims its
reader slot (`g_tty_reader_ep[slot]`). Under the redesign the reader is
push-notified (`VT_IPC_INPUT_NOTIFY`) and blocks between notifications.

---

### Lazy CLI Spawn *(Proposed — phase 5)*

Text slots spawn their CLI on demand: on the first switch to an empty text slot,
`vt` asks the process manager (via `PROC_IPC_SPAWN_PATH`) to spawn `cli.wap`
**pinned to that slot** (a new spawn flag forces `si->tty = N` rather than the
`pm_alloc_cli_tty()` round-robin at `src/kernel/process_manager_spawn.c`). The
CLI reads its slot from the spawn-info contract (`wasmos_startup_tty()`). sysinit
no longer eagerly starts the single CLI.

---

### Keyboard Scancode Handling

**Proposed (phase 3):** a single 3-layer keymap (`plain`/`shift`/`altgr`, adopted
from the compositor's `KeyMap`) plus Ctrl/Shift/AltGr modifier tracking, produced
as a canonical key event. **Shipped:** the VT uses 2-layer `g_sc_to_ascii[58]` /
`g_sc_to_ascii_shift[58]` (PS/2 Set-1, indices 0–57); CapsLock and AltGr are not
yet tracked in the VT.

#### Modifier Tracking

| Scancode  | Key              | Action            |
|-----------|------------------|-------------------|
| 0x1D      | Ctrl (L + E ext) | `g_ctrl_down`     |
| 0x2A      | Left Shift       | `g_shift_down`    |
| 0x36      | Right Shift      | `g_shift_down`    |
| 0x38 ext  | AltGr            | `g_altgr_down` (phase 3) |

Key-up events update modifier state and return immediately (no character output).

#### TTY Switch Hotkeys

| Condition            | Scancodes          | Action               |
|----------------------|--------------------|----------------------|
| Ctrl+Shift held      | 0x3B–0x3E (F1–F4)  | Switch to vt-0–vt-3  |
| Active slot is text  | 0x3C–0x3E (F2–F4)  | Switch to vt-1–vt-3  |

#### Extended Key Mapping (raw mode)

| Scancode | Key       | Output in input queue            |
|----------|-----------|----------------------------------|
| 0x48     | Up        | `ESC [ A`                        |
| 0x50     | Down      | `ESC [ B`                        |
| 0x4D     | Right     | `ESC [ C`                        |
| 0x4B     | Left      | `ESC [ D`                        |
| 0x47     | Home      | `ESC [ H`                        |
| 0x4F     | End       | `ESC [ F`                        |
| 0x49     | Page Up   | `ESC [ 5 ~`                      |
| 0x51     | Page Down | `ESC [ 6 ~`                      |
| 0x52     | Insert    | `ESC [ 2 ~`                      |
| 0x53     | Delete    | `ESC [ 3 ~`                      |

In canonical mode, Up/Down arrows instead invoke history navigation
(`Ctrl+P` / `Ctrl+N` semantics) rather than emitting escape sequences.

For `vt-0` the canonical key event (including extended keys) is forwarded to the
compositor via `VT_IPC_KEY_FORWARD`; the enriched `GFX_EVENT_KEY` carries the
keysym + modifiers + up/down, so GUI apps get arrow/function keys (previously they
arrived as code 0, forcing games onto WASD).

#### Ctrl Chord Mapping

| Chord  | Scancode | Byte sent  |
|--------|----------|------------|
| Ctrl+U | 0x16     | 0x15 (NAK) |
| Ctrl+C | 0x2E     | 0x03 (ETX) |
| Ctrl+P | 0x19     | 0x10 (DLE) |
| Ctrl+N | 0x31     | 0x0E (SO)  |

vt-0 receives no text-queue keyboard input; keys route to the compositor.

---

### Line Discipline (Canonical Mode)

When `input_canonical = 1`, `vt_input_handle_char` processes each byte before it
enters the input queue:

| Input           | Action                                               |
|-----------------|------------------------------------------------------|
| `\r` or `\n`    | Flush `input_line` + `\n` to queue; store in history |
| `\b` or `0x7F`  | Delete last char from line buffer; echo `\b`         |
| `0x03` (Ctrl+C) | Clear line buffer; push `0x03` to queue; echo `^C\n` |
| `0x15` (Ctrl+U) | Clear line buffer; reset history nav                 |
| `0x10` (Ctrl+P) | Navigate to older history entry                      |
| `0x0E` (Ctrl+N) | Navigate to newer history entry                      |
| `< 0x20`        | Ignored (other control bytes)                        |
| printable       | Append to `input_line` (max 127 bytes); echo if set  |

History ring: 8 entries × 128 bytes per slot. `input_history_head` advances on
each commit. Navigation index -1 means the live (uncommitted) line. Duplicate
suppression: if the newest history entry matches the current line, it is not
re-stored.

In raw mode (`input_canonical = 0`) bytes pass directly to the input queue
without buffering or editing. Echo still applies if `input_echo = 1`.

---

### Trace Events

`vt_trace_mark(event, arg0, arg1)` emits a kernel trace via `wasmos_debug_mark`.

| Event                      | Value | Meaning                                             |
|----------------------------|-------|-----------------------------------------------------|
| `VT_TRACE_SWITCH`          | 0xA1  | Slot switch completed; arg0=new_slot, arg1=generation |
| `VT_TRACE_WRITER_OK`       | 0xA2  | Writer registered; arg0=slot, arg1=ep               |
| `VT_TRACE_WRITER_CONFLICT` | 0xA3  | Writer conflict resolved; arg0=slot, arg1=ep        |
| `VT_TRACE_DROP_UNOWNED`    | 0xA4  | Write dropped: source not a registered writer       |
| `VT_TRACE_DROP_STALE`      | 0xA5  | Write dropped: stale switch generation              |

---

### Structural Invariants

1. **The framebuffer has a single owner per frame.** Either the compositor
   (vt-0) or the VT's text render of one vt-x draws the framebuffer, never both.
   The overlay lock (`FBTEXT_IPC_GFX_OVERLAY_REQ`) enforces the handoff. *(phase 5)*
2. **vt-0 has no text-input queue.** Keyboard input to vt-0 is forwarded to the
   compositor; serial cannot bind to vt-0.
3. **Serial is bound to exactly one slot** (`g_serial_tty`, default 1), and that
   slot's output mirrors to serial even when it is not visible. *(phase 5)*
4. **Panic bypasses the VT.** Panic/fault output writes COM1 and the framebuffer
   directly, unlocked, because the kernel may be in an illegal state.
5. **Framebuffer text-cell endpoint is private to `vt`.** Other services write
   through `VT_IPC_WRITE_REQ`.
6. **g_switch_barrier + generation counter serialize switches.** Writes queued
   before a switch are invalidated by the generation mismatch after it completes.
7. **Input is push, then pull-to-drain.** `VT_IPC_INPUT_NOTIFY` wakes the reader;
   `VT_IPC_READ_REQ` drains. The VT never blocks a reader inside a read. *(phase 2)*

---

### Rollout

| Phase | Scope                                                        | Status   |
|-------|--------------------------------------------------------------|----------|
| 0     | Idle spin fixes (init/cli/font/fbpci) — CPU/mouse relief     | Shipped  |
| 1     | Serial IRQ4 → `VT_IPC_SERIAL_INPUT_REQ` → serial-bound slot  | Shipped  |
| 2     | Push input (`VT_IPC_INPUT_NOTIFY`); event-driven CLI         | Shipped  |
| 3     | Single keyboard decoder in VT (loadable `.kmap` layouts); compositor consumes `VT_IPC_KEY_FORWARD`; enriched `GFX_EVENT_KEY` (scancode) | Shipped  |
| 4     | klog into vt-1 via VT-owned xfer-buffer SPSC ring (additive; drained on each VT wake). FB→blit-surface retirement deferred to phase 5 | Shipped  |
| 5     | Default visible vt-1 (retires the fbpci console-ring drain); serial-bound selector; lazy CLI spawn; `vt_switch_tty` overlay-wedge fix | Proposed |

Keymap layouts are data files under `system/keymaps/` (`us-qwerty.kmap`,
`de-nodeadkeys.kmap`), loaded by the VT at init (built-in US fallback).  Runtime
layout switching and extracting a dedicated `keymapd` service remain future work;
the compositor's now-unused legacy keymap awaits a cleanup pass.
