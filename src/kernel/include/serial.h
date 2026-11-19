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

/* Thread-safe kernel-log writes (acquire internal spinlock).  Each appends the text to
 * the kmsg ring (kmsg.h) and mirrors it into the shared console ring.  Once the vt has registered
 * its console ring (klog_register_ring) the text is published there and the vt puts it on serial;
 * before that, or for whatever a full ring cannot take, it is transmitted with '\n'
 * expanded to "\r\n", busy-waiting on the UART's transmit-holding register -- so a write
 * is bounded by the line rate, not instantaneous.  A NULL string is ignored.
 * serial_write_hex64 emits "0x" + 16 uppercase hex digits + newline.  serial_printf
 * formats into a 512-byte stack buffer, so longer output is truncated. */
void serial_write(const char* s);
void serial_write_hex64(uint64_t value);
void serial_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Unlocked variants — for NMI, exception and panic handlers, and for very early boot
 * before the lock is meaningful.  They always transmit directly and never use the vt's
 * console ring, whose consumer may be the thing that failed.  Otherwise the same
 * behaviour without the spinlock, so a concurrent
 * writer interleaves MID-STRING and both lines come out corrupted; preemption is still
 * disabled around the character loop.
 *
 * A diagnostic that fires on a running system is not one of those contexts, however
 * deep in the kernel it sits: an interleaved line is indistinguishable from a garbled
 * one, and it corrupts whatever the other CPU was writing, up to and including a test
 * marker the harness matches byte for byte.  g_serial_lock is a leaf lock and the
 * IRQ/preempt depths spinlock_lock touches are per-CPU counters, so the locked writers
 * are safe to nest inside a cli window or a held scheduler lock; use them anywhere a
 * fault handler is not what is running. */
void serial_write_unlocked(const char* s);
void serial_write_hex64_unlocked(uint64_t value);
void serial_printf_unlocked(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Console output of a process whose controlling text slot is `tty` (process_t.ctty):
 * published to the vt's console ring for that slot, so it appears on the slot and on
 * serial only while the vt mirrors that slot there.  `tty` 0 is kernel log, identical to
 * serial_write.  With no console ring registered, or a ring too full to take the text,
 * it is transmitted directly.  Same lock and context rules as serial_write. */
void console_write_tty(uint32_t tty, const char* s);

/* Enable the kernel-log port: a second UART at I/O base `port` that receives every
 * kernel-log write (and only those) directly from the kernel, starting with a replay of
 * the kmsg ring.  Returns 0, or WASMOS_NOENT when no UART answers there.
 * Called once at boot from the `log.serial` option. */
int32_t serial_log_port_enable(uint16_t port);

/* COM1 output for the console ring's owner (the vt writing its serial-bound slot): `len`
 * bytes, '\n' expanded to "\r\n", reaching no ring and not the kmsg ring.  Returns 0, or
 * WASMOS_ERR_KERNEL_NOT_AUTHORIZED when no console ring is registered or `context_id` did
 * not register it. */
int32_t serial_owner_write(uint32_t context_id, const char* s, uint32_t len);

/* Non-zero when the vt has consumed every record in the console ring, or when no ring is
 * registered (the kernel then transmits everything itself).  A snapshot: a write on
 * another CPU can make it stale at once.  Used before power-off, which would otherwise
 * discard what the vt has yet to write to COM1. */
int console_ring_drained(void);

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

/* Console ring (VT I/O multiplexer): the VT owns an SPSC ring (wasmos/ringbuf.h)
 * overlaid on a BUFFER_KIND_TRANSFER xfer-buffer (the same zero-copy transport the
 * socket rings use) and registers that buffer id here.  From then on serial_write and
 * console_write_tty publish their text into it as slot-tagged records
 * (VT_CONSOLE_SLOT_KLOG / a controlling slot, wasmos_driver_abi.h) instead of
 * transmitting it, and owner_context_id owns COM1 for that text (serial_owner_write).
 * owner_context_id must own the buffer and, when non-zero, notify_endpoint.
 * notify_endpoint receives the VT_IPC_KLOG_NOTIFY doorbell; 0 registers the ring
 * without one, leaving the consumer to drain on its own schedule.  Returns 0 on
 * success or a packed error (see the definition in serial.c). */
int klog_register_ring(uint32_t owner_context_id, uint32_t id, uint32_t notify_endpoint);

/* Drops the console ring registration when owner_context_id is its owner; any
 * other context changes nothing. The kernel holds the ring by physical address
 * without a pin, so the owner's teardown calls this BEFORE its transfer buffers
 * are released: afterwards console text goes straight to COM1, as before any vt
 * registered, until a successor registers a ring of its own. */
void klog_release_ring(uint32_t owner_context_id);

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

/* The kmsg ring (kmsg.h): kernel log only, LF line ends, from the first serial_write on.
 * early_log_size returns the number of bytes retained (capped at KMSG_SIZE).
 * early_log_copy copies up to len bytes starting at logical offset into dst.
 * Logical offset 0 is the oldest byte still held, so offsets shift as the ring wraps and
 * older text is overwritten.  A NULL dst, an offset past the end, or a len beyond what is
 * buffered copies only what is available (nothing, in the first two cases) and reports
 * nothing back — pair it with serial_early_log_size. */
uint32_t serial_early_log_size(void);
void serial_early_log_copy(uint8_t* dst, uint32_t offset, uint32_t len);

/* Consistent paged read of the kmsg ring by sequence number (kmsg_read), taken under the
 * serial lock with interrupts off: `dst` must be non-faulting kernel memory and `len`
 * small.  Returns the bytes copied and stores the first byte's sequence in *seq, or
 * WASMOS_INVAL for a sequence the log never reached. */
int32_t serial_kmsg_read(uint64_t* seq, uint8_t* dst, uint32_t len);

/* Kernel-log subscribers (kmsg.h): serial_kmsg_subscribe registers an endpoint the caller's
 * context owns; kmsg_poll, called from the scheduler loop, sends WASMOS_IPC_KMSG_NOTIFY to
 * each subscriber once per batch of appends and drops subscribers that have exited. */
int32_t serial_kmsg_subscribe(uint32_t context_id, uint32_t endpoint);
void kmsg_poll(void);

/* Compile-time trace gate.  Defaults to 0 (off); define it to 1 on the command line to
 * turn the trace_* macros below into real serial output. */
#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

/* Diagnostic output that compiles away entirely when WASMOS_TRACE is 0: trace_write and
 * trace_write_unlocked become `((void)0)` and trace_do drops its statement, so the
 * argument is not evaluated and must have no side effects the code depends on.
 *
 * trace_do is variadic so that a call carrying its own commas -- a serial_printf with
 * arguments -- passes through as ONE statement; a fixed single-parameter macro splits
 * such a call across parameters and fails to compile. */
#if WASMOS_TRACE
#define trace_write(s) serial_write(s)
#define trace_write_unlocked(s) serial_write_unlocked(s)
#define trace_do(...)                                                                              \
    do {                                                                                           \
        __VA_ARGS__;                                                                               \
    } while (0)
#else
#define trace_write(s) ((void)0)
#define trace_write_unlocked(s) ((void)0)
#define trace_do(...) ((void)0)
#endif

#endif
