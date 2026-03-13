#include <stdarg.h>
#include <stdint.h>
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "serial.h"

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; ++i) {
        d[i] = v;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return (pa[i] < pb[i]) ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    if (!s) {
        return 0;
    }
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

int strcmp(const char *a, const char *b) {
    if (!a && !b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    while (*a && *b) {
        if (*a != *b) {
            return (*a < *b) ? -1 : 1;
        }
        a++;
        b++;
    }
    if (*a == *b) {
        return 0;
    }
    return (*a < *b) ? -1 : 1;
}

static size_t append_char(char *buf, size_t size, size_t pos, char ch) {
    if (pos + 1 < size) {
        buf[pos] = ch;
        buf[pos + 1] = '\0';
    }
    return pos + 1;
}

static size_t append_str(char *buf, size_t size, size_t pos, const char *s) {
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        pos = append_char(buf, size, pos, *s++);
    }
    return pos;
}

static size_t append_u64(char *buf, size_t size, size_t pos, uint64_t value, uint32_t base, int uppercase) {
    char tmp[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int idx = 0;
    if (value == 0) {
        tmp[idx++] = '0';
    } else {
        while (value > 0 && idx < (int)sizeof(tmp)) {
            tmp[idx++] = digits[value % base];
            value /= base;
        }
    }
    while (idx > 0) {
        pos = append_char(buf, size, pos, tmp[--idx]);
    }
    return pos;
}

static size_t append_i64(char *buf, size_t size, size_t pos, int64_t value) {
    if (value < 0) {
        pos = append_char(buf, size, pos, '-');
        return append_u64(buf, size, pos, (uint64_t)(-value), 10, 0);
    }
    return append_u64(buf, size, pos, (uint64_t)value, 10, 0);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (!buf || size == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t pos = 0;
    for (const char *p = fmt; p && *p; ++p) {
        if (*p != '%') {
            pos = append_char(buf, size, pos, *p);
            continue;
        }
        p++;
        if (*p == '\0') {
            break;
        }
        switch (*p) {
        case '%':
            pos = append_char(buf, size, pos, '%');
            break;
        case 'c': {
            int ch = va_arg(ap, int);
            pos = append_char(buf, size, pos, (char)ch);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            pos = append_str(buf, size, pos, s);
            break;
        }
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            pos = append_i64(buf, size, pos, (int64_t)v);
            break;
        }
        case 'u': {
            unsigned int v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, (uint64_t)v, 10, 0);
            break;
        }
        case 'x': {
            unsigned int v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, (uint64_t)v, 16, 0);
            break;
        }
        case 'X': {
            unsigned int v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, (uint64_t)v, 16, 1);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            pos = append_str(buf, size, pos, "0x");
            pos = append_u64(buf, size, pos, (uint64_t)v, 16, 0);
            break;
        }
        default:
            pos = append_char(buf, size, pos, '%');
            pos = append_char(buf, size, pos, *p);
            break;
        }
    }
    return (int)pos;
}

void abort(void) {
    serial_write("[kernel] abort\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
