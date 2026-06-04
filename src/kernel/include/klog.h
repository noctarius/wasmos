/* klog.h - Kernel log: writes to serial COM1 and a ring buffer visible in
 * the QEMU monitor.  Safe to call before paging and scheduling are active. */
#ifndef WASMOS_KLOG_H
#define WASMOS_KLOG_H

/* Write a literal string to the kernel log. */
void klog_write(const char *s);

/* printf-style kernel log; subset of format specifiers (no floating point). */
void klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
