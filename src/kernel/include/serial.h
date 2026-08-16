/* serial.h - Serial console driver and debug output interface.
 *
 * Provides two output modes: locked (safe from any context) and unlocked
 * (for ISR/NMI paths where spinlock acquisition would deadlock).
 * A pluggable serial_driver_t lets the virtio-serial or chardev driver
 * replace the default COM1 UART backend at runtime. */
#ifndef WASMOS_SERIAL_H
#define WASMOS_SERIAL_H

#include <stdint.h>

/* Pluggable backend; default is the COM1 x86 UART.  Of the three hooks only `init` is
 * reached through the vtable: the transmit and receive paths call COM1 directly, because
 * both run in contexts where dereferencing a low-address global is unsafe.  read_char
 * returns 1 and fills *out_char when a byte was available, 0 when none was, -1 on a NULL
 * argument. */
typedef struct serial_driver {
    void (*init)(void);
    void (*put_char)(char c);
    int (*read_char)(uint8_t* out_char);
} serial_driver_t;

/* Program the active backend (COM1: 115200 8N1, FIFO on) and create the console ring.
 * Safe to call before paging; the ring creation needs the shared-memory allocator. */
void serial_init(void);

/* When enabled, adjust data pointers from physical to kernel higher-half VAs
 * (needed after paging is active but before the higher-half alias is set up).
 * Rebasing applies only to pointers that land inside the kernel image at a low VA;
 * anything else is written as given.  Non-zero enables, 0 disables. */
void serial_enable_high_alias(uint8_t enabled);
uint8_t serial_high_alias_enabled(void);

/* Thread-safe writes (acquire internal spinlock).  Each mirrors the text into the early
 * log ring, the shared console ring, and the klog xfer-buffer ring, expands '\n' to
 * "\r\n" on the wire, and busy-waits on the UART's transmit-holding register — so a write
 * is bounded by the line rate, not instantaneous.  A NULL string is ignored.
 * serial_write_hex64 emits "0x" + 16 uppercase hex digits + newline.  serial_printf
 * formats into a 512-byte stack buffer, so longer output is truncated. */
void serial_write(const char* s);
void serial_write_hex64(uint64_t value);
void serial_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Unlocked variants — use only in ISR / NMI / very early boot.  Same behaviour without
 * the spinlock, so concurrent writers interleave; preemption is still disabled around
 * the character loop. */
void serial_write_unlocked(const char* s);
void serial_write_hex64_unlocked(uint64_t value);
void serial_printf_unlocked(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Read one console byte, preferring a registered remote serial driver and falling back to
 * a direct COM1 poll.  Returns 1 with the byte in *out_char, 0 when nothing is pending, or
 * -1 on a NULL argument.  Non-blocking. */
int serial_read_char(uint8_t* out_char);

/* Replace the active serial backend; returns the previous driver.  A NULL argument
 * restores the built-in COM1 driver rather than clearing the slot, so the getter never
 * returns NULL.  The vtable is borrowed, not copied, and must outlive its installation. */
const serial_driver_t* serial_set_driver(const serial_driver_t* driver);
const serial_driver_t* serial_get_driver(void);

/* Point the console's READ path at a user-space serial driver reachable at `endpoint`,
 * creating the kernel-owned reply endpoint on first use and clearing any in-flight read
 * request.  The transmit path is unaffected and keeps writing COM1 directly.  Returns 0
 * on success, -1 when the endpoint is IPC_ENDPOINT_NONE, has no owner, or is owned by
 * the kernel itself, and -1 when the reply endpoint cannot be created.  The link is
 * dropped again automatically on a fatal IPC error. */
int serial_register_remote_driver(uint32_t endpoint);

/* Shared-memory id of the one-page console ring the framebuffer driver drains, creating
 * it on first use.  Returns 0 when creation fails — indistinguishable from "not created
 * yet"; a later call retries. */
uint32_t serial_console_ring_id(void);

/* The console ring as the raw value mm_shared_create handed back: a PHYSICAL base, not
 * rebased onto the kernel higher-half alias even when that alias is armed.  A caller that
 * dereferences it must rebase it itself or run under a root that still identity-maps low
 * memory.  NULL when the ring could not be created. */
void* serial_console_ring_ptr(void);

/* klog ring (VT I/O multiplexer): the VT owns an SPSC byte ring
 * (wasmos/ringbuf.h) overlaid on a BUFFER_KIND_TRANSFER xfer-buffer (the same
 * zero-copy transport the socket rings use) and registers that buffer id here.
 * serial_write then additionally publishes klog text into the ring for the VT
 * to drain into vt-1.  This is additive to the legacy console_ring (the
 * framebuffer driver still drains console_ring for early-boot on-screen klog).
 * owner_context_id must own the buffer and, when non-zero, notify_endpoint.
 * notify_endpoint receives the VT_IPC_KLOG_NOTIFY doorbell; 0 registers the ring
 * without one, leaving the consumer to drain on its own schedule.  Returns 0 on
 * success, -1 on a bad/foreign id, a foreign notify endpoint, or a region that is
 * not an initialized ring. */
int klog_register_ring(uint32_t owner_context_id, uint32_t id, uint32_t notify_endpoint);

/* Deliver a pending klog doorbell, if any.  Runs from the scheduler loop, not
 * from the logging path: serial_write can run under a lock, in interrupt context
 * and during a panic, so it may not send IPC.  A no-op when no ring, no doorbell
 * endpoint, or nothing pending. */
void klog_poll(void);
/* Keyboard input ring, filled by the VT and drained by the wasmos_input_read host call.
 * It is independent of serial_read_char: a reader sees only pushed keystrokes and never
 * falls back to COM1.  Push appends to a 64-entry ring and drops the byte silently when
 * it is full — keystrokes are advisory and blocking would stall the pushing driver.
 * Read pops the oldest byte, returning 1 with it in *out or 0 when the ring is empty;
 * it never blocks and dereferences `out` without a NULL check whenever a byte is
 * available.  Both take the same lock as serial_write, so neither may run from a context
 * that can interrupt a locked writer on the same CPU. */
void serial_input_push(uint8_t ch);
int serial_input_read(uint8_t* out);

/* Early log ring buffer — captured from the first serial_write onward.
 * Returns the number of bytes currently buffered (capped at ring size).
 * early_log_copy copies up to len bytes starting at logical offset into dst.
 * Logical offset 0 is the oldest byte still held, so offsets shift as the ring wraps and
 * older text is overwritten.  A NULL dst, an offset past the end, or a len beyond what is
 * buffered copies only what is available (nothing, in the first two cases) and reports
 * nothing back — pair it with serial_early_log_size. */
uint32_t serial_early_log_size(void);
void serial_early_log_copy(uint8_t* dst, uint32_t offset, uint32_t len);

/* Compile-time trace gate.  Defaults to 0 (off); define it to 1 on the command line to
 * turn the trace_* macros below into real serial output. */
#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

/* Diagnostic output that compiles away entirely when WASMOS_TRACE is 0: trace_write and
 * trace_write_unlocked become `((void)0)` and trace_do drops its statement, so the
 * argument is not evaluated and must have no side effects the code depends on. */
#if WASMOS_TRACE
#define trace_write(s) serial_write(s)
#define trace_write_unlocked(s) serial_write_unlocked(s)
#define trace_do(stmt)                                                                             \
    do {                                                                                           \
        stmt;                                                                                      \
    } while (0)
#else
#define trace_write(s) ((void)0)
#define trace_write_unlocked(s) ((void)0)
#define trace_do(stmt) ((void)0)
#endif

#endif
