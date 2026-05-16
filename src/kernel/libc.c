#include <stdarg.h>
#include <stdint.h>
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "serial.h"
#include "paging.h"

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static inline const char *
kernel_str_ptr(const char *s)
{
    uintptr_t p = (uintptr_t)s;
    uint64_t base = KERNEL_HIGHER_HALF_BASE;
    if (serial_high_alias_enabled() && p != 0 && (uint64_t)p < base) {
        uint64_t start = (uint64_t)(uintptr_t)&__kernel_start;
        uint64_t end = (uint64_t)(uintptr_t)&__kernel_end;
        uint64_t low_start = start - base;
        uint64_t low_end = end - base;
        if ((uint64_t)p >= low_start && (uint64_t)p < low_end) {
            p = (uintptr_t)((uint64_t)p + base);
        }
    }
    return (const char *)p;
}

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

size_t strnlen(const char *s, size_t max_len) {
    if (!s) {
        return 0;
    }
    s = kernel_str_ptr(s);
    size_t len = 0;
    while (len < max_len && s[len]) {
        len++;
    }
    return len;
}

int strcmp(const char *a, const char *b) {
    a = kernel_str_ptr(a);
    b = kernel_str_ptr(b);
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

int strncmp(const char *a, const char *b, size_t n) {
    if (n == 0 || a == b) {
        return 0;
    }
    a = kernel_str_ptr(a);
    b = kernel_str_ptr(b);
    if (!a && !b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb || ca == '\0' || cb == '\0') {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    if (!dst || !src) {
        return dst;
    }
    src = kernel_str_ptr(src);
    size_t i = 0;
    do {
        dst[i] = src[i];
    } while (src[i++] != '\0');
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    if (!dst || !src) {
        return dst;
    }
    src = kernel_str_ptr(src);
    size_t i = 0;
    while (i < n && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i++] = '\0';
    }
    return dst;
}

char *strchr(const char *s, int ch) {
    if (!s) {
        return 0;
    }
    s = kernel_str_ptr(s);
    char needle = (char)ch;
    for (;;) {
        if (*s == needle) {
            return (char *)s;
        }
        if (*s == '\0') {
            break;
        }
        s++;
    }
    return 0;
}

char *strrchr(const char *s, int ch) {
    const char *last = 0;
    if (!s) {
        return 0;
    }
    s = kernel_str_ptr(s);
    char needle = (char)ch;
    for (;;) {
        if (*s == needle) {
            last = s;
        }
        if (*s == '\0') {
            break;
        }
        s++;
    }
    return (char *)last;
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
    s = kernel_str_ptr(s);
    while (*s) {
        pos = append_char(buf, size, pos, *s++);
    }
    return pos;
}

static size_t append_u64(char *buf, size_t size, size_t pos,
                          uint64_t value, uint32_t base, int uppercase,
                          int width, char pad) {
    char tmp[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    digits = kernel_str_ptr(digits);
    int idx = 0;
    if (value == 0) {
        tmp[idx++] = '0';
    } else {
        while (value > 0 && idx < (int)sizeof(tmp)) {
            tmp[idx++] = digits[value % base];
            value /= base;
        }
    }
    while (idx < width && idx < (int)sizeof(tmp)) {
        tmp[idx++] = pad;
    }
    while (idx > 0) {
        pos = append_char(buf, size, pos, tmp[--idx]);
    }
    return pos;
}

static size_t append_i64(char *buf, size_t size, size_t pos, int64_t value) {
    if (value < 0) {
        pos = append_char(buf, size, pos, '-');
        return append_u64(buf, size, pos, (uint64_t)(-value), 10, 0, 0, ' ');
    }
    return append_u64(buf, size, pos, (uint64_t)value, 10, 0, 0, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (!buf || size == 0) {
        return 0;
    }
    buf[0] = '\0';
    fmt = kernel_str_ptr(fmt);
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
        if (*p == '%') {
            pos = append_char(buf, size, pos, '%');
            continue;
        }

        /* Flags: only '0' for now. */
        char pad = ' ';
        if (*p == '0') { pad = '0'; p++; }

        /* Width. */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* Length modifier: l, ll, z. */
        int lmod = 0; /* 0=int, 1=long, 2=long long / size_t */
        if (*p == 'l') {
            lmod = 1; p++;
            if (*p == 'l') { lmod = 2; p++; }
        } else if (*p == 'z') {
            lmod = 2; p++;
        }

        switch (*p) {
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
            int64_t v;
            if (lmod == 2)      v = va_arg(ap, long long);
            else if (lmod == 1) v = va_arg(ap, long);
            else                v = va_arg(ap, int);
            pos = append_i64(buf, size, pos, v);
            break;
        }
        case 'u': {
            uint64_t v;
            if (lmod == 2)      v = va_arg(ap, unsigned long long);
            else if (lmod == 1) v = va_arg(ap, unsigned long);
            else                v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, v, 10, 0, width, pad);
            break;
        }
        case 'x': {
            uint64_t v;
            if (lmod == 2)      v = va_arg(ap, unsigned long long);
            else if (lmod == 1) v = va_arg(ap, unsigned long);
            else                v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, v, 16, 0, width, pad);
            break;
        }
        case 'X': {
            uint64_t v;
            if (lmod == 2)      v = va_arg(ap, unsigned long long);
            else if (lmod == 1) v = va_arg(ap, unsigned long);
            else                v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, v, 16, 1, width, pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            pos = append_str(buf, size, pos, "0x");
            pos = append_u64(buf, size, pos, (uint64_t)v, 16, 0, 0, ' ');
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

int tolower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

int toupper(int ch) {
    if (ch >= 'a' && ch <= 'z') {
        return ch - ('a' - 'A');
    }
    return ch;
}

int isspace(int ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' ||
           ch == '\r' || ch == '\f' || ch == '\v';
}

int isdigit(int ch) {
    return ch >= '0' && ch <= '9';
}

int isalpha(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

int isalnum(int ch) {
    return isalpha(ch) || isdigit(ch);
}

int isxdigit(int ch) {
    return isdigit(ch) ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}
