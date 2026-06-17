/* zig/compat/string.h
 * Minimal <string.h> declarations for freestanding WASM C compilation.
 * Definitions are provided by the app's C shim.
 * Include path ordering: this directory must come before any system sysroot. */
#ifndef WASMOS_COMPAT_STRING_H
#define WASMOS_COMPAT_STRING_H

#include <stddef.h>

void  *memset(void *s, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
void  *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
char  *strstr(const char *haystack, const char *needle);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

#endif /* WASMOS_COMPAT_STRING_H */
