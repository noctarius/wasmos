#pragma once
/* compat/unistd.h — freestanding stub of <unistd.h>.
 * Provides types and stubs needed by WARP's MemUtils.hpp. */

#include <stddef.h>
#include <stdint.h>

typedef long          off_t;
typedef long          ssize_t;
typedef unsigned long pid_t;

/* _SC_PAGE_SIZE matches Linux value 30 — sysconf() is implemented in
 * posix_kernel.c and always returns 4096. */
#define _SC_PAGE_SIZE 30

#ifdef __cplusplus
extern "C" {
#endif

long  sysconf(int name);
int   close(int fd);
int   posix_memalign(void **memptr, size_t alignment, size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);
int   ftruncate(int fd, off_t length);
pid_t getpid(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
