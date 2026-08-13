/* Host shadow of src/kernel/include/klog.h.
 *
 * Kernel sources under test call klog_write/klog_printf, and the real
 * implementation fans out to COM1, the console ring and the VT ring -- none of
 * which exist in a host process. These discard their arguments instead, and are
 * static inline so no stub object has to be linked. */
#ifndef WASMOS_TEST_KLOG_H
#define WASMOS_TEST_KLOG_H

#include <stdarg.h>

static inline void klog_write(const char* msg) {
    (void)msg;
}

static inline void klog_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)fmt;
    va_end(ap);
}

#endif
