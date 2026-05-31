## Virtual Terminal Service

This document describes the VT subsystem: the `vt` WASM service, the
framebuffer driver's text-cell IPC interface, keyboard routing, multi-TTY
switching, escape sequence parsing, and the line discipline.

---

### Component Split

```
keyboard driver ──KBD_IPC_KEY_NOTIFY──▶ vt (WASM service)
                                         │  escape parser
                                         │  TTY mux
                                         │  line discipline
                                         │
          ┌──────────────────────────────┘
          │  FBTEXT_IPC_CELL_WRITE_REQ / CURSOR_SET_REQ
          │  FBTEXT_IPC_SCROLL_REQ / CLEAR_REQ
          │  FBTEXT_IPC_CONSOLE_MODE_REQ / GEOMETRY_REQ
          ▼
  framebuffer_pci driver (native ELF)
  cell grid · font rasterization · direct pixel writes

kernel/services ──VT_IPC_WRITE_REQ──▶ vt
```

`vt` is the sole owner of the framebuffer driver's text-cell endpoint. Clients
write bytes to `vt`; `vt` parses escapes, updates per-TTY cell state, and
forwards cell/cursor/scroll operations to the framebuffer driver.

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
| `VT_MAX_TTYS`               | 4     | Total TTY count (tty0..tty3)               |
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
| `g_active_tty`        | 0    | Currently displayed TTY index                     |
| `g_tty_writer_ep[4]`  | -1   | Registered writer endpoint per TTY                |
| `g_tty_reader_ep[4]`  | -1   | Registered reader endpoint per TTY                |
| `g_switch_generation` | 1    | Monotonic counter, incremented on each TTY switch |
| `g_switch_barrier`    | 0    | Set to 1 during an in-progress TTY switch         |
| `g_ctrl_down`         | 0    | Ctrl modifier state                               |
| `g_shift_down`        | 0    | Shift modifier state                              |
| `g_vt_cols`           | 80   | Active column count (updated from FB geometry)    |
| `g_vt_rows`           | 25   | Active row count (updated from FB geometry)       |

---

### Initialization Sequence

`initialize(proc_endpoint, arg1, arg2, arg3)` in `src/services/vt/vt_main.c`:

1. `wasmos_ipc_create_endpoint()` → `g_vt_ep`
2. `wasmos_svc_register(proc_endpoint, g_vt_ep, "vt", 1)`
3. `wasmos_svc_lookup(proc_endpoint, g_vt_ep, "fb", 2)` → `g_fb_ep`
4. `wasmos_svc_lookup(proc_endpoint, g_vt_ep, "kbd", 3)` → `g_kbd_ep`
5. `vt_query_geometry()` — sends `FBTEXT_IPC_GEOMETRY_REQ`; clamps result to
   `[40, VT_MAX_COLS]` × `[16, VT_MAX_ROWS]`
6. `vt_heap_init()` + `vt_alloc_tty_cells()` — bump-allocate cell grids for
   all 4 TTYs from WASM heap (`__heap_base`, grows via `wasm_memory_grow`)
7. If allocation fails: fall back to `VT_COLS_DEFAULT × VT_ROWS_DEFAULT` and
   retry; returns -1 if fallback also fails
8. `vt_init_ttys()` — zero all TTY state; `g_active_tty=0`,
   `g_switch_generation=1`, `g_switch_barrier=0`
9. If `g_kbd_ep >= 0`: send `KBD_IPC_SUBSCRIBE_REQ`
10. If `g_fb_ep >= 0`: `vt_fb_console_mode(1)` — enable console ring drain
11. `wasmos_sys_notify_ready(proc_endpoint, g_vt_ep)`
12. Enter main `wasmos_ipc_recv` loop

---

### Framebuffer Driver IPC Interface

`vt` is the only caller of the framebuffer driver's text-cell endpoint.

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
| `FBTEXT_IPC_RESP`               | 0x680 | Acknowledgment from framebuffer                      |
| `FBTEXT_IPC_ERROR`              | 0x6FF | Error from framebuffer                               |

Cell color packing: `arg3 = (fg & 0x0F) << 8 | (bg & 0x0F)`.

`FBTEXT_IPC_CONSOLE_MODE_REQ` toggles whether the framebuffer driver drains
the kernel console ring (the shared-memory path fed by `serial_write`). The VT
service disables ring draining before replaying a non-tty0 virtual terminal and
re-enables it when switching back to tty0.

---

### VT Public IPC Interface

| Opcode                   | Value  | Caller → vt                                            |
|--------------------------|--------|--------------------------------------------------------|
| `VT_IPC_WRITE_REQ`       | 0x700  | Write up to 4 bytes: arg0–arg3, each zero-terminated   |
| `VT_IPC_READ_REQ`        | 0x701  | Request next input byte; arg0=tty index                |
| `VT_IPC_SET_ATTR_REQ`    | 0x702  | Set fg/bg/attr; arg0=fg, arg1=bg, arg2=attr            |
| `VT_IPC_SWITCH_TTY`      | 0x703  | Switch active TTY; arg0=tty index                      |
| `VT_IPC_GET_ACTIVE_TTY`  | 0x704  | Query; reply: arg0=switch_generation, arg1=active_tty  |
| `VT_IPC_REGISTER_WRITER` | 0x705  | Register caller as writer; arg0=tty index              |
| `VT_IPC_SET_MODE_REQ`    | 0x706  | Set input mode bits; arg0=mode flags                   |
| `VT_IPC_RESP`            | 0x780  | Success response                                       |
| `VT_IPC_ERROR`           | 0x7FF  | Error response                                         |

#### VT_IPC_WRITE_REQ

Bytes are packed one per arg word (arg0..arg3). Processing stops at the first
zero byte. Kernel-originated writes (source < 0) target `g_active_tty`
unconditionally and bypass ownership and generation checks. Client writes from
registered writers are validated against `g_switch_generation`; writes with a
stale generation are dropped silently.

#### VT_IPC_READ_REQ Response

- `VT_IPC_RESP`, arg0=0, arg1=byte: byte available
- `VT_IPC_RESP`, arg0=1, arg1=0: queue empty

#### VT_IPC_REGISTER_WRITER Response

- `VT_IPC_RESP`, arg0=g_switch_generation, arg1=tty_id: registered
- Conflicts: previous owner is replaced (recovery semantics, not rejection)

#### VT_IPC_SWITCH_TTY Response

- `VT_IPC_RESP`, arg0=g_switch_generation, arg1=g_active_tty: switched
- `VT_IPC_ERROR`, arg0=error_code, arg1=g_active_tty: failed

#### Input Mode Flags

| Constant                  | Value | Meaning                                 |
|---------------------------|-------|-----------------------------------------|
| `VT_INPUT_MODE_RAW`       | 0     | Raw; bytes pass directly to input queue |
| `VT_INPUT_MODE_CANONICAL` | 1<<0  | Line-buffered; commit on Enter          |
| `VT_INPUT_MODE_ECHO`      | 1<<1  | Echo input back to the TTY cell grid    |

---

### Keyboard IPC Interface

| Opcode                   | Value | Direction            | Meaning                                  |
|--------------------------|-------|----------------------|------------------------------------------|
| `KBD_IPC_SUBSCRIBE_REQ`  | 0x800 | vt → keyboard driver | Register for key notifications           |
| `KBD_IPC_SUBSCRIBE_RESP` | 0x880 | keyboard → vt        | Acknowledgment                           |
| `KBD_IPC_KEY_NOTIFY`     | 0x801 | keyboard → vt        | arg0=scancode, arg1=keyup, arg2=extended |

`KBD_IPC_KEY_NOTIFY` is fire-and-forget (`request_id = 0`). Multiple
subscribers can coexist; the keyboard driver does not know what `vt` does with
the event.

---

### Escape Sequence Parser

The parser state machine operates per-TTY on each byte fed by `vt_process_byte`.

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

`vt_put_char_virtual` handles special characters for tty1..tty3:

| Character | Action                                                     |
|-----------|------------------------------------------------------------|
| `\r`      | cursor_col = 0                                             |
| `\n`      | cursor_col = 0; scroll or cursor_row++                     |
| `\b`      | cursor_col--; store space at old position; re-render       |
| `\t`      | advance to next multiple of 8 (recursive space expansion)  |
| other     | store in cell; advance cursor; wrap and scroll at edge     |

For tty0: `vt_put_char_tty0` tracks the cursor for cell state but routes
output through `wasmos_console_write` (serial + console ring) instead of
directly sending cell writes to the framebuffer.

Scroll: `vt_scroll_up` copies rows 1..N-1 down over rows 0..N-2, clears the
last row, and sends `FBTEXT_IPC_SCROLL_REQ(n=1)` to the framebuffer driver.

---

### TTY Switching

`vt_switch_tty(tty_index)` in `vt_main.c`:

1. If `g_fb_ep < 0` (framebuffer unavailable): logical switch only —
   increment `g_switch_generation`, update `g_active_tty`, return 0.
2. Set `g_switch_barrier = 1`.
3. If previous TTY was tty0: send `FBTEXT_IPC_CONSOLE_MODE_REQ(0)` to pause
   ring drain. On failure: clear barrier, return `VT_SWITCH_ERR_MODE_OFF`.
4. Send `FBTEXT_IPC_CLEAR_REQ`. On failure: restore console mode, clear
   barrier, return `VT_SWITCH_ERR_CLEAR`.
5. `vt_replay_tty(tty_index, reliable=1)`: replay all cells row by row;
   yield between rows to reduce framebuffer queue saturation. On failure:
   restore console mode, clear barrier, return `VT_SWITCH_ERR_REPLAY`.
6. If next TTY is tty0: send `FBTEXT_IPC_CONSOLE_MODE_REQ(1)`. On failure:
   return `VT_SWITCH_ERR_MODE_ON`.
7. Increment `g_switch_generation`. Set `g_active_tty = tty_index`.
8. Emit `VT_TRACE_SWITCH`. Clear `g_switch_barrier`.
9. If tty0: call `vt_draw_tty0_hint` to display the "tty0 system console
   (read-only)" banner.

#### Switch Error Codes

| Code                        | Value | Meaning                                     |
|-----------------------------|-------|---------------------------------------------|
| `VT_SWITCH_ERR_INVALID_TTY` | -1    | tty_index ≥ VT_MAX_TTYS                     |
| `VT_SWITCH_ERR_MODE_OFF`    | -11   | CONSOLE_MODE_REQ(0) failed on fb endpoint   |
| `VT_SWITCH_ERR_CLEAR`       | -12   | CLEAR_REQ failed on fb endpoint             |
| `VT_SWITCH_ERR_REPLAY`      | -13   | Cell replay returned error                  |
| `VT_SWITCH_ERR_MODE_ON`     | -14   | CONSOLE_MODE_REQ(1) failed on fb endpoint   |

#### Stale Write Detection

Every `VT_IPC_WRITE_REQ` from a registered client carries `request_id` set to
the switch generation at write time. If `msg.request_id != g_switch_generation`
the write is dropped with `VT_TRACE_DROP_STALE`. This prevents cell writes
queued before a switch from corrupting the newly displayed TTY.

---

### Writer and Reader Registration

**Writer** (`VT_IPC_REGISTER_WRITER`): a client registers its endpoint as the
owner of a TTY's write path. Conflicts (a second client claiming the same slot)
replace the previous owner rather than rejecting; this keeps CLI recovery
robust when a prior process exits without an explicit unregister.

**Reader** (`VT_IPC_READ_REQ`): the first client to read from a TTY claims its
reader slot (`g_tty_reader_ep[tty_id]`). Subsequent read requests from a
different endpoint are rejected with `VT_IPC_ERROR`. The VT service does not
block; if the input queue is empty it returns immediately with arg0=1.

---

### Keyboard Scancode Handling

Scancode tables: `g_sc_to_ascii[58]` and `g_sc_to_ascii_shift[58]`
(PS/2 Set-1 scancodes, indices 0–57). CapsLock and AltGr are not currently
tracked.

#### Modifier Tracking

| Scancode  | Key              | Action            |
|-----------|------------------|-------------------|
| 0x1D      | Ctrl (L + E ext) | `g_ctrl_down`     |
| 0x2A      | Left Shift       | `g_shift_down`    |
| 0x36      | Right Shift      | `g_shift_down`    |

Key-up events update modifier state and return immediately (no character output).

#### TTY Switch Hotkeys

| Condition            | Scancodes          | Action               |
|----------------------|--------------------|----------------------|
| Ctrl+Shift held      | 0x3B–0x3E (F1–F4)  | Switch to tty0–tty3  |
| Active tty = 0       | 0x3C–0x3E (F2–F4)  | Switch to tty1–tty3  |

#### Extended Key Mapping (raw mode, `csi_private` path)

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

#### Ctrl Chord Mapping

| Chord  | Scancode | Byte sent  |
|--------|----------|------------|
| Ctrl+U | 0x16     | 0x15 (NAK) |
| Ctrl+C | 0x2E     | 0x03 (ETX) |
| Ctrl+P | 0x19     | 0x10 (DLE) |
| Ctrl+N | 0x31     | 0x0E (SO)  |

tty0 receives no keyboard input; it is a read-only system console mirror.

---

### Line Discipline (Canonical Mode)

When `input_canonical = 1`, `vt_input_handle_char` processes each byte before
it enters the input queue:

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

History ring: 8 entries × 128 bytes per TTY. `input_history_head` advances
forward on each commit. Navigation index -1 means the live (uncommitted) line.
Navigating to -1 restores the original input. Duplicate suppression: if the
newest history entry matches the current line, it is not re-stored.

In raw mode (`input_canonical = 0`) bytes pass directly to the input queue
without buffering or editing. Echo still applies if `input_echo = 1`.

---

### Trace Events

`vt_trace_mark(event, arg0, arg1)` emits a kernel trace via `wasmos_debug_mark`.

| Event                      | Value | Meaning                                             |
|----------------------------|-------|-----------------------------------------------------|
| `VT_TRACE_SWITCH`          | 0xA1  | TTY switch completed; arg0=new_tty, arg1=generation |
| `VT_TRACE_WRITER_OK`       | 0xA2  | Writer registered; arg0=tty, arg1=ep                |
| `VT_TRACE_WRITER_CONFLICT` | 0xA3  | Writer conflict resolved; arg0=tty, arg1=ep         |
| `VT_TRACE_DROP_UNOWNED`    | 0xA4  | Write dropped: source not a registered writer       |
| `VT_TRACE_DROP_STALE`      | 0xA5  | Write dropped: stale switch generation              |

---

### Structural Invariants

1. **tty0 is read-only.** Keyboard input never routes to tty0's input queue.
   Output to tty0 goes through `wasmos_console_write` (serial + console ring),
   not directly to the framebuffer cell IPC path.

2. **g_switch_barrier serializes switches.** Writes during an in-progress
   switch that target the active TTY are not guarded by the barrier; they may
   land on either the old or new TTY. The generation counter handles stale
   detection after the switch completes.

3. **Replay is best-effort.** Cell updates dropped during `vt_replay_tty` due
   to persistent framebuffer backpressure are tolerated. The switch does not
   abort on dropped cells to avoid user-visible failure on replay under load.

4. **Framebuffer endpoint is private.** Only `vt` sends `FBTEXT_IPC_*` opcodes.
   Other services must write through the `VT_IPC_WRITE_REQ` path.

5. **No blocking reads.** `VT_IPC_READ_REQ` always replies immediately. The
   caller is responsible for polling if the queue is empty.
