/* stdlib.h - Minimal stdlib declarations for WASM libc: the malloc family,
 * abs/labs, and the strtol-based string-to-integer conversions. */
#ifndef WASMOS_LIBC_STDLIB_H
#define WASMOS_LIBC_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The heap is a per-instance first-fit free list over a bump allocator that
 * starts at __heap_base and grows WASM linear memory 64 KiB at a time. Blocks
 * carry an inline header, so a pointer handed to free/realloc must be one this
 * allocator returned; adjacent free blocks are coalesced on free. Allocations
 * are pointer-aligned (not 16-byte aligned), and the heap is not thread-safe. */

/* Returns a block of at least `size` bytes, or NULL when linear memory cannot
 * grow. size == 0 allocates one byte rather than returning NULL, so the result
 * is always a distinct freeable pointer. Contents are uninitialized. */
void* malloc(size_t size);
/* Marks the block back as free and coalesces neighbours. NULL is a no-op.
 * Freeing memory not obtained from this allocator corrupts the heap. */
void free(void* ptr);
/* Zero-filled malloc(nmemb * size). Returns NULL if the product overflows
 * size_t; a zero `nmemb` or `size` yields a one-byte allocation whose single
 * byte is NOT zeroed. */
void* calloc(size_t nmemb, size_t size);
/* Resizes `ptr`. realloc(NULL, size) is malloc(size); realloc(ptr, 0) frees and
 * returns NULL (so a NULL return does not always mean failure). A block already
 * large enough is kept in place and may be split; otherwise the payload is
 * copied into a new block and the old one is freed. Returns NULL and leaves
 * `ptr` valid when a needed allocation fails. */
void* realloc(void* ptr, size_t size);

/* Absolute value. abs(INT_MIN) / labs(LONG_MIN) are not representable and
 * overflow, as in ISO C. */
int abs(int value);
long labs(long value);
/* strtol(s, NULL, 10), so a non-numeric string yields 0 and overflow saturates;
 * neither reports an error. atoi truncates the long result to int. */
int atoi(const char* s);
long atol(const char* s);
/* Parse a signed integer: optional whitespace, optional sign, then digits in
 * `base` (2..36, case-insensitive above 9). base 0 auto-detects 0x/0X as 16, a
 * leading 0 as 8, otherwise 10; base 16 also accepts an optional 0x prefix.
 * When `endptr` is non-NULL it receives the first unconsumed character, or `s`
 * itself when no digit was consumed (and NULL when `s` is NULL). Overflow
 * saturates at LONG_MAX / LONG_MIN and is not otherwise reported — there is no
 * errno. An out-of-range `base` returns 0 with *endptr = s. */
long strtol(const char* s, char** endptr, int base);

#ifdef __cplusplus
}
#endif

#endif
