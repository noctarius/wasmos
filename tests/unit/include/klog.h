#ifndef WASMOS_TEST_KLOG_H
#define WASMOS_TEST_KLOG_H

#include <stdarg.h>

static inline void klog_write(const char *msg) {
    (void)msg;
}

static inline void klog_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)fmt;
    va_end(ap);
}

#endif
