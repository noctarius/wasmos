#pragma once
/* compat/errno.h — freestanding errno stub for bare-metal kernel.
 *
 * The kernel has no per-thread errno cell.  We define errno as a constant
 * zero (no error) so that code that checks errno after syscall stubs gets a
 * safe default.  WARP only checks errno after mmap/mprotect; those stubs set
 * it to ENOMEM on failure via the macro below if needed.
 *
 * NOTE: if WARP code *writes* to errno (e.g. errno = ENOMEM), the macro
 * expands to a write to a discarded rvalue — the compiler will warn. A
 * writable global would require per-CPU storage; add one if WARP starts
 * writing errno. */

/* Common POSIX error numbers. */
#define EPERM    1   /* Operation not permitted */
#define ENOENT   2   /* No such file or directory */
#define ESRCH    3   /* No such process */
#define EINTR    4   /* Interrupted system call */
#define EIO      5   /* Input/output error */
#define ENXIO    6   /* No such device or address */
#define EBADF    9   /* Bad file descriptor */
#define ENOMEM  12   /* Out of memory */
#define EACCES  13   /* Permission denied */
#define EFAULT  14   /* Bad address */
#define EBUSY   16   /* Device or resource busy */
#define EEXIST  17   /* File exists */
#define EINVAL  22   /* Invalid argument */
#define ENOSPC  28   /* No space left on device */
#define ENOSYS  38   /* Function not implemented */
#define ENOTSUP 95   /* Operation not supported */

/* errno is always 0 in the freestanding kernel context. */
#define errno 0
