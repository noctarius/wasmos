/* libsys_string.h - NULL-tolerant string helpers for libsys (WASM target).
 * Comparison, bounded append, character classification, and whitespace
 * trimming, used by the device-manager rule parser, fs-manager, and the CLI. */
#ifndef WASMOS_LIBSYS_STRING_H
#define WASMOS_LIBSYS_STRING_H

#include <stdint.h>

#include "ctype.h"
#include "string.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Comparison group. A NULL operand is treated as the empty string rather than
 * faulting: strlen(NULL) is 0, and strcmp/strcasecmp against NULL compare as if
 * against "". Ordering and sign follow the underlying libc functions. */
static inline int32_t wasmos_sys_strlen(const char* s) {
    return s ? (int32_t)strlen(s) : 0;
}

static inline int wasmos_sys_strcmp(const char* a, const char* b) {
    if (!a)
        a = "";
    if (!b)
        b = "";
    return strcmp(a, b);
}

static inline int wasmos_sys_strcasecmp(const char* a, const char* b) {
    if (!a)
        a = "";
    if (!b)
        b = "";
    return strcasecmp(a, b);
}

static inline int wasmos_sys_streq(const char* a, const char* b) {
    return wasmos_sys_strcmp(a, b) == 0;
}

/* Append `src` to the NUL-terminated string in `dst`, which holds at most
 * dst_len bytes including the terminator. TRUNCATES silently when the result
 * would not fit and always re-terminates. Does nothing for a NULL dst or src or
 * a zero dst_len; `dst` must already be NUL-terminated within dst_len. */
static inline void wasmos_sys_str_append(char* dst, uint32_t dst_len, const char* src) {
    uint32_t pos = 0;
    if (!dst || dst_len == 0 || !src) {
        return;
    }
    while (pos + 1u < dst_len && dst[pos]) {
        pos++;
    }
    for (uint32_t i = 0; src[i] && pos + 1u < dst_len; ++i) {
        dst[pos++] = src[i];
    }
    dst[pos] = '\0';
}

/* Classification and case group. Each forwards to the matching <ctype.h>
 * function with the argument widened as an unsigned char, so a byte above 0x7F
 * is well-defined rather than undefined. The predicates normalise to 0/1;
 * to_lower/to_upper return the converted character as an int and leave
 * anything outside the alphabetic range unchanged. */
static inline int wasmos_sys_is_space(char c) {
    return isspace((unsigned char)c) != 0;
}

static inline int wasmos_sys_is_alpha(char c) {
    return isalpha((unsigned char)c) != 0;
}

static inline int wasmos_sys_is_digit(char c) {
    return isdigit((unsigned char)c) != 0;
}

static inline int wasmos_sys_is_alnum(char c) {
    return isalnum((unsigned char)c) != 0;
}

static inline int wasmos_sys_to_lower(int c) {
    return tolower((unsigned char)c);
}

static inline int wasmos_sys_to_upper(int c) {
    return toupper((unsigned char)c);
}

/* Lowercase a NUL-terminated string in place; NULL is ignored. */
static inline void wasmos_sys_to_lower_ascii(char* s) {
    if (!s) {
        return;
    }
    for (uint32_t i = 0; s[i] != '\0'; ++i) {
        s[i] = (char)wasmos_sys_to_lower((unsigned char)s[i]);
    }
}

/* Return pointer past leading whitespace; does not modify s. NULL in, NULL out;
 * an all-whitespace string yields a pointer to its terminator. */
static inline const char* wasmos_sys_trim_left(const char* s) {
    if (!s) {
        return s;
    }
    while (*s && wasmos_sys_is_space(*s)) {
        s++;
    }
    return s;
}

/* NUL-terminate s at the last non-whitespace character; modifies in-place.
 * NULL is ignored; an all-whitespace string becomes empty. */
static inline void wasmos_sys_trim_right(char* s) {
    int32_t i = 0;
    if (!s) {
        return;
    }
    while (s[i] != '\0') {
        i++;
    }
    while (i > 0 && wasmos_sys_is_space(s[i - 1])) {
        s[i - 1] = '\0';
        i--;
    }
}

/* Trim leading and trailing whitespace in-place; returns s. Leading whitespace
 * is removed by shifting the remainder down, so the returned pointer is the
 * original buffer and NULL in yields NULL out. */
static inline char* wasmos_sys_trim(char* s) {
    const char* left = 0;
    uint32_t n = 0;
    if (!s) {
        return s;
    }
    left = wasmos_sys_trim_left(s);
    if (left != s) {
        while (left[n] != '\0') {
            s[n] = left[n];
            n++;
        }
        s[n] = '\0';
    }
    wasmos_sys_trim_right(s);
    return s;
}

#ifdef __cplusplus
}
#endif

#endif
