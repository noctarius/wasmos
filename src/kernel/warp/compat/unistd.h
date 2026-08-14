#pragma once
/* compat/unistd.h — freestanding stub of <unistd.h>.
 *
 * Provides off_t/ssize_t/pid_t, _SC_PAGE_SIZE, and declarations of sysconf,
 * close, posix_memalign, free, realloc, ftruncate and getpid.  Not the POSIX
 * header's file/process surface: no read/write/open, no fork/exec, no sleep.
 *
 * Which declarations are real:
 *   - sysconf, close, posix_memalign, free and realloc are defined in
 *     src/kernel/warp/posix_kernel.c.  free/realloc are the kernel slab
 *     allocator (the same pair compat/cstdlib declares); posix_memalign IGNORES
 *     the alignment argument and returns slab memory, so a caller that needs a
 *     specific alignment does not get it and nothing reports the failure;
 *     close is a no-op returning 0.
 *   - ftruncate and getpid are DECLARATION ONLY.  Nothing in the tree defines
 *     them, so calling one is an undefined-symbol link error, not a runtime
 *     surprise.
 *
 * No translation unit in the kernel build includes this header today.  The
 * WARP sources that include <unistd.h> — utils/MemUtils.cpp,
 * utils/ExecutableMemory.cpp, utils/SignalFunctionWrapper_unix.cpp — are all
 * excluded from the kernel build, and posix_kernel.c defines its half of the
 * above without including this file.  So the subset here is what the kernel
 * build links, not a set established from live call sites. */

#include <stddef.h>
#include <stdint.h>

typedef long off_t;
typedef long ssize_t;
typedef unsigned long pid_t;

/* _SC_PAGE_SIZE matches Linux value 30 — sysconf() is implemented in
 * posix_kernel.c and always returns 4096.  It ignores its argument, so every
 * other _SC_* query would get the page size back as well; this is the only
 * name defined here to keep such a query from compiling. */
#define _SC_PAGE_SIZE 30

#ifdef __cplusplus
extern "C" {
#endif

long sysconf(int name);
int close(int fd);
int posix_memalign(void** memptr, size_t alignment, size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
int ftruncate(int fd, off_t length);
pid_t getpid(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
