/* stdlib.h - Minimal stdlib declarations: malloc/free/atoi/itoa for WASM libc. */
#ifndef WASMOS_LIBC_STDLIB_H
#define WASMOS_LIBC_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int abs(int value);
long labs(long value);
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **endptr, int base);

#ifdef __cplusplus
}
#endif

#endif
