#pragma once
/* compat/sys/mman.h — freestanding mmap stub for bare-metal kernel.
 *
 * Provides the PROT_ and MAP_ flag families, MAP_FAILED, and declarations of mmap,
 * mprotect and munmap.  All three have real definitions in
 * src/kernel/warp/posix_kernel.c; nothing here is declaration-only.  Missing
 * relative to POSIX: msync, madvise, mlock, mremap, and MAP_NORESERVE and the
 * other less common flags — using one is a compile error.
 *
 * WARP uses mmap()/mprotect()/munmap() to allocate executable JIT buffers and
 * set page permissions.  In the kernel build only kernel glue includes this
 * header: the one WARP source that includes <sys/mman.h>, utils/MemUtils.cpp,
 * is replaced by warp/mem_utils_kernel.cpp.
 *
 * What the kernel implementation actually does, and how it differs from POSIX:
 *   - mmap ignores addr, prot, flags, fd and offset.  It allocates whole pages
 *     from the kernel frame allocator below 512 MB and returns their
 *     higher-half alias, which is already mapped RWX.  So the mapping is always
 *     anonymous, private and fixed-address-of-the-kernel's-choosing regardless
 *     of what the caller asked for, and MAP_FIXED is not honoured.
 *   - mprotect is a no-op returning 0.  JIT pages are RWX for their whole
 *     lifetime; a request to drop write or exec permission silently succeeds
 *     without changing anything.  Code that relies on W^X enforcement gets
 *     none — see the TODO in posix_kernel.c.
 *   - munmap frees only an address that a previous mmap returned, matched
 *     against a fixed 64-entry table, and returns -1 otherwise.  Partial
 *     unmapping of a range is not supported; length is ignored. */

#include <stddef.h>

/* Protection and mapping flag values match Linux/x86_64 so the numbers agree
 * with whatever a WARP source assumes.  Both sets are accepted and ignored by
 * the kernel implementation; they exist to let the call sites compile. */

/* Protection flags. */
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define PROT_NONE 0

/* Mapping flags. */
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS /* BSD alias used by some headers */
#define MAP_FIXED 0x10

/* Sentinel returned on failure — the POSIX value, an all-ones pointer that is
 * deliberately not a valid mapping.  mmap in posix_kernel.c returns this for a
 * zero length, an exhausted frame allocator, or a full mmap table; munmap
 * rejects it as an input. */
#define MAP_FAILED ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif

/* Declarations — implementations must be provided by the kernel.
 * Backed by src/kernel/warp/posix_kernel.c. */
void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset);
int mprotect(void* addr, size_t length, int prot);
int munmap(void* addr, size_t length);

#ifdef __cplusplus
} /* extern "C" */
#endif
