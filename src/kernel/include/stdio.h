/* stdio.h - Minimal kernel stdio declarations. printf/fprintf both route to the
 * kernel log regardless of stream; FILE and stderr exist only so third-party
 * sources compile. */
#ifndef WASMOS_STDIO_H
#define WASMOS_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* Placeholder stream type; it carries no state and is never opened or closed. */
typedef struct FILE {
    int unused;
} FILE;

extern FILE* stderr;

/* Format into a 256-byte stack buffer and emit it through klog_write, ignoring `stream`
 * entirely.  Longer output is truncated.  Returns the number of characters the formatted
 * text occupies in that buffer. */
int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);

/* Declared for third-party sources that reference it; the kernel provides no definition,
 * so calling it fails to link.  Use vsnprintf, or klog_printf for log output. */
int snprintf(char* buf, size_t size, const char* fmt, ...);

/* Format into buf, always NUL-terminating, and return the length the result WOULD have
 * had — a return of size or more means the output was truncated.  A NULL buf or a zero
 * size writes nothing and returns 0 rather than that length, so it cannot be used to
 * measure a formatted string.  Supports %c %s %d %i %u %x %X %p with the l/ll/z length
 * modifiers; width and the '0' pad flag apply to %u/%x/%X only, and there is no precision
 * or floating point. */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

#endif
