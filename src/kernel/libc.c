/* libc.c - Minimal freestanding C library functions for the kernel.
 * Provides the memory and string families (memcpy, memset, memmove, strlen,
 * strcmp, ...), vsnprintf, and the ctype predicates.  Declared snprintf has no
 * definition here.  Kept deliberately simple; only memset and the 8-byte copy
 * loops are tuned. */
#include <stdarg.h>
#include "klog.h"
#include <stdint.h>
#include "string.h"
#include "ctype.h"
#include "kpanic.h"
#include "stdio.h"
#include "stdlib.h"
#include "serial.h"
#include "paging.h"

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

/* Rebase a low-VA pointer into the kernel image (typically a string literal
 * reached through the identity map) onto the higher-half alias, so the string
 * functions stay readable under a root without a low identity mapping.  Any
 * other pointer, including caller buffers, is returned untouched. */
static inline const char* kernel_str_ptr(const char* s) {
    uintptr_t p = (uintptr_t)s;
    uint64_t base = KERNEL_HIGHER_HALF_BASE;
    if (serial_high_alias_enabled() && p != 0 && (uint64_t)p < base) {
        uint64_t start = addr_cast(uint64_t, &__kernel_start);
        uint64_t end = addr_cast(uint64_t, &__kernel_end);
        uint64_t low_start = start - base;
        uint64_t low_end = end - base;
        if ((uint64_t)p >= low_start && (uint64_t)p < low_end) {
            p = (uintptr_t)((uint64_t)p + base);
        }
    }
    return (const char*)p;
}

static inline void copy8_forward(uint8_t* d, const uint8_t* s) {
    uint64_t v;
    __builtin_memcpy(&v, s, sizeof(v));
    __builtin_memcpy(d, &v, sizeof(v));
}

static inline void copy8_backward(uint8_t* d, const uint8_t* s) {
    uint64_t v;
    __builtin_memcpy(&v, s, sizeof(v));
    __builtin_memcpy(d, &v, sizeof(v));
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    /* Copies strictly forward and is NOT overlap-safe; use memmove when the
     * ranges can overlap. */
    while (n >= 32) {
        copy8_forward(d, s);
        copy8_forward(d + 8, s + 8);
        copy8_forward(d + 16, s + 16);
        copy8_forward(d + 24, s + 24);
        d += 32;
        s += 32;
        n -= 32;
    }
    while (n >= 8) {
        copy8_forward(d, s);
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n > 0) {
        *d++ = *s++;
        n--;
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    /* rep stosb: correct on every x86 CPU and microcode-accelerated (ERMS) on
     * anything since ~2012, so no CPUID gate is needed.  `cld` forces forward
     * direction independent of the caller's DF, since kernel memset runs in
     * arbitrary contexts (boot, ISRs). */
    void* ret = dst;
    __asm__ __volatile__("cld\n\t"
                         "rep stosb"
                         : "+D"(dst), "+c"(n)
                         : "a"((uint8_t)c)
                         : "memory");
    return ret;
}

/* Overlap-safe copy: it copies backward only when dst falls strictly inside
 * [src, src+n), and forward otherwise, so both directions get the same 8-byte
 * chunking as memcpy.  Identical pointers and a zero n return immediately.  The
 * chunked loads and stores are unaligned by construction. */
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s || (size_t)(d - s) >= n) {
        while (n >= 32) {
            copy8_forward(d, s);
            copy8_forward(d + 8, s + 8);
            copy8_forward(d + 16, s + 16);
            copy8_forward(d + 24, s + 24);
            d += 32;
            s += 32;
            n -= 32;
        }
        while (n >= 8) {
            copy8_forward(d, s);
            d += 8;
            s += 8;
            n -= 8;
        }
        while (n > 0) {
            *d++ = *s++;
            n--;
        }
    } else {
        d += n;
        s += n;
        while (n >= 32) {
            d -= 32;
            s -= 32;
            copy8_backward(d, s);
            copy8_backward(d + 8, s + 8);
            copy8_backward(d + 16, s + 16);
            copy8_backward(d + 24, s + 24);
            n -= 32;
        }
        while (n >= 8) {
            d -= 8;
            s -= 8;
            copy8_backward(d, s);
            n -= 8;
        }
        while (n > 0) {
            d--;
            s--;
            *d = *s;
            n--;
        }
    }
    return dst;
}

/* Returns exactly -1, 0 or 1 from the first differing byte compared as unsigned,
 * not the byte difference C permits.  Neither pointer is NULL-checked or rebased
 * through kernel_str_ptr; a zero n compares equal without any dereference. */
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return (pa[i] < pb[i]) ? -1 : 1;
        }
    }
    return 0;
}

/* NULL yields 0 instead of faulting, which is the house convention across this
 * file.  Unlike strnlen and the compare/copy family, this does NOT apply
 * kernel_str_ptr: s is read exactly as given, so a low-VA string literal needs a
 * root that still identity-maps low memory. */
size_t strlen(const char* s) {
    if (!s) {
        return 0;
    }
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/* Never reads past max_len bytes, so an unterminated buffer returns max_len.
 * NULL yields 0.  Rebases s through kernel_str_ptr. */
size_t strnlen(const char* s, size_t max_len) {
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

/* Returns -1, 0 or 1 — normalised, unlike strncmp and strcasecmp below, which
 * return the character difference.  Compares as signed char, so bytes >= 0x80
 * order below ASCII.  NULL is accepted and sorts before any string; two NULLs
 * compare equal.  Both sides are rebased through kernel_str_ptr. */
int strcmp(const char* a, const char* b) {
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

/* Compares at most n bytes as UNSIGNED char and returns their difference, not a
 * normalised -1/0/1 as strcmp does.  A zero n, or identical pointers, compares
 * equal without dereferencing either side; otherwise NULL is accepted and sorts
 * first.  Both sides are rebased through kernel_str_ptr. */
int strncmp(const char* a, const char* b, size_t n) {
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

/* Case-insensitive over ASCII A-Z only — tolower has no locale and leaves every
 * other byte alone.  Returns the difference of the folded unsigned bytes, like
 * strncmp.  Unbounded: it runs to the first NUL on either side. */
int strcasecmp(const char* a, const char* b) {
    if (a == b) {
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
    for (;;) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb || ca == '\0' || cb == '\0') {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }
}

/* Unbounded copy including the terminator; dst must already be large enough.
 * Returns dst in every case, so the return value does not distinguish a copy
 * from the NULL-argument no-op — prefer str_copy, which bounds the write. */
char* strcpy(char* dst, const char* src) {
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

/* Standard strncpy semantics, with the standard trap: it writes exactly n bytes,
 * NUL-padding a short source, and does NOT terminate when src is n bytes or
 * longer.  Returns dst unconditionally.  str_copy is the truncate-and-terminate
 * alternative. */
char* strncpy(char* dst, const char* src, size_t n) {
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

/* Turns a counted byte range into a NUL-terminated string, REFUSING rather than
 * truncating when it would not fit — the counterpart to str_copy, which
 * truncates.  src is raw bytes and need not be terminated; embedded NULs are
 * copied through unchanged.
 *
 * Returns 0 on success and -1 on refusal: a NULL argument, a zero dst_len, a
 * zero src_len, or src_len >= dst_len (the terminator needs the last byte).  On
 * refusal dst is left untouched, so it is not made a valid string. */
int str_copy_bytes(char* dst, size_t dst_len, const uint8_t* src, size_t src_len) {
    if (!dst || !src || dst_len == 0 || src_len == 0 || src_len >= dst_len) {
        return -1;
    }
    for (size_t i = 0; i < src_len; ++i) {
        dst[i] = (char)src[i];
    }
    dst[src_len] = '\0';
    return 0;
}

/* Truncating C-string copy shared by the service/subsystem registries and the
 * app loader; mirrors the userspace libc str_copy.  Always NUL-terminates and
 * returns the number of bytes written, excluding the terminator.  Reads src
 * directly, with no kernel_str_ptr translation. */
size_t str_copy(char* dst, size_t dst_len, const char* src) {
    size_t i = 0;
    if (!dst || dst_len == 0) {
        return 0;
    }
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    while (i + 1 < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

/* Compares an unterminated byte range against a C string literal.  Note the
 * INVERTED polarity relative to strcmp: 1 means equal, 0 means different.
 * Equality requires the lengths to match exactly, so a prefix is not a match, and
 * a NUL inside `bytes` before bytes_len makes it differ.  Either pointer NULL is
 * reported as different.  lit is rebased through kernel_str_ptr. */
int str_eq_bytes(const uint8_t* bytes, size_t bytes_len, const char* lit) {
    size_t i = 0;
    if (!bytes || !lit) {
        return 0;
    }
    lit = kernel_str_ptr(lit);
    while (lit[i]) {
        if (i >= bytes_len || bytes[i] != (const uint8_t)lit[i]) {
            return 0;
        }
        i++;
    }
    return i == bytes_len;
}

/* Returns a pointer INTO s (rebased through kernel_str_ptr, so not necessarily
 * equal to the caller's s plus an offset) at the first occurrence of ch, or 0
 * when absent.  ch is truncated to char, and searching for '\0' finds the
 * terminator, as the standard requires. */
char* strchr(const char* s, int ch) {
    if (!s) {
        return 0;
    }
    s = kernel_str_ptr(s);
    char needle = (char)ch;
    for (;;) {
        if (*s == needle) {
            return (char*)s;
        }
        if (*s == '\0') {
            break;
        }
        s++;
    }
    return 0;
}

/* As strchr but returns the LAST occurrence.  Same rebasing caveat and same
 * '\0' behaviour. */
char* strrchr(const char* s, int ch) {
    const char* last = 0;
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
    return (char*)last;
}

static size_t append_char(char* buf, size_t size, size_t pos, char ch) {
    if (pos + 1 < size) {
        buf[pos] = ch;
        buf[pos + 1] = '\0';
    }
    return pos + 1;
}

static size_t append_str(char* buf, size_t size, size_t pos, const char* s) {
    if (!s) {
        s = "(null)";
    }
    s = kernel_str_ptr(s);
    while (*s) {
        pos = append_char(buf, size, pos, *s++);
    }
    return pos;
}

static size_t append_u64(char* buf, size_t size, size_t pos, uint64_t value, uint32_t base,
                         int uppercase, int width, char pad) {
    char tmp[32];
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
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

static size_t append_i64(char* buf, size_t size, size_t pos, int64_t value) {
    if (value < 0) {
        pos = append_char(buf, size, pos, '-');
        return append_u64(buf, size, pos, (uint64_t)(-value), 10, 0, 0, ' ');
    }
    return append_u64(buf, size, pos, (uint64_t)value, 10, 0, 0, ' ');
}

/* Formats into buf, always NUL-terminating, and returns the length the result
 * WOULD have had — which exceeds size-1 when the output was truncated.  At most
 * size-1 characters are written plus the terminator.  A zero size (or a NULL
 * buf) writes nothing and returns 0 rather than that length.
 *
 * The supported subset, in the order the parser accepts them:
 *
 *   flags   '0' only, and only one of it.  '-', '+', ' ' and '#' are not
 *           recognised and end up in the conversion position.
 *   width   decimal digits, honoured by %u, %x and %X only.  %d, %i, %s, %c and
 *           %p ignore it.  Padding happens inside a 32-byte digit scratch, so a
 *           width above 32 pads to 32.  There is no precision: a '.' is not
 *           parsed and falls through to the unknown-conversion path.
 *   length  'l', 'll' and 'z'; 'z' is read as long long.  'h', 'hh', 'j', 't'
 *           and 'L' are not recognised.
 *   conv    %% %c %s %d %i %u %x %X %p.  No floating point.
 *
 * %s prints "(null)" for a NULL argument and rebases kernel-image literals via
 * kernel_str_ptr.  %p always prints "0x" followed by unpadded lowercase hex.
 * %x/%X differ only in digit case.  An unknown conversion is echoed as '%'
 * followed by that character and consumes NO va_arg, so the remaining arguments
 * shift — a typo silently corrupts everything after it.  A '%' at the very end
 * of fmt ends formatting.
 *
 * %d/%i of INT64_MIN negates in signed 64-bit and does not print correctly. */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    if (!buf || size == 0) {
        return 0;
    }
    buf[0] = '\0';
    fmt = kernel_str_ptr(fmt);
    size_t pos = 0;
    for (const char* p = fmt; p && *p; ++p) {
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

        /* Flags: '0' (zero-pad) is the only one recognised. */
        char pad = ' ';
        if (*p == '0') {
            pad = '0';
            p++;
        }

        /* Width. */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* Length modifier: l, ll, z. */
        int lmod = 0; /* 0=int, 1=long, 2=long long / size_t */
        if (*p == 'l') {
            lmod = 1;
            p++;
            if (*p == 'l') {
                lmod = 2;
                p++;
            }
        } else if (*p == 'z') {
            lmod = 2;
            p++;
        }

        switch (*p) {
        case 'c': {
            int ch = va_arg(ap, int);
            pos = append_char(buf, size, pos, (char)ch);
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            pos = append_str(buf, size, pos, s);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v;
            if (lmod == 2)
                v = va_arg(ap, long long);
            else if (lmod == 1)
                v = va_arg(ap, long);
            else
                v = va_arg(ap, int);
            pos = append_i64(buf, size, pos, v);
            break;
        }
        case 'u': {
            uint64_t v;
            if (lmod == 2)
                v = va_arg(ap, unsigned long long);
            else if (lmod == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, v, 10, 0, width, pad);
            break;
        }
        case 'x': {
            uint64_t v;
            if (lmod == 2)
                v = va_arg(ap, unsigned long long);
            else if (lmod == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, v, 16, 0, width, pad);
            break;
        }
        case 'X': {
            uint64_t v;
            if (lmod == 2)
                v = va_arg(ap, unsigned long long);
            else if (lmod == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            pos = append_u64(buf, size, pos, v, 16, 1, width, pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void*);
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

/* Does not return: it logs and hands over to kpanic, which is noreturn.  The
 * declaration is a plain void function, so the compiler still generates the
 * fallthrough after a call site. */
void abort(void) {
    klog_write("[kernel] abort\n");
    kpanic("abort", 0ULL, 0ULL);
}

/* The ctype set below is ASCII-only and locale-free: bytes outside the ranges
 * named in each test are returned or reported unchanged, including every byte
 * >= 0x80.  All of them take an int for source compatibility but only examine
 * the low 8 bits' worth of ASCII range; unlike the standard versions, EOF is not
 * a distinguished input. */
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

/* Space, tab, newline, carriage return, form feed and vertical tab — the full C
 * whitespace set, not just space and tab. */
int isspace(int ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
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

/* Both letter cases, so 'F' and 'f' are equally accepted. */
int isxdigit(int ch) {
    return isdigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}
