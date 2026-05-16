#ifndef WASMOS_KLOG_H
#define WASMOS_KLOG_H

void klog_write(const char *s);
void klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
