#pragma once
/* compat/sys/mman.h — freestanding mmap stub for bare-metal kernel.
 *
 * WARP uses mmap()/mprotect()/munmap() to allocate executable JIT buffers
 * and set page permissions.  In the kernel these calls are intercepted and
 * backed by the kernel paging/physmem layer.  The declarations here match
 * the POSIX signatures; the definitions live in a separate kernel source
 * file that implements the actual page-level operations. */

#include <stddef.h>

/* Protection flags. */
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define PROT_NONE   0

/* Mapping flags. */
#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS   /* BSD alias used by some headers */
#define MAP_FIXED     0x10

/* Sentinel returned on failure. */
#define MAP_FAILED ((void *)-1)

#ifdef __cplusplus
extern "C" {
#endif

/* Declarations — implementations must be provided by the kernel.
 * See kernel/warp/mmap_shim.c (or equivalent) for the backing code. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int   mprotect(void *addr, size_t length, int prot);
int   munmap(void *addr, size_t length);

#ifdef __cplusplus
} /* extern "C" */
#endif
