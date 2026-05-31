## Diagnostics and Testing

This document describes the WASMOS diagnostic infrastructure: the serial output
pipeline, kernel logging, the trace macro system, console ring, exception
reporting, debug hooks, and the Python-over-QEMU integration test framework.

Implementation status snapshots belong in `docs/STATUS.md`, not here.

---

### Serial Output Pipeline

All kernel diagnostic output routes through `src/kernel/serial.c`. The default
driver targets COM1.

#### COM1 Initialization

```c
#define COM1_PORT 0x3F8
```

`serial_init()` configures COM1 at startup:

| Register       | Value | Effect                                |
|----------------|-------|---------------------------------------|
| `COM1+1` (IER) | 0x00  | Disable all interrupts                |
| `COM1+3` (LCR) | 0x80  | Enable DLAB for baud rate divisor     |
| `COM1+0` (DLL) | 0x01  | Divisor low byte → 115200 baud        |
| `COM1+1` (DLH) | 0x00  | Divisor high byte                     |
| `COM1+3` (LCR) | 0x03  | 8 bits, no parity, 1 stop bit         |
| `COM1+2` (FCR) | 0xC7  | Enable FIFO, clear, 14-byte threshold |
| `COM1+4` (MCR) | 0x0B  | IRQs enabled, RTS/DSR set             |

TX is polled (spin on bit 5 of status register `COM1+5`). Every `\n` written
via `serial_write` emits a preceding `\r` for QEMU's `-serial mon:stdio` mode.

#### Locking Model

| Function                         | Lock behavior                             |
|----------------------------------|-------------------------------------------|
| `serial_write(s)`                | Acquires `g_serial_lock` spinlock         |
| `serial_printf(fmt, ...)`        | Acquires spinlock; 512-byte format buffer |
| `serial_write_unlocked(s)`       | No lock; calls `preempt_disable/enable`   |
| `serial_printf_unlocked`         | No lock; 512-byte format buffer           |
| `serial_write_hex64(v)`          | Locked; 18-byte hex string                |
| `serial_write_hex64_unlocked(v)` | Unlocked                                  |

The unlocked variants are used in exception and fault handlers where acquiring
the spinlock would be unsafe.

The write path always uses direct COM1 I/O (`com1_serial_put_char`). The
remote serial driver endpoint is retained for read requests only — per-character
IPC on the write side overflows the 32-slot endpoint queue for strings longer
than 32 bytes and causes out-of-order output.

#### Early Log Ring

`serial_write_unlocked` captures all output into a 4096-byte circular ring
(`g_early_log[EARLY_LOG_SIZE]`):

```c
#define EARLY_LOG_SIZE 4096
static uint8_t  g_early_log[EARLY_LOG_SIZE];
static uint32_t g_early_log_head  = 0;  /* next write index (wraps) */
static uint32_t g_early_log_count = 0;  /* bytes written, capped at ring size */
```

The ring is queryable after boot:

```c
uint32_t serial_early_log_size(void);
void     serial_early_log_copy(uint8_t *dst, uint32_t offset, uint32_t len);
```

Logical index 0 is the oldest byte. When the ring is full (count reaches 4096),
`head` marks the oldest position and new writes overwrite oldest content.

#### High-Alias Mode

After the kernel relocates to the higher-half virtual address, `serial_enable_high_alias(1)`
is called so that `serial_write_unlocked` adjusts pointers for low-address
kernel globals (GDT/IDT/TSS are accessed via their higher-half addresses, but
some constants survive from pre-relocation addresses). The alias check compares
each pointer against `__kernel_start...__kernel_end` symbols.

---

### Kernel Log API

`src/kernel/klog.c` wraps `serial_write`:

```c
void klog_write(const char *s);
void klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

`klog_printf` uses a 512-byte stack buffer. These are the standard log functions
for non-trace kernel messages.

---

### Trace Macro System

`src/kernel/include/serial.h` defines the trace macros:

```c
#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

#if WASMOS_TRACE
#define trace_write(s)           serial_write(s)
#define trace_write_unlocked(s)  serial_write_unlocked(s)
#define trace_do(stmt)           do { stmt; } while (0)
#else
#define trace_write(s)           ((void)0)
#define trace_write_unlocked(s)  ((void)0)
#define trace_do(stmt)           ((void)0)
#endif
```

Enable with CMake: `cmake -S . -B build -DWASMOS_TRACE=ON`.

When `WASMOS_TRACE=1`:
- `[init]` process manager spawn, wasm3 probe results
- `[kmap]` context dump loop markers (`[kmap] contexts begin/end`, per-PID lines)
- `[wasm] debug_mark tag=...` from WASM-level `wasmos_debug_mark()` calls
- Timer tick progress markers (timer.c)
- IPC ownership and scheduler transition traces throughout the kernel

When `WASMOS_TRACE=0` (default): all of the above are compiled to no-ops;
only always-on log calls (`klog_write`, `serial_write`, `serial_printf`)
remain active.

---

### Console Ring (Serial → Framebuffer Chardev)

`src/kernel/include/console_ring.h`:

```c
#define CONSOLE_RING_DATA_SIZE 4080u

typedef struct {
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    uint32_t          capacity;
    uint32_t          _pad;
    uint8_t           data[CONSOLE_RING_DATA_SIZE];
} console_ring_t;
```

Total struct size: 16 + 4080 = 4096 bytes (exactly one page). The ring is
allocated as a shared-memory region (`mm_shared_create(0, 1, READ|WRITE)`)
so that the framebuffer chardev can map it and forward serial text to the
on-screen text console without per-character IPC.

`serial_ring_write(s)` appends to the ring from within
`serial_write_unlocked()`. The ring pointer is also accessible via:

```c
uint32_t serial_console_ring_id(void);  /* shared-memory object ID */
void    *serial_console_ring_ptr(void); /* direct kernel pointer    */
```

**Note:** `serial_ring_write` currently returns without writing when
`g_serial_high_alias_enabled` is set. This is a known limitation in the
ring-3 strict-mode path — the TODO in `serial.c` tracks routing framebuffer
logging through a CR3-invariant path.

#### Keyboard Input Ring

A separate 64-byte ring (`g_input_ring[INPUT_RING_SIZE]`) receives keyboard
bytes pushed from the VT/keyboard driver:

```c
#define INPUT_RING_SIZE 64
void serial_input_push(uint8_t ch);  /* called by keyboard driver   */
int  serial_input_read(uint8_t *out); /* polled by kernel console API */
```

`serial_input_read` is polled by the `wasmos_console_read` WASM host import
before falling back to direct COM1 polling.

---

### Exception Reporting

Two functions in `src/kernel/arch/x86_64/cpu_x86_64.c` handle fatal exceptions:

#### `x86_exception_panic(vector)`

Captures `RIP`, `CS`, `RFLAGS`, `CR3`, and optionally `CR2` (for vector 14)
via inline assembly. Reports to serial and renders the framebuffer panic screen.

#### `x86_exception_panic_frame(vector, frame)`

Used when the IDT stub provides a full exception frame. The frame layout is
`[err, rip, cs, rflags, ...]`. Produces unlocked serial output in this format:

```
[cpu] exception vector=<hex16>
[cpu] err=<hex16>
[cpu] rip=<hex16>
[cpu] cs=<hex16>
[cpu] rflags=<hex16>
[cpu] cr2=<hex16>         (only for vector 14, #PF)
[cpu] frame=<hex16>
[cpu] pid=<decimal>
[cpu] name=<string>
[cpu] stack base=<hex16>
[cpu] stack top=<hex16>
[cpu] cr3=<hex16>
```

If `RIP` falls within `__kernel_start..__kernel_end`, 16 bytes at `RIP` are
also dumped via `serial_dump_bytes_unlocked`.

#### Framebuffer Panic Screen

`panic_render_screen()` renders a text-mode panic display via `framebuffer_panic_begin()` /
`framebuffer_panic_write()`:

```
KERNEL PANIC
CPU EXCEPTION <hex16>

err      <hex16>
rip      <hex16>
cs       <hex16>
rflags   <hex16>
cr2      <hex16>     (if #PF)
cr3      <hex16>
frame    <hex16>

pid      <decimal>
proc     <string>
stack_lo <hex16>
stack_hi <hex16>

ktext_lo <hex16>
ktext_hi <hex16>
fb_base  <hex16>    (if framebuffer present)
fb_size  <hex16>
fb_w/h   <w> x <h>
fb_strd  <n>

System halted.
```

After rendering, the CPU executes `cli; hlt` in an infinite loop. There is no
recovery path from a kernel panic.

---

### Page Fault and User Exception Handling

#### Page Fault Classification

`x86_page_fault_handler()` reads `CR2`, identifies the faulting process, and
classifies via `pf_classify_reason(error_code, cr2, from_user)`:

| `pf_reason_t`         | Condition                                    |
|-----------------------|----------------------------------------------|
| `user_to_kernel`      | User-mode access to kernel address           |
| `unmapped`            | Present bit clear, user access               |
| `exec_violation`      | NX (instruction-fetch) fault                 |
| `write_violation`     | Write to read-only page                      |
| `protection`          | Other protection fault                       |

The handler first calls `memory_service_handle_fault_ipc()` to give the memory
service a chance to demand-page. If that returns non-zero and the fault came
from user space:

```
[fault] user-pf pid=<n> reason=<name> err=<hex16> cr2=<hex16> rip=<hex16>
[cpu] user page fault terminate pid=<n> err=<hex16> cr2=<hex16>
```

The process is terminated with exit code `-11` and yielded.

#### User Exception Handling

`x86_user_exception_handler()` intercepts these vectors when they arrive from
ring 3: 0 (#DE), 1 (#DB), 4 (#OF), 6 (#UD), 7 (#NM), 12 (#SS), 13 (#GP), 17 (#AC).

```
[fault] user-exc pid=<n> vector=<decimal> rip=<hex16>
```

Process is terminated with exit code `-11`.

---

### Debug Hooks

#### `g_skip_wasm_boot`

Defined in `src/kernel/kernel_init_runtime.c`:

```c
static const uint8_t g_skip_wasm_boot = 0;
```

Set to `1` to bypass the full WASM boot chain and run only the wasm3 probe
(`native-call-min` module). The init process blocks on IPC instead of spawning
the process manager. Useful for isolating runtime bring-up from service startup.

#### `wasmos_debug_mark(tag)`

WASM host import (`"wasmos"/"debug_mark"`, signature `i(i)`), linked in
`wasm3_link.c`. Callable from any WASM module:

```c
extern int32_t wasmos_debug_mark(int32_t tag)
    WASMOS_WASM_IMPORT("wasmos", "debug_mark");
```

Emits (only when `WASMOS_TRACE=1`):
```
[wasm] debug_mark tag=<hex16>
[wasm] debug_mark pid=<hex16>
```

No-op in production builds.

#### `wasmos_kmap_dump()` and `wasmos_kmap_dump_all()`

WASM host imports callable from the CLI (`kmaps` / `kmaps all` commands):

```c
extern int32_t wasmos_kmap_dump(void)     WASMOS_WASM_IMPORT("wasmos", "kmap_dump");
extern int32_t wasmos_kmap_dump_all(void) WASMOS_WASM_IMPORT("wasmos", "kmap_dump_all");
```

`wasmos_kmap_dump()` dumps the calling process's user root kernel mappings via
`paging_dump_user_root_kernel_mappings()`. For ring-3 processes, also verifies
the user root has no low PML4 slot (user-to-kernel isolation check).

`wasmos_kmap_dump_all()` iterates all active processes and performs the same
dump and verification for each. Returns 0 if all root tables are clean, -1 if
any ring-3 process fails the low-slot check.

Both are unconditionally available; the output detail depends on the paging
implementation.

---

### Integration Test Framework

All integration tests use `scripts/qemu_test_framework.py` as a shared base.

#### `QemuSession`

A subprocess wrapper that starts QEMU, routes `stdin`/`stdout` via pipe, and
provides:

```python
session.send(line: str)               # write line to QEMU stdin
session.expect(needle: bytes) -> bool # scan serial output until needle found or timeout
session.force_stop()                  # send QEMU monitor command to quit
```

`expect` returns `False` on timeout (default 120 seconds). The full accumulated
serial output is available in `session.buf` for post-run assertions.

#### Default QEMU Command

```
qemu-system-x86_64
  -m 512M
  -serial mon:stdio
  -drive if=pflash,format=raw,readonly=on,file=<OVMF_CODE>
  [-drive if=pflash,format=raw,file=<OVMF_VARS>]
  -nographic
  -drive format=raw,file=fat:rw:<esp_dir>
  [-drive format=raw,file=fat:rw:<userfs_dir>]
```

OVMF path comes from `WASMOS_OVMF_CODE` env var or `CMakeCache.txt`.
ESP path comes from `WASMOS_ESP` env var or `build/esp`.

`WASMOS_QEMU_ISOLATE_ESP=1` copies the ESP to a temp directory before launch
so test-written FAT files do not persist across runs.

---

### Test Targets

Tests must NOT be run in parallel — they share `build/esp` artifacts.

| CMake target                       | Script                                        | Pass condition                                            |
|------------------------------------|-----------------------------------------------|-----------------------------------------------------------|
| `run-qemu-test`                    | `qemu_halt_test.py`                           | `wamos>` prompt appears; `halt` accepted                  |
| `run-qemu-cli-test`                | `qemu_halt_test.py`                           | Same (CLI smoke)                                          |
| `run-qemu-ring3-check`             | `qemu_ring3_halt_test.py`                     | All ring3 markers appear in order (see below)             |
| `run-qemu-ring3-threading-check`   | `qemu_ring3_halt_test.py --require-threading` | Ring3 markers + threading markers                         |
| `run-qemu-ring3-fault-storm-check` | `qemu_ring3_fault_storm_test.py`              | Fault-storm markers + ud/gp count ≥ 2 each; up to 3 tries |
| `run-qemu-ring3-test`              | wrapper                                       | ring3-check in isolated build tree                        |
| `run-qemu-ring3-threading-test`    | wrapper                                       | ring3-threading-check in isolated build tree              |
| `run-qemu-ring3-fault-storm-test`  | wrapper                                       | fault-storm-check in isolated build tree                  |
| `run-qemu-ui-test`                 | `qemu_ui_test.py`                             | Ring3 markers + native-call-smoke + UI display            |
| `run-qemu-ring3-gate`              | wrapper                                       | run-qemu-test + run-qemu-ring3-test                       |

#### Ring3 Halt Test Markers

`qemu_ring3_halt_test.py` asserts these appear in serial order before `halt`:

```
[kernel] kmain
native-call-smoke: ipc-call ok
[test] ring3 native abi ok
[test] ring3 native gettid ok
[test] ring3 thread exit syscall ok
[test] ring3 thread create syscall ok
[test] ring3 thread join syscall ok
[test] ring3 thread join self deny ok
[test] ring3 thread detach syscall ok
[test] ring3 thread detach invalid deny ok
[fault] user-pf pid=
[test] ring3 fault isolate ok
[test] ring3 fault write reason ok
[test] ring3 fault exec reason ok
[test] ring3 fault ud reason ok   (and db/gp/de/of/nm/ss/ac variants)
[test] ring3 fault exit status ok (and per-fault exit status variants)
[test] ring3 ipc syscall deny ok
[test] ring3 ipc syscall ok
[test] ring3 ipc call ok
[test] ring3 ipc call correlate ok
[test] ring3 ipc call source auth ok
[test] pm wait reply owner deny ok
[test] pm kill owner deny ok
[test] pm status owner deny ok
[test] pm spawn owner deny ok
[test] ring3 yield syscall ok
[test] ring3 syscall ok
[test] ring3 preempt stress ok
[test] ring3 shmem owner deny ok
[test] ring3 shmem grant allow ok
```

#### Fault Storm Markers

`qemu_ring3_fault_storm_test.py` requires:
- Required: `[test] ring3 fault bp exit status ok`, `[test] ring3 containment liveness ok`,
  `[test] ring3 mixed stress ok`, `[test] ring3 watchdog clean ok`, `[test] sched progress ok`
- Count: `[test] ring3 fault ud reason ok` ≥ 2 and `[test] ring3 fault gp reason ok` ≥ 2
- Forbidden: `[test] ring3 mixed stress spawn failed`, `[test] ring3 containment liveness mismatch`,
  `[watchdog] trap frame invalid cs=`
- Retries: up to 3 attempts before failure

#### Pre-Commit Gate

Per `AGENTS.md`, always run before staging changes that affect runtime behavior:

```
cmake --build build --target run-qemu-test
```

For ring-3 changes, also run:
```
cmake --build build --target run-qemu-ring3-test
```

Documentation-only changes (no runtime behavior change) do not require test execution.
