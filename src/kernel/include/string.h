/* string.h - Minimal kernel string function declarations (memcpy/memset/strcmp/etc.). */
#ifndef WASMOS_STRING_H
#define WASMOS_STRING_H

#include <stddef.h>
#include <stdint.h>

/* The string functions that take a `const char*` source (strcmp, strncmp, strcasecmp,
 * strcpy, strncpy, strnlen, str_eq_bytes, strchr, strrchr) rebase a source pointer that
 * lands inside the kernel image at a low VA onto the higher-half alias, so a string
 * literal reached through the identity map stays readable under a root that has no low
 * mapping.  Buffers outside the kernel image are used as given.  strlen, str_copy and the
 * mem* family perform no such translation.  None of these validate that a pointer belongs
 * to the caller's address space — user pointers go through mm_copy_from_user instead. */

/* Copies n bytes strictly forward and returns dst.  Not overlap-safe: overlapping ranges
 * need memmove.  Passing NULL with a non-zero n faults. */
void* memcpy(void* dst, const void* src, size_t n);

/* Fills n bytes of dst with the low byte of c via `rep stosb` (direction flag forced
 * forward, so it is safe in ISRs and early boot) and returns dst. */
void* memset(void* dst, int c, size_t n);

/* Copies n bytes with overlap handled — descending when the ranges overlap forward — and
 * returns dst.  A zero n or identical pointers are a no-op. */
void* memmove(void* dst, const void* src, size_t n);

/* Compares n bytes and returns -1, 0 or 1 (not the byte difference).  A zero n compares
 * equal without dereferencing. */
int memcmp(const void* a, const void* b, size_t n);

/* Length of a NUL-terminated string, or 0 for NULL.  Unbounded: a string without a
 * terminator runs off the end. */
size_t strlen(const char* s);

/* Length of s, scanning at most max_len bytes; returns max_len when no terminator is
 * found within that window, and 0 for NULL. */
size_t strnlen(const char* s, size_t max_len);

/* Ordering comparison returning -1, 0 or 1 (not the byte difference).  NULL is ordered
 * before any non-NULL string, and two NULLs compare equal. */
int strcmp(const char* a, const char* b);

/* Compares at most n bytes, stopping at the first difference or terminator, and returns
 * the difference of the two unsigned chars (0 when equal).  A zero n or identical
 * pointers compare equal; NULL is ordered before non-NULL. */
int strncmp(const char* a, const char* b, size_t n);

/* strncmp-style case-insensitive comparison over the whole string: returns the
 * difference of the lowercased unsigned chars, 0 when equal.  ASCII only. */
int strcasecmp(const char* a, const char* b);

/* Copies src including its terminator into dst and returns dst.  Unbounded — dst must
 * already be large enough — so prefer str_copy.  A NULL dst or src copies nothing and
 * returns dst unchanged. */
char* strcpy(char* dst, const char* src);

/* C-standard strncpy: writes exactly n bytes, NUL-padding a short source and NOT
 * terminating when src is at least n bytes long.  Returns dst; a NULL dst or src copies
 * nothing. */
char* strncpy(char* dst, const char* src, size_t n);

/* Refuse-on-overflow copy of a length-counted, not necessarily NUL-terminated byte range
 * into a C string.  Writes src_len bytes plus a terminator and returns 0, or returns -1
 * without writing anything when dst or src is NULL, either length is 0, or src_len does
 * not leave room for the terminator (src_len >= dst_len).  Use this rather than a
 * hand-rolled loop when a too-long name must be rejected instead of silently shortened. */
int str_copy_bytes(char* dst, size_t dst_len, const uint8_t* src, size_t src_len);

/* Truncating copy of a C string into a dst_len-byte buffer.  Always NUL-terminates and
 * returns the number of bytes written excluding the terminator, so a return of
 * dst_len - 1 is the signal that truncation may have happened.  A NULL or zero-length
 * dst writes nothing and returns 0; a NULL src produces an empty string. */
size_t str_copy(char* dst, size_t dst_len, const char* src);

/* Compares a length-counted byte range against a NUL-terminated literal.  Returns 1 only
 * when the lengths match exactly and every byte is equal, 0 otherwise (including for a
 * NULL argument) — note the polarity is the opposite of strcmp's. */
int str_eq_bytes(const uint8_t* bytes, size_t bytes_len, const char* lit);

/* First / last occurrence of (char)ch in s, or NULL when absent or s is NULL.  The
 * terminator is part of the search, so passing 0 for ch finds the end of the string.  The
 * returned pointer aliases into s (possibly rebased to the higher-half alias). */
char* strchr(const char* s, int ch);
char* strrchr(const char* s, int ch);

#endif
