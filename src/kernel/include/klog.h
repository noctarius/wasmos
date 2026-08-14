/* klog.h - Kernel log. Every write fans out to COM1, the shared-memory console
 * ring user space reads, the VT-owned klog ring once one is registered, and the
 * in-kernel early-log ring.  Safe to call before paging and scheduling are
 * active. */
#ifndef WASMOS_KLOG_H
#define WASMOS_KLOG_H

/* Write a NUL-terminated string to the kernel log. No newline is appended.  Takes the
 * serial spinlock and busy-waits on the UART, so it is not free and must not be called
 * from a context that can interrupt a CPU already holding that lock — use the
 * serial_*_unlocked family from an ISR or NMI.  A NULL string is ignored. */
void klog_write(const char* s);

/* printf-style kernel log; subset of format specifiers (no floating point).
 * The formatted line is truncated to 512 bytes.  Supports %c %s %d %i %u %x %X %p, the
 * l/ll/z length modifiers and zero-padded field widths; there is no precision support.
 * Formats onto the caller's stack, so it needs ~512 bytes of headroom. */
void klog_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
