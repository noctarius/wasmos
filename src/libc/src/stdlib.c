#include "stdlib.h"

#include <stddef.h>

static int
is_space(char ch)
{
    switch (ch) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            return 1;
        default:
            return 0;
    }
}

static int
digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'Z') {
        return 10 + (ch - 'A');
    }
    return -1;
}

int
abs(int value)
{
    if (value < 0) {
        return -value;
    }
    return value;
}

long
labs(long value)
{
    if (value < 0) {
        return -value;
    }
    return value;
}

long
strtol(const char *s, char **endptr, int base)
{
    const char *p = s;
    int negative = 0;
    long value = 0;
    int consumed = 0;

    if (!p) {
        if (endptr) {
            *endptr = NULL;
        }
        return 0;
    }

    while (is_space(*p)) {
        p++;
    }

    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
            p++;
        } else {
            base = 10;
        }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = (char *)s;
        }
        return 0;
    }

    while (*p) {
        int digit = digit_value(*p);
        if (digit < 0 || digit >= base) {
            break;
        }
        value = (value * (long)base) + (long)digit;
        consumed = 1;
        p++;
    }

    if (endptr) {
        *endptr = (char *)(consumed ? p : s);
    }

    return negative ? -value : value;
}

int
atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long
atol(const char *s)
{
    return strtol(s, NULL, 10);
}
