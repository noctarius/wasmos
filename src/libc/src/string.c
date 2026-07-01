/* string.c - minimal C string library for WASM user space */
#include "string.h"
#include "ctype.h"
#include <stdint.h>

/* Copy 8 bytes at once using uint64_t to hint at wider loads/stores. */
static inline void
copy8_forward(unsigned char *out, const unsigned char *in)
{
    uint64_t v;
    __builtin_memcpy(&v, in, sizeof(v));
    __builtin_memcpy(out, &v, sizeof(v));
}

static inline void
copy8_backward(unsigned char *out, const unsigned char *in)
{
    uint64_t v;
    __builtin_memcpy(&v, in, sizeof(v));
    __builtin_memcpy(out, &v, sizeof(v));
}

size_t
strlen(const char *s)
{
    size_t len = 0;

    if (!s) {
        return 0;
    }
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

size_t
strnlen(const char *s, size_t max_len)
{
    size_t len = 0;

    if (!s) {
        return 0;
    }
    while (len < max_len && s[len] != '\0') {
        len++;
    }
    return len;
}

int
strcmp(const char *lhs, const char *rhs)
{
    size_t i = 0;

    if (lhs == rhs) {
        return 0;
    }
    if (!lhs) {
        return -1;
    }
    if (!rhs) {
        return 1;
    }
    while (lhs[i] && rhs[i]) {
        if (lhs[i] != rhs[i]) {
            return (int)(unsigned char)lhs[i] - (int)(unsigned char)rhs[i];
        }
        i++;
    }
    return (int)(unsigned char)lhs[i] - (int)(unsigned char)rhs[i];
}

int
strncmp(const char *lhs, const char *rhs, size_t count)
{
    if (count == 0 || lhs == rhs) {
        return 0;
    }
    if (!lhs) {
        return -1;
    }
    if (!rhs) {
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (lhs[i] != rhs[i] || lhs[i] == '\0' || rhs[i] == '\0') {
            return (int)(unsigned char)lhs[i] - (int)(unsigned char)rhs[i];
        }
    }
    return 0;
}

int
strcasecmp(const char *lhs, const char *rhs)
{
    size_t i = 0;

    if (lhs == rhs) {
        return 0;
    }
    if (!lhs) {
        return -1;
    }
    if (!rhs) {
        return 1;
    }
    for (;;) {
        unsigned char a = (unsigned char)tolower((unsigned char)lhs[i]);
        unsigned char b = (unsigned char)tolower((unsigned char)rhs[i]);
        if (a != b || a == '\0' || b == '\0') {
            return (int)a - (int)b;
        }
        i++;
    }
}

char *
strcpy(char *dest, const char *src)
{
    size_t i = 0;

    if (!dest || !src) {
        return dest;
    }
    do {
        dest[i] = src[i];
    } while (src[i++] != '\0');
    return dest;
}

char *
strncpy(char *dest, const char *src, size_t count)
{
    size_t i = 0;

    if (!dest || !src) {
        return dest;
    }
    while (i < count && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    while (i < count) {
        dest[i++] = '\0';
    }
    return dest;
}

char *
strchr(const char *s, int ch)
{
    char needle = (char)ch;

    if (!s) {
        return 0;
    }
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

char *
strrchr(const char *s, int ch)
{
    const char *last = 0;
    char needle = (char)ch;

    if (!s) {
        return 0;
    }
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

void *
memcpy(void *dest, const void *src, size_t count)
{
    unsigned char *out = (unsigned char *)dest;
    const unsigned char *in = (const unsigned char *)src;

    if (!dest || !src) {
        return dest;
    }
    /* TODO: memcpy remains intentionally non-overlap-safe; use memmove when ranges can overlap. */
    while (count >= 32) {
        copy8_forward(out, in);
        copy8_forward(out + 8, in + 8);
        copy8_forward(out + 16, in + 16);
        copy8_forward(out + 24, in + 24);
        out += 32;
        in += 32;
        count -= 32;
    }
    while (count >= 8) {
        copy8_forward(out, in);
        out += 8;
        in += 8;
        count -= 8;
    }
    while (count > 0) {
        *out++ = *in++;
        count--;
    }
    return dest;
}

void *
memmove(void *dest, const void *src, size_t count)
{
    unsigned char *out = (unsigned char *)dest;
    const unsigned char *in = (const unsigned char *)src;

    if (!dest || !src || dest == src || count == 0) {
        return dest;
    }
    if (out < in || (size_t)(out - in) >= count) {
        while (count >= 32) {
            copy8_forward(out, in);
            copy8_forward(out + 8, in + 8);
            copy8_forward(out + 16, in + 16);
            copy8_forward(out + 24, in + 24);
            out += 32;
            in += 32;
            count -= 32;
        }
        while (count >= 8) {
            copy8_forward(out, in);
            out += 8;
            in += 8;
            count -= 8;
        }
        while (count > 0) {
            *out++ = *in++;
            count--;
        }
    } else {
        out += count;
        in += count;
        while (count >= 32) {
            out -= 32;
            in -= 32;
            copy8_backward(out + 24, in + 24);
            copy8_backward(out + 16, in + 16);
            copy8_backward(out + 8, in + 8);
            copy8_backward(out, in);
            count -= 32;
        }
        while (count >= 8) {
            out -= 8;
            in -= 8;
            copy8_backward(out, in);
            count -= 8;
        }
        while (count > 0) {
            out--;
            in--;
            *out = *in;
            count--;
        }
    }
    return dest;
}

void *
memset(void *dest, int value, size_t count)
{
    /* Word-optimized: fill 8 bytes at a time (with an unrolled 32-byte stride)
     * after aligning to 8.  This compiles to wasm32 (no x86 asm / hostcall) and
     * is ~8x fewer stores than a byte loop.  (The kernel's native memset uses
     * rep stosb; wasm can't, so this is the portable equivalent.) */
    if (!dest) {
        return dest;
    }
    uint8_t *d = (uint8_t *)dest;
    uint8_t v8 = (uint8_t)value;

    uint64_t v64 = v8;
    v64 |= v64 << 8;
    v64 |= v64 << 16;
    v64 |= v64 << 32;

    while (count && ((uintptr_t)d & 7u)) {   /* align to 8 */
        *d++ = v8;
        count--;
    }
    uint64_t *q = (uint64_t *)d;
    while (count >= 32) {
        q[0] = v64; q[1] = v64; q[2] = v64; q[3] = v64;
        q += 4;
        count -= 32;
    }
    while (count >= 8) {
        *q++ = v64;
        count -= 8;
    }
    d = (uint8_t *)q;
    while (count--) {
        *d++ = v8;
    }
    return dest;
}

int
memcmp(const void *lhs, const void *rhs, size_t count)
{
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;

    if (lhs == rhs || count == 0) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    return 0;
}
