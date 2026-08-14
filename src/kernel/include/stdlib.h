/* stdlib.h - Minimal kernel stdlib declarations: heap allocation, string-to-
 * number conversion, and abort. */
#ifndef WASMOS_STDLIB_H
#define WASMOS_STDLIB_H

#include <stddef.h>

/* The heap behind malloc/free is backend-specific: under WARP it forwards to
 * the global kernel slab, under wasm3 it is a PER-PROCESS arena bound to the
 * pid currently running on this CPU. Kernel-global metadata that outlives a
 * process must therefore not come from here -- use kmem_alloc (kmem.h). */
/* malloc returns NULL when the request cannot be satisfied; free ignores NULL; calloc
 * zeroes the block and returns NULL on a zero operand or a multiplication overflow;
 * realloc(NULL, n) allocates.  realloc(p, 0) differs by backend — under WARP it frees p
 * and returns NULL, under wasm3 it returns NULL and leaves p allocated — so do not use it
 * to free.  A block must be released through free on the same backend, and under wasm3
 * from the same process, since another process's arena does not recognise the pointer.
 * calloc is provided by the wasm3 backend only. */
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

/* Minimal string-to-number conversion for third-party sources, provided by the wasm3
 * backend only.  strtoul/strtoull skip leading spaces, accept no sign and no 0x prefix
 * (base 0 is treated as 10), stop at the first character not valid in `base`, and store
 * that position in *endptr when it is non-NULL; there is no overflow detection and no
 * errno.  strtod parses an optional sign, an integer part and an optional fractional part
 * only: no exponent, no inf/nan. */
unsigned long strtoul(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);

/* Logs and panics the kernel via kpanic(); does not return. */
void abort(void);

#endif
