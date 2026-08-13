/* stdio.h - Minimal kernel stdio declarations. printf/fprintf both route to the
 * kernel log regardless of stream; FILE and stderr exist only so third-party
 * sources compile. */
#ifndef WASMOS_STDIO_H
#define WASMOS_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct FILE {
    int unused;
} FILE;

extern FILE* stderr;

int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

#endif
