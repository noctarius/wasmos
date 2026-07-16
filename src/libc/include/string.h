/* string.h - Minimal string function declarations for WASM libc. */
#ifndef WASMOS_LIBC_STRING_H
#define WASMOS_LIBC_STRING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max_len);
int strcmp(const char *lhs, const char *rhs);
int strncmp(const char *lhs, const char *rhs, size_t count);
int strcasecmp(const char *lhs, const char *rhs);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);
/* Copy src_len raw bytes into dst[] and NUL-terminate. All-or-nothing: returns
 * -1 (leaving dst untouched) if the result would not fit, i.e. src_len >=
 * dst_len; 0 on success. Same contract as the kernel's str_copy_bytes — keep
 * the two in sync. */
int str_copy_bytes(char *dst, size_t dst_len, const uint8_t *src, size_t src_len);
/* Copy the C-string src into dst[], truncating to fit and always
 * NUL-terminating (dst_len >= 1). Returns the number of bytes written (excluding
 * the NUL). The shared truncating counterpart to str_copy_bytes; keep the kernel
 * and libc copies in sync. */
size_t str_copy(char *dst, size_t dst_len, const char *src);
char *strchr(const char *s, int ch);
char *strrchr(const char *s, int ch);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *lhs, const void *rhs, size_t count);

#ifdef __cplusplus
}
#endif

#endif
