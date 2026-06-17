/* zig/compat/stdlib.h
 * Minimal <stdlib.h> declarations for freestanding WASM C compilation.
 * Definitions are provided by the app's C shim (arena allocator backed by
 * a static buffer so no OS heap is required).
 * Include path ordering: this directory must come before any system sysroot
 * so that #include <stdlib.h> in wasmos C headers finds this file. */
#ifndef WASMOS_COMPAT_STDLIB_H
#define WASMOS_COMPAT_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t n, size_t size);

long int strtol(const char *nptr, char **endptr, int base);

static inline void abort(void) { for (;;) {} }
static inline void exit(int status) { (void)status; for (;;) {} }

#endif /* WASMOS_COMPAT_STDLIB_H */
