/* libsys_string.h - String parsing helpers for libsys (WASM target).
 * Provides integer parsing, token splitting, and whitespace handling used by
 * the script engine and device-manager rule parser. */
#ifndef WASMOS_LIBSYS_STRING_H
#define WASMOS_LIBSYS_STRING_H

#include <stdint.h>

#include "ctype.h"
#include "string.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline int32_t
wasmos_sys_strlen(const char *s)
{
    return s ? (int32_t)strlen(s) : 0;
}

static inline int
wasmos_sys_strcmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b);
}

static inline int
wasmos_sys_strcasecmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    return strcasecmp(a, b);
}

static inline int
wasmos_sys_streq(const char *a, const char *b)
{
    return wasmos_sys_strcmp(a, b) == 0;
}

static inline void
wasmos_sys_strcpy(char *dst, uint32_t dst_len, const char *src)
{
    uint32_t i = 0;
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1u < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline void
wasmos_sys_str_append(char *dst, uint32_t dst_len, const char *src)
{
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

static inline int
wasmos_sys_is_space(char c)
{
    return isspace((unsigned char)c) != 0;
}

static inline int
wasmos_sys_is_alpha(char c)
{
    return isalpha((unsigned char)c) != 0;
}

static inline int
wasmos_sys_is_digit(char c)
{
    return isdigit((unsigned char)c) != 0;
}

static inline int
wasmos_sys_is_alnum(char c)
{
    return isalnum((unsigned char)c) != 0;
}

static inline int
wasmos_sys_to_lower(int c)
{
    return tolower((unsigned char)c);
}

static inline int
wasmos_sys_to_upper(int c)
{
    return toupper((unsigned char)c);
}

static inline void
wasmos_sys_to_lower_ascii(char *s)
{
    if (!s) {
        return;
    }
    for (uint32_t i = 0; s[i] != '\0'; ++i) {
        s[i] = (char)wasmos_sys_to_lower((unsigned char)s[i]);
    }
}

/* Return pointer past leading whitespace; does not modify s. */
static inline const char *
wasmos_sys_trim_left(const char *s)
{
    if (!s) {
        return s;
    }
    while (*s && wasmos_sys_is_space(*s)) {
        s++;
    }
    return s;
}

/* NUL-terminate s at the last non-whitespace character; modifies in-place. */
static inline void
wasmos_sys_trim_right(char *s)
{
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

/* Trim leading and trailing whitespace in-place; returns s. */
static inline char *
wasmos_sys_trim(char *s)
{
    const char *left = 0;
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
