#include "string.h"
#include "ctype.h"

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
    for (size_t i = 0; i < count; ++i) {
        out[i] = in[i];
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
    if (out < in) {
        for (size_t i = 0; i < count; ++i) {
            out[i] = in[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            out[i - 1] = in[i - 1];
        }
    }
    return dest;
}

void *
memset(void *dest, int value, size_t count)
{
    unsigned char *out = (unsigned char *)dest;

    if (!dest) {
        return dest;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = (unsigned char)value;
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
