#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include "serial.h"

static int
append_char(char *buf, size_t size, int *idx, char ch)
{
    if (*idx + 1 < (int)size) {
        buf[*idx] = ch;
    }
    (*idx)++;
    return 0;
}

static int
append_str(char *buf, size_t size, int *idx, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        append_char(buf, size, idx, *s++);
    }
    return 0;
}

static int
append_uint(char *buf, size_t size, int *idx, uint64_t value, int base, int prefix)
{
    char tmp[32];
    int n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value > 0 && n < (int)sizeof(tmp)) {
            uint64_t digit = value % (uint64_t)base;
            tmp[n++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
            value /= (uint64_t)base;
        }
    }
    if (prefix && base == 16) {
        append_char(buf, size, idx, '0');
        append_char(buf, size, idx, 'x');
    }
    for (int i = n - 1; i >= 0; --i) {
        append_char(buf, size, idx, tmp[i]);
    }
    return 0;
}

static int
format_to_buffer(char *buf, size_t size, const char *fmt, va_list ap)
{
    int idx = 0;
    if (!buf || size == 0) {
        return 0;
    }
    if (!fmt) {
        buf[0] = '\0';
        return 0;
    }
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            append_char(buf, size, &idx, *p);
            continue;
        }
        ++p;
        if (*p == '%') {
            append_char(buf, size, &idx, '%');
            continue;
        }
        if (*p == 's') {
            append_str(buf, size, &idx, va_arg(ap, const char *));
            continue;
        }
        if (*p == 'd' || *p == 'i') {
            int v = va_arg(ap, int);
            if (v < 0) {
                append_char(buf, size, &idx, '-');
                append_uint(buf, size, &idx, (uint64_t)(-v), 10, 0);
            } else {
                append_uint(buf, size, &idx, (uint64_t)v, 10, 0);
            }
            continue;
        }
        if (*p == 'u') {
            unsigned int v = va_arg(ap, unsigned int);
            append_uint(buf, size, &idx, (uint64_t)v, 10, 0);
            continue;
        }
        if (*p == 'x') {
            unsigned int v = va_arg(ap, unsigned int);
            append_uint(buf, size, &idx, (uint64_t)v, 16, 0);
            continue;
        }
        if (*p == 'p') {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            append_uint(buf, size, &idx, (uint64_t)v, 16, 1);
            continue;
        }
        append_char(buf, size, &idx, '%');
        append_char(buf, size, &idx, *p);
    }
    if (idx >= (int)size) {
        buf[size - 1] = '\0';
    } else {
        buf[idx] = '\0';
    }
    return idx;
}

static FILE wasm3_stderr_instance;
FILE *stderr = &wasm3_stderr_instance;

int fprintf(FILE *stream, const char *fmt, ...)
{
    (void)stream;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int rc = format_to_buffer(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
    return rc;
}

int printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int rc = format_to_buffer(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
    return rc;
}

static unsigned long
parse_ul(const char *nptr, char **endptr, int base)
{
    if (base == 0) {
        base = 10;
    }
    unsigned long value = 0;
    const char *s = nptr;
    while (s && *s == ' ') {
        s++;
    }
    while (s && *s) {
        char c = *s;
        unsigned int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned int)(10 + (c - 'a'));
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned int)(10 + (c - 'A'));
        } else {
            break;
        }
        if (digit >= (unsigned int)base) {
            break;
        }
        value = value * (unsigned long)base + (unsigned long)digit;
        s++;
    }
    if (endptr) {
        *endptr = (char *)(uintptr_t)s;
    }
    return value;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    return parse_ul(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    return (unsigned long long)parse_ul(nptr, endptr, base);
}
