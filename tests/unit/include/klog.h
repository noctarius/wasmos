/* Host shadow of src/kernel/include/klog.h.
 *
 * Kernel sources under test call klog_write/klog_printf, and the real
 * implementation fans out to COM1, the console ring and the VT ring -- none of
 * which exist in a host process. These discard their arguments instead, and are
 * static inline so no stub object has to be linked.
 *
 * Two consequences for a test built against this shadow. Nothing a kernel source
 * logs is observable, so a suite cannot assert on log content. And klog_printf
 * here carries no __attribute__((format(printf, 1, 2))), which the real
 * declaration does: a format-string/argument mismatch in a kernel source under
 * test is diagnosed by the target build but not by the host one. The real
 * klog_printf also truncates a formatted line at 512 bytes; with the arguments
 * discarded there is no length behaviour to model. */
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
