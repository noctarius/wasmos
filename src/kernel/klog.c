/* klog.c - Kernel log: thin printf wrapper that writes to the serial console.
 * klog_printf supports %s, %d, %u, %x, %p and width/precision basics.
 * Safe to call from any context including early boot (before scheduling). */
#include "klog.h"

#include "serial.h"

#include <stdarg.h>
#include <stdio.h>

void
klog_write(const char *s)
{
    serial_write(s);
}

void
klog_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
}
