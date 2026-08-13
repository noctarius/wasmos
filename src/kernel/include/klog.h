/* klog.h - Kernel log. Every write fans out to COM1, the shared-memory console
 * ring user space reads, the VT-owned klog ring once one is registered, and the
 * in-kernel early-log ring.  Safe to call before paging and scheduling are
 * active. */
#ifndef WASMOS_KLOG_H
#define WASMOS_KLOG_H

/* Write a NUL-terminated string to the kernel log. No newline is appended. */
void klog_write(const char* s);

/* printf-style kernel log; subset of format specifiers (no floating point).
 * The formatted line is truncated to 512 bytes. */
void klog_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
