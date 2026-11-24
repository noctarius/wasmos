## Console I/O and Character Device

> **Documentation status: Implemented reference.**

This document describes the console I/O path from WASM service to terminal
output: the `wasmos_console_write` hostcall, its routing through the vt, the
chardev IPC protocol, and the read path.

**Sources**: `src/kernel/wasm3/link.c`,
`src/drivers/char/chardev/chardev_server.c`,
`src/kernel/include/wasmos_ipc.h`

---

### Overview

Every process prints through a single hostcall, `wasmos_console_write` (native
services: `api->console_write`). Where the bytes go depends on the caller's
**controlling slot** (`process_t.ctty`, set at spawn — see
`19-virtual-terminal.md`, slot model):

1. **A process with a controlling slot** — a shell and everything it starts —
   writes to that vt slot. The vt renders the slot and, while it is the
   serial-bound slot, transmits it on serial.
2. **A process without one** — drivers, services, anything started outside a
   shell — writes kernel log, which the vt shows on vt-1 and transmits while
   vt-1 is serial-bound.

A service therefore needs no knowledge of which output channel is active, and
an app's output stays on the slot it was run from.

---

### `wasmos_console_write` Hostcall

**Sources**: `src/kernel/wasm3/link.c`, `src/kernel/warp/link.cpp`,
`src/kernel/native_driver.c`, `src/kernel/serial.c`

Parameters (`abi/hostcalls.yaml` `console_write`): a guest buffer and its
length. Returns 0, including for a zero length; `WASMOS_INVAL` for a negative
length; `WASMOS_ERR_KERNEL_BAD_POINTER` for a range outside linear memory.

Implementation path:

1. Validates the range against the caller's linear memory.
2. Relays it in NUL-terminated 127-byte chunks to
   `console_write_tty(process_current_ctty(), chunk)`, with preemption disabled
   so one write is not interleaved with another process's.
3. `console_write_tty` publishes each chunk into the vt's console ring as a
   record tagged with the slot (0 = kernel log). Before the vt registers the ring,
   or when the ring is too full to take the text, the kernel transmits it on
   COM1 itself.

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

**Source**: `src/drivers/char/chardev/chardev_server.c`

The chardev service is a lightweight single-byte test utility that provides
an IPC-based read/write interface. It is not the primary I/O path for
production services; it exists for integration testing where a service needs
to send or receive a single byte through an IPC-fronted pipe.

It is a normal WASM driver packed as a WASMOS-APP, loaded from initfs and
started at boot by device-manager (via a `SUBSYSTEM=="boot"` rule), exactly
like every other driver. Its `initialize` export creates a service endpoint,
self-registers under the service name `"chardev"` with the process manager,
and then serves request/reply IPC. Clients resolve it at runtime with
`wasmos_svc_lookup(..., "chardev", ...)`.

#### State

```c
static uint8_t g_last_byte;
static uint8_t g_has_data;
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
process calls printf / write
         │
         ▼
wasmos_console_write (hostcall) ──► console_write_tty(ctty)
         │
         ├── console ring registered ──► record {slot, text} ──► vt
         │                                  ├─► slot grid ──► framebuffer (if visible)
         │                                  └─► COM1 via serial_owner_write (if serial-bound)
         │
         └── no ring yet / ring full ──► COM1 directly
```

---

### Structural Invariants

1. **Serial carries the serial-bound slot.** Once the vt owns the console UART
   (COM1 on x86_64, the PL011 on aarch64), a write
   reaches the UART if and only if it belongs to the serial-bound slot (kernel
   log belongs to vt-1). Before the vt is up, and for text the console ring
   cannot take, the kernel transmits directly, so output is never lost. Panic
   output always goes straight to the UART.
   The kernel log alone also leaves on a separate kernel-log port: by default
   a virtio console the `virtio-serial` driver feeds from the kmsg ring, or,
   with `log.serial=com2` on the kernel command line (x86_64), a second UART
   the kernel writes directly (`19-virtual-terminal.md`, kernel-log port).

2. **Chardev is a test stub, not a production I/O path.** Real services use
   the keyboard driver -- PS/2 on x86_64, virtio-input on aarch64, both
   IRQ- or MSI-driven with up to 4 subscribers -- and the
   compositor's VT for interactive I/O, not the chardev single-byte buffer.

3. **Console output is one-way.** `wasmos_console_write` pushes output into
   the vt; it does not read back from it. Reading is a separate hostcall
   (`wasmos_console_read`) with its own input source.

4. **No flow control.** The serial write path does not block if the UART is
   busy; excess bytes may be dropped under high output volume. This is
   acceptable for diagnostic/log output.
