/* stdlib.h - Minimal kernel stdlib declarations: heap allocation, string-to-
 * number conversion, and abort. */
#ifndef WASMOS_STDLIB_H
#define WASMOS_STDLIB_H

#include <stddef.h>

/* The heap behind malloc/free is backend-specific: under WARP it forwards to
 * the global kernel slab, under wasm3 it is a PER-PROCESS arena bound to the
 * pid currently running on this CPU. Kernel-global metadata that outlives a
 * process must therefore not come from here -- use kmem_alloc (kmem.h). */
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
unsigned long strtoul(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);

/* Logs and panics the kernel via kpanic(); does not return. */
void abort(void);

#endif
