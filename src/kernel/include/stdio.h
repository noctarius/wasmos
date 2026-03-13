#ifndef WASMOS_STDIO_H
#define WASMOS_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct FILE {
    int unused;
} FILE;

extern FILE *stderr;

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
