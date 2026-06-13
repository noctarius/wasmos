#pragma once
/* compat/unistd.h — freestanding stub of <unistd.h>
 *
 * The kernel does not implement POSIX file descriptors.  close() is declared
 * here so WARP code that calls it compiles; the implementation always returns
 * -1 (error) since there are no open file descriptors in kernel context. */

#ifdef __cplusplus
extern "C" {
#endif

/* Stub: always returns -1 (EBADF).  No file descriptor table in kernel. */
int close(int fd);

#ifdef __cplusplus
} /* extern "C" */
#endif
