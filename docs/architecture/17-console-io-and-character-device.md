## Console I/O and Character Device

This document describes the console I/O path from WASM service to terminal
output: the `wasmos_console_write` hostcall, the VT mirror, the chardev IPC
protocol, and the read path.

**Sources**: `src/kernel/wasm3_link.c`,
`src/drivers/chardev/chardev_server.c`,
`src/kernel/include/wasmos_ipc.h`

---

### Overview

WASM services produce output through a single hostcall,
`wasmos_console_write`. The kernel handles this call in two ways
simultaneously:

1. **Serial output** — the bytes are forwarded directly to the UART (visible
   in QEMU's serial console / `-serial mon:stdio`).
2. **VT mirror** — the bytes are also passed to `wasm_console_write_vt_mirror`
   which routes them to the active virtual terminal if one is running.

This dual-path design means `printf` output from any WASM service appears on
both the serial log and the framebuffer terminal without the service having
any knowledge of which output channel is active.

---

### `wasmos_console_write` Hostcall

**Source**: `src/kernel/wasm3_link.c`

Signature (wasm3 import descriptor): `"i(*i*i)"`

```
arg0 : i32 — pointer to output buffer in WASM linear memory
arg1 : i32 — length of output buffer
arg2 : i32 — pointer to return value location
arg3 : i32 — length of return value buffer (unused for console write)
```

Implementation path:

1. Validates the pointer and length against the caller's linear memory bounds.
2. Writes `length` bytes to the serial port via the kernel serial driver.
3. Calls `wasm_console_write_vt_mirror(ptr, length)` to forward the same
   bytes to the VT routing layer.
4. Returns the number of bytes written.

The VT mirror function dispatches to the compositor's virtual terminal input
handler if the compositor service is active. If not, the call is a no-op and
serial-only output continues.

---

### `wasmos_console_read` Hostcall

Signature: `"i(*i)"`

```
arg0 : i32 — pointer to receive buffer
arg1 : i32 — capacity of receive buffer
```

Reads bytes from the active input source. The input source is whatever the
kernel's current console input endpoint is configured to be — typically the
keyboard driver's character stream or the chardev service in test
environments.

---

### Chardev IPC Protocol

**Source**: `src/drivers/chardev/chardev_server.c`

The chardev service is a lightweight single-byte test utility that provides
an IPC-based read/write interface. It is not the primary I/O path for
production services; it exists for integration testing where a service needs
to send or receive a single byte through an IPC-fronted pipe.

Service name: `"chardev"` (as registered with the process manager).

#### State

```c
static uint8_t g_last_byte;
static int32_t g_has_data;
```

One byte of buffering. If `g_has_data == 0` a read request returns an error
response.

#### IPC Opcodes

| Opcode                        | Value | Direction        | Meaning                      |
|-------------------------------|-------|------------------|------------------------------|
| `WASM_CHARDEV_IPC_READ_REQ`   | 0x100 | client → chardev | Request the buffered byte    |
| `WASM_CHARDEV_IPC_WRITE_REQ`  | 0x101 | client → chardev | Store a byte in the buffer   |
| `WASM_CHARDEV_IPC_READ_RESP`  | 0x180 | chardev → client | Reply with the buffered byte |
| `WASM_CHARDEV_IPC_WRITE_RESP` | 0x181 | chardev → client | Acknowledge the write        |
| `WASM_CHARDEV_IPC_ERROR_RESP` | 0x1FF | chardev → client | No data / invalid opcode     |

`READ_REQ` response fields:

```
arg0 = byte value (0–255)
```

`WRITE_REQ` fields:

```
arg0 = byte value to store
```

Error response arg0 values:

| Value | Meaning                       |
|-------|-------------------------------|
| -1    | No data available (read miss) |
| -22   | Unknown opcode (EINVAL)       |

---

### Output Routing Summary

```
WASM service calls printf / write
         │
         ▼
wasmos_console_write (hostcall)
         │
         ├──► serial_write() ──► UART (always)
         │
         └──► wasm_console_write_vt_mirror()
                    │
                    ├── compositor active? ──► VT input handler ──► framebuffer terminal
                    │
                    └── no compositor ──► (dropped)
```

---

### Structural Invariants

1. **Serial output is unconditional.** Every call to `wasmos_console_write`
   reaches the UART regardless of compositor or VT state. Serial remains
   usable for diagnostics when the framebuffer is unavailable.

2. **Chardev is a test stub, not a production I/O path.** Real services use
   the PS/2 keyboard driver (IRQ-driven, up to 4 subscribers) and the
   compositor's VT for interactive I/O, not the chardev single-byte buffer.

3. **The VT mirror is one-way.** `wasmos_console_write` pushes output into
   the VT; it does not read back from it. Reading is a separate hostcall
   (`wasmos_console_read`) with its own input source.

4. **No flow control.** The serial write path does not block if the UART is
   busy; excess bytes may be dropped under high output volume. This is
   acceptable for diagnostic/log output.
