/* string.h - Minimal string function declarations for WASM libc. */
#ifndef WASMOS_LIBC_STRING_H
#define WASMOS_LIBC_STRING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NULL handling, which ISO C leaves undefined: the length functions report 0,
 * the string comparisons order NULL before any non-NULL string (NULL vs NULL
 * compares equal), the copies return their destination unmodified, the search
 * functions return NULL, and memcpy/memmove/memset return their destination
 * without touching memory. memcmp is the exception: it dereferences whatever it
 * is given once the pointers differ and `count` is non-zero. */

size_t strlen(const char* s);
/* Length of `s`, stopping at `max_len`; the string need not be terminated
 * within that window. Returns at most max_len. */
size_t strnlen(const char* s, size_t max_len);
/* Compares as unsigned chars and returns the byte difference (not merely its
 * sign), 0 when equal. */
int strcmp(const char* lhs, const char* rhs);
int strncmp(const char* lhs, const char* rhs, size_t count);
/* ASCII-case-insensitive strcmp: both bytes are lowercased with tolower(), so
 * only 'A'-'Z' fold and no locale is consulted. */
int strcasecmp(const char* lhs, const char* rhs);
/* Copies including the terminator with no bound on `dest` — the caller must
 * already know the source fits. Prefer str_copy for anything sized at runtime.
 * Returns `dest`. */
char* strcpy(char* dest, const char* src);
/* ISO C strncpy: writes exactly `count` bytes, NUL-padding a short source and
 * leaving `dest` unterminated when the source is `count` bytes or longer.
 * Returns `dest`. */
char* strncpy(char* dest, const char* src, size_t count);
/* Copy src_len raw bytes into dst[] and NUL-terminate. All-or-nothing: returns
 * -1 (leaving dst untouched) if the result would not fit, i.e. src_len >=
 * dst_len; 0 on success. Same contract as the kernel's str_copy_bytes — keep
 * the two in sync. */
int str_copy_bytes(char* dst, size_t dst_len, const uint8_t* src, size_t src_len);
/* Copy the C-string src into dst[], truncating to fit and always
 * NUL-terminating (dst_len >= 1). Returns the number of bytes written (excluding
 * the NUL). The shared truncating counterpart to str_copy_bytes; keep the kernel
 * and libc copies in sync. */
size_t str_copy(char* dst, size_t dst_len, const char* src);
/* First / last occurrence of `ch` in `s`, or NULL. `ch` is narrowed to char, and
 * the terminator is part of the searched string, so both return a pointer to it
 * when `ch` is '\0'. */
char* strchr(const char* s, int ch);
char* strrchr(const char* s, int ch);
/* First occurrence of `needle` in `haystack`, or NULL. An empty needle matches
 * at `haystack`. Plain O(n*m) scan, no Boyer-Moore. */
char* strstr(const char* haystack, const char* needle);
/* Copies `count` bytes forward in 32/8/1-byte steps. NOT overlap-safe: use
 * memmove when the ranges can overlap. Returns `dest`. */
void* memcpy(void* dest, const void* src, size_t count);
/* Overlap-safe copy: picks a forward or backward pass by comparing the ranges.
 * Returns `dest`. */
void* memmove(void* dest, const void* src, size_t count);
/* Fills `count` bytes with the low byte of `value`, aligning to 8 and then
 * storing 64 bits at a time. Returns `dest`. */
void* memset(void* dest, int value, size_t count);
/* Returns the difference of the first differing bytes compared as unsigned
 * chars, or 0 when the ranges are equal, `count` is 0, or both pointers are the
 * same address. */
int memcmp(const void* lhs, const void* rhs, size_t count);

#ifdef __cplusplus
}
#endif

#endif
