## Virtual Input Testing via Virtio-Serial

This document describes the implemented virtio-serial driver, the keyboard and
mouse driver IPC contracts, the compositor's input subscription model, and the
design for the not-yet-implemented virtual input injection layer that would sit
on top of those contracts.

---

### What Is Implemented

| Component                                        | State           | Source                                           |
|--------------------------------------------------|-----------------|--------------------------------------------------|
| Virtio-serial PCI discovery and register access  | Implemented     | `src/drivers/virtio_serial/virtio_serial.c`      |
| Device manager match rule for virtio-serial      | Implemented     | `scripts/system/devmgr/rules/default.rules`      |
| PS/2 keyboard driver (IRQ-driven + polling)      | Implemented     | `src/drivers/keyboard/keyboard.ts`               |
| PS/2 mouse driver (IRQ-driven + polling)         | Implemented     | `src/drivers/mouse/mouse.ts`                     |
| Compositor keyboard + mouse subscription         | Implemented     | `src/services/gfx_compositor/gfx_compositor.zig` |
| Python QEMU test framework                       | Implemented     | `scripts/qemu_test_framework.py`                 |
| Virtio-serial queue setup and data plane         | **Deferred**    | See TODO in `virtio_serial.c:189`                |
| Virt-input service (protocol parser + injection) | **Not started** | —                                                |

---

### Virtio-Serial Driver

**Source**: `src/drivers/virtio_serial/virtio_serial.c`

The driver is a WASM C application with 64 KiB of memory (stack 4096 bytes,
initial and max memory 65536 bytes). It handles PCI enumeration, registers a
named service, and exposes a register-access IPC interface to other drivers.

#### PCI Scan

```c
#define PCI_CFG_ADDR_PORT         0xCF8
#define PCI_CFG_DATA_PORT         0xCFC

#define VIRTIO_PCI_VENDOR_ID      0x1AF4u
#define VIRTIO_PCI_DEV_MIN        0x1000u
#define VIRTIO_PCI_DEV_MAX        0x107Fu
#define VIRTIO_SERIAL_DEV_LEGACY  0x1003u      // virtio-serial, legacy
#define VIRTIO_SERIAL_DEV_TRANSITIONAL 0x1043u  // virtio-serial, transitional
```

`probe_virtio_serial()` performs a full BDF scan (bus 0–255, slot 0–31,
function 0–7). For each candidate it verifies:
1. Vendor ID `0x1AF4`.
2. Device ID is `0x1003`, `0x1043`, or within `[0x1000, 0x107F]`.
3. PCI class `0x07` (communications), subclass `0x00`.
4. BAR0 is I/O-space (bit 0 = 1) and non-zero after masking bits [1:0].

On success it records the device in `g_dev`:

```c
typedef struct {
    uint8_t  present;
    uint8_t  bus, slot, function, irq;
    uint16_t io_base;
    uint16_t vendor_id, device_id;
} virtio_serial_device_t;
```

#### Service Registration and Log Markers

After probing the device the driver registers as `"virtio.serial"` via
`wasmos_svc_register` and then calls `wasmos_sys_notify_ready`.

Log output:
```
[virtio-serial] ready io=0xXXXX irq=N dev=XXXX   // device found
[virtio-serial] no device found                   // no match
[virtio-serial] register failed                   // svc_register error
```

#### IPC Interface

| Opcode  | Constant                            | Direction       |
|---------|-------------------------------------|-----------------|
| `0x830` | `VIRTIO_SERIAL_IPC_QUERY_REQ`       | Client → driver |
| `0x831` | `VIRTIO_SERIAL_IPC_READ_REG32_REQ`  | Client → driver |
| `0x832` | `VIRTIO_SERIAL_IPC_WRITE_REG32_REQ` | Client → driver |
| `0x8B0` | `VIRTIO_SERIAL_IPC_RESP`            | Driver → client |
| `0x8BF` | `VIRTIO_SERIAL_IPC_ERROR`           | Driver → client |

**QUERY_REQ** — returns device presence and identity:

| Field  | Content                                                 |
|--------|---------------------------------------------------------|
| `arg0` | `present` (0 or 1)                                      |
| `arg1` | `(vendor_id << 16) \| device_id`                        |
| `arg2` | `(bus << 24) \| (slot << 16) \| (function << 8) \| irq` |
| `arg3` | `io_base`                                               |

**READ_REG32_REQ** — `arg0 = offset` (must be in [0, 0x3C], 4-byte aligned):

| Field  | Content             |
|--------|---------------------|
| `arg1` | register value read |
| `arg2` | offset echoed       |

**WRITE_REG32_REQ** — `arg0 = offset`, `arg1 = value`:

| Field  | Content       |
|--------|---------------|
| `arg1` | offset echoed |
| `arg2` | value echoed  |

Error reply (`VIRTIO_SERIAL_IPC_ERROR`):
- `arg0 = -2`: device not present.
- `arg0 = -22`: invalid or misaligned offset (EINVAL).
- `arg0 = -38`: unknown IPC type (ENOSYS).

#### Deferred Work

The driver does not yet set up VirtIO queues or expose a byte-stream
interface. The transport layer is explicitly deferred:

```c
/* TODO(virtio-serial-transport): add queue setup and data/control-plane
 * operations; this initial slice is discovery + register access only. */
```

Until queue setup is implemented, the driver cannot transfer data to/from
host-side chardevs. Named-port enumeration and the `wasmos.virtinput`
channel depend on this foundation.

#### Device Manager Match Rule

```
SUBSYSTEM=="pci", ATTR{class}=="0x07", ATTR{subclass}=="0x00",
ATTR{prog_if}=="0x00", ATTR{vendor}=="0x1AF4",
RUN+="system/drivers/virtio_serial.wap"
```

This is the last entry in `scripts/system/devmgr/rules/default.rules` and
matches any VirtIO PCI communications device from vendor `0x1AF4`.

#### Linker Metadata

`src/drivers/virtio_serial/linker.metadata`:

```toml
[package]
name    = "virtio-serial"
kind    = "driver"
native  = false
entry   = "initialize"

[resources]
stack_pages = 16
heap_pages  = 16

[[capabilities]]
name = "io.port"   # PCI config-space access at 0x0CF8–0x0CFF

[[matches]]
bus       = "pci"
class     = 0x07
subclass  = 0x00
prog_if   = 0x00
vendor    = 0x1AF4
device    = "any"
io_port_min = 0x0CF8
io_port_max = 0x0CFF
priority  = 100
```

No `irq.route` capability is declared; the driver does not handle interrupts.

---

### Keyboard Driver

**Source**: `src/drivers/keyboard/keyboard.ts` (AssemblyScript/WASM)

Registered service name: `"kbd"` (encoded as the literal bytes `0x64 0x62 0x6B 0x00`).

#### Hardware Interface

| Constant               | Value  | Meaning                       |
|------------------------|--------|-------------------------------|
| `KEYBOARD_STATUS_PORT` | `0x64` | PS/2 controller status        |
| `KEYBOARD_DATA_PORT`   | `0x60` | PS/2 data                     |
| `KEYBOARD_OBF_FLAG`    | `0x01` | Output buffer full            |
| `KEYBOARD_AUX_FLAG`    | `0x20` | Byte is from AUX (mouse) port |
| `KBD_IRQ`              | `1`    | PS/2 keyboard IRQ             |

`readScancode()` returns -1 if OBF is clear or if the AUX flag is set (mouse
byte; left for the mouse driver). Otherwise returns the byte from port 0x60.

#### Subscriber Model

Up to four subscriber endpoints are tracked in `g_subs0..g_subs3` (hard
limit; `-1` = empty slot). Duplicate registrations are silently ignored.

| Opcode  | Constant                 | Meaning                                     |
|---------|--------------------------|---------------------------------------------|
| `0x800` | `KBD_IPC_SUBSCRIBE_REQ`  | Register for key events                     |
| `0x880` | `KBD_IPC_SUBSCRIBE_RESP` | Registration reply (`arg0 = 0` OK, -1 full) |
| `0x801` | `KBD_IPC_KEY_NOTIFY`     | Key event notification                      |

`KBD_IPC_KEY_NOTIFY` fields:

| Field  | Content                                                  |
|--------|----------------------------------------------------------|
| `arg0` | `scancode` — PS/2 Set 1 scancode, 7-bit (bit 7 stripped) |
| `arg1` | `keyup` — 0 = key down, 1 = key up                       |
| `arg2` | `extended` — 1 if preceded by `0xE0` prefix, else 0      |

#### IRQ and Polling Modes

The driver first attempts `irq_route_ipc(KBD_IRQ, g_kbd_ep)`. If that
succeeds, it runs an IRQ-driven event loop: `ipc_recv` blocks until either a
client request or an `0xFF00` IRQ event arrives. After reading the scan code
the driver calls `irq_ack(KBD_IRQ)` — this must happen after reading the OBF
to prevent level-triggered re-fire.

If IRQ routing fails, the driver falls back to a polling loop that interleaves
`drainIpc()` (non-blocking `ipc_try_recv`) with `readScancode()` / `io_wait`
/ `sched_yield` calls.

Extended key state (`0xE0` prefix) is tracked in `g_extended_pending` which
is reset to 0 after each complete scancode dispatch.

---

### Mouse Driver

**Source**: `src/drivers/mouse/mouse.ts` (AssemblyScript/WASM)

Registered service name: `"mouse"` (split across two IPC args:
`arg0 = 0x73756F6D` = `"mous"`, `arg1 = 0x65` = `'e'`).

#### Hardware Interface

| Constant           | Value  | Meaning                 |
|--------------------|--------|-------------------------|
| `CTRL_STATUS_PORT` | `0x64` | PS/2 controller status  |
| `CTRL_CMD_PORT`    | `0x64` | PS/2 controller command |
| `CTRL_DATA_PORT`   | `0x60` | PS/2 data               |
| `STATUS_OBF`       | `0x01` | Output buffer full      |
| `STATUS_IBF`       | `0x02` | Input buffer full       |
| `STATUS_AUX`       | `0x20` | AUX data present        |
| `MOUSE_IRQ`        | `12`   | PS/2 AUX IRQ            |

#### Initialization Sequence

`initMouseDevice()` runs at startup:
1. `flushOutputBuffer()` — drain any stale bytes (up to 64 reads).
2. Send `0xA8` to controller command port — enable AUX port.
3. Read CCB via `0x20` command; set bit 1 (AUX interrupt enable) via `0x60`
   write. This is required for IRQ 12 to fire.
4. Send mouse command `0xF6` (set defaults) + `0xF4` (enable streaming).
   Both ACK reads use a 4096-iteration timeout (`readAuxAck(4096)`).

The initialization is fail-open: a slow or virtual controller may delay ACKs.
Failure logs `[mouse] init failed` but does not abort the driver.

#### Subscriber Model

Identical structure to the keyboard driver (four static slots).

| Opcode  | Constant                   | Meaning                     |
|---------|----------------------------|-----------------------------|
| `0x810` | `MOUSE_IPC_SUBSCRIBE_REQ`  | Register for move events    |
| `0x890` | `MOUSE_IPC_SUBSCRIBE_RESP` | Registration reply          |
| `0x811` | `MOUSE_IPC_MOVE_NOTIFY`    | Mouse movement/button event |

`MOUSE_IPC_MOVE_NOTIFY` fields:

| Field  | Content                                                        |
|--------|----------------------------------------------------------------|
| `arg0` | `dx` — signed 8-bit X delta                                    |
| `arg1` | `dy` — signed 8-bit Y delta (negated: PS/2 Y-axis is inverted) |
| `arg2` | `buttons` — bits [2:0]: left, right, middle                    |

#### Packet Assembly

The driver assembles 3-byte PS/2 packets across IRQs using `g_packet_state`
(0/1/2) and `g_packet0`/`g_packet1` as staging bytes.

`handleAuxByte(byte)`:
- State 0: sync — byte must have bit 3 set, otherwise packet is discarded.
- State 1: store `g_packet1`, advance to state 2.
- State 2: complete packet. If overflow bits in byte 0 [7:6] are set, drop.
  Otherwise extract `dx` (sign-extended byte 1), `dy` (negated sign-extended
  byte 2), `buttons` (byte 0 bits [2:0]).

`readAuxByte()` returns -1 if OBF is clear, -2 if OBF is set but AUX flag is
clear (byte belongs to keyboard driver).

---

### Compositor Input Subscription

**Source**: `src/services/gfx_compositor/gfx_compositor.zig`

The compositor subscribes to both input drivers during initialization and
maintains subscriptions dynamically across driver restarts.

#### Lookup and Subscribe

```
lookup_kbd_endpoint()   → svcLookupEndpointRetry("kbd",  ...)
lookup_mouse_endpoint() → svcLookupEndpointRetry("mouse", ...)
subscribe_keyboard()    → KBD_IPC_SUBSCRIBE_REQ to g_kbd_endpoint
subscribe_mouse()       → MOUSE_IPC_SUBSCRIBE_REQ to g_mouse_endpoint
```

State variables: `g_kbd_endpoint`, `g_mouse_endpoint`, `g_kbd_subscribed`,
`g_mouse_subscribed`. All four are checked each heartbeat tick: if an endpoint
is no longer alive, the subscription state is reset and the driver is
re-looked-up and re-subscribed automatically.

#### Event Handlers

`KBD_IPC_KEY_NOTIFY` is dispatched to the focused window via the IPC event
loop registered in `eventRegister(&g_ipc_loop, KBD_IPC_KEY_NOTIFY, ...)`.

`MOUSE_IPC_MOVE_NOTIFY` is handled by `handle_mouse_notify()`:
- Extracts `dx` and `dy` as `i8` (truncated from `arg0`/`arg1`).
- Calls `pointer_update_position(dx, dy)` — clamps pointer to screen bounds.
- If overlay is locked (gfx mode active), requests repaint of old and new
  cursor positions.
- Calls `handle_mouse_resize`, `handle_mouse_drag`, and
  `handle_mouse_press_transition` for window chrome interaction.
- Calls `maybe_emit_pointer_event` to forward events to the focused window.
- Updates `g_pointer_buttons` from `arg2 & 0x7`.

---

### Virtual Input Injection Design

The virt-input layer — a guest service that reads host commands over the
virtio-serial transport and injects them as keyboard/mouse events — is not yet
implemented. The sections below document the intended design.

#### Host QEMU Wiring

```sh
-device virtio-serial-pci \
-chardev socket,id=virtinput,path=/tmp/wasmos-virtinput.sock,server=on,wait=off \
-device virtserialport,chardev=virtinput,name=wasmos.virtinput
```

One named port (`wasmos.virtinput`) is reserved for this channel. Unix domain
sockets are preferred for local test orchestration.

**Test isolation requirements:**
- Use per-test socket paths to avoid collisions.
- Remove stale socket files before launch.
- Do not run UI/QEMU integration tests in parallel (shared `build/esp`
  artifacts cause `Error deleting` races).

#### Injection IPC Contracts

A virt-input service would inject events by subscribing to neither driver
directly, but by **sending synthetic notify messages** on behalf of the
keyboard and mouse drivers. This requires the virt-input service to hold
subscriber registrations with the compositor and emit:

- `KBD_IPC_KEY_NOTIFY` (0x801): inject keyboard scan codes.
- `MOUSE_IPC_MOVE_NOTIFY` (0x811): inject pointer deltas and button states.

These messages traverse the same compositor code paths as hardware events —
there is no bypass.

For keyboard injection, the virt-input service must synthesize PS/2 Set 1
scan codes (7-bit value, `extended` flag for 0xE0-prefixed codes). For mouse
injection it sends signed 8-bit deltas.

#### Text Protocol v1

Transport: UTF-8 text lines terminated by `\n`, one command per line.

Command grammar:

```
HELLO <version>
MOVE_ABS <x> <y>
MOVE_REL <dx> <dy>
BTN <left|right|middle> <down|up>
WHEEL <dy>
KEY <code> <down|up>
TEXT <utf8-text>
MOD <shift|ctrl|alt|meta> <down|up>
SYNC <token>
PING <token>
```

Response lines:

```
OK <optional-data>
ERR <code> <message>
PONG <token>
SYNCED <token>
```

Semantics:
- `HELLO` negotiates protocol version (start with `1`).
- `MOVE_ABS` converts compositor-space absolute coordinates into a delta from
  the current pointer position before emitting `MOUSE_IPC_MOVE_NOTIFY`.
- `MOVE_REL` emits the delta directly (clamp to ±127 per axis per message or
  split into multiple notifications).
- `BTN` changes one button state; emits the full button bitmask in `arg2`.
- `KEY` maps to a PS/2 Set 1 scan code + extended flag, then emits
  `KBD_IPC_KEY_NOTIFY`.
- `TEXT` synthesizes down+up scan code pairs for each character.
- `MOD` injects modifier key transitions using well-known scan codes.
- `SYNC <token>` guarantees all prior accepted commands are injected; replies
  `SYNCED <token>` once the injection queue drains.

Validation:
- Numeric parsing must reject overflow and out-of-range values.
- Unknown button/key/modifier names are hard errors (`ERR`).
- Empty lines and `#` comments are ignored.

#### Capability and Gating

The virt-input service requires the virtio-serial data plane to be
implemented before it can read host commands. The service should be started
conditionally — either via a CMake feature toggle
(`WASMOS_VIRTINPUT_TESTING`, default off) or a `sysinit.rc` condition
checking for a device or configuration flag. It must not start in regular
production boots.

The virt-input service itself needs no I/O-port capability. It communicates
entirely over IPC with the virtio-serial driver (once the data plane exists)
and with the compositor (via subscriber registration + synthetic notify
messages).

#### Python Test Framework Integration

The existing `scripts/qemu_test_framework.py` provides `QemuConfig` and
`QemuSession` but has no virtinput fields. To enable virtual input testing,
`QemuConfig` would need:

```python
virtinput_socket_path: str = ""
virtinput_port_name: str = "wasmos.virtinput"
enable_virtinput: bool = False
```

Environment overrides (proposed):
- `WASMOS_QEMU_VIRTINPUT=1`
- `WASMOS_QEMU_VIRTINPUT_SOCK=/tmp/wasmos-virtinput.sock`
- `WASMOS_QEMU_VIRTINPUT_PORT=wasmos.virtinput`

A host bridge helper class would connect to the Unix socket with
timeout/retry and expose convenience methods (`move_abs`, `move_rel`, `click`,
`key_down`, `key_up`, `text`, `sync`). Tests would use `SYNC <token>` /
`SYNCED <token>` between action phases to sequence assertions against serial
log markers.

`scripts/qemu_ui_test.py` already boots to `wamos>` and optionally displays
a UI. UI automation tests would boot with `enable_virtinput`, wait for the
system ready marker, drive actions through the bridge helper, and assert
expected state transitions in serial output.

---

### Structural Invariants

1. **Virtio-serial currently exposes discovery only.** Queue setup and the
   data plane are not implemented. Any component that depends on reading bytes
   from the host over virtio-serial (including the virt-input service) must
   wait for the transport layer TODO to be resolved.

2. **Injected input uses the same IPC path as hardware input.** A virt-input
   service sends `KBD_IPC_KEY_NOTIFY` and `MOUSE_IPC_MOVE_NOTIFY` messages to
   the compositor's endpoint — the same messages the hardware drivers emit.
   There is no compositor bypass for injection.

3. **Subscriber slots are limited.** Both keyboard and mouse drivers cap
   subscribers at 4 (`g_subs0..g_subs3`). A virt-input service that registers
   as a subscriber consumes one slot. If the compositor and VT already
   subscribe, capacity may be exhausted.

4. **Mouse delta is 8-bit signed per packet.** Large absolute movements must
   be split across multiple `MOUSE_IPC_MOVE_NOTIFY` messages. `MOVE_ABS`
   commands in the text protocol must decompose the absolute-to-relative
   conversion into clamped 8-bit steps.

5. **Keyboard uses PS/2 Set 1 scan codes, not key names or Unicode.**
   The virt-input service must maintain a translation table from text-protocol
   key names to PS/2 Set 1 scan codes and extended flags. There is no
   higher-level key abstraction in the current IPC path.

6. **No parallel integration tests.** `run-qemu-test` and UI/virtinput
   integration tests share mutable `build/esp` artifacts. Running them
   concurrently causes flaky build failures. All QEMU test targets must be
   run sequentially.
