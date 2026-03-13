#ifndef WASMOS_LIBC_STRING_H
#define WASMOS_LIBC_STRING_H

#include <stddef.h>

size_t strlen(const char *s);
int strcmp(const char *lhs, const char *rhs);
int strncmp(const char *lhs, const char *rhs, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *lhs, const void *rhs, size_t count);

#endif
