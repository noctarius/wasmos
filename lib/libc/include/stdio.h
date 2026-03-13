#ifndef WASMOS_LIBC_STDIO_H
#define WASMOS_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int putsn(const char *s, size_t len);
int puts(const char *s);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vprintf(const char *format, va_list args);
int printf(const char *format, ...);

#endif
