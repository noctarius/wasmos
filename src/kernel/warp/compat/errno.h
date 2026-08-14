#pragma once
/* compat/errno.h — freestanding errno stub for bare-metal kernel.
 *
 * Provides the POSIX error-number constants listed below plus an errno macro.
 * compat/cerrno is a one-line wrapper around this file.
 *
 * The kernel has no per-thread errno cell, so errno expands to the literal 0
 * (no error).  Nothing in the kernel sets it: the POSIX stubs in
 * src/kernel/warp/posix_kernel.c report failure through their return value
 * (MAP_FAILED from mmap, EINVAL/ENOMEM returned directly by posix_memalign) and
 * never touch errno.  No WARP translation unit in the kernel build reads errno
 * either — the one WARP source that does, utils/ExecutableMemory.cpp, is not
 * compiled here.  The constants exist so that WARP code and posix_kernel.c
 * agree on the numeric values.
 *
 * Consequences of the constant-zero errno, both of which are compile-time
 * failures rather than silent misbehaviour:
 *   - `errno = ENOMEM` expands to `0 = 12`, which is a hard error ("expression
 *     is not assignable"), not a warning.
 *   - `&errno` and any other lvalue use fail the same way.
 * A reader that only tests errno compiles and always sees success, which is the
 * one silent case; add real per-CPU storage before letting WARP branch on it. */

/* Common POSIX error numbers.  Values match Linux/glibc so that a WARP source
 * comparing against its own copy of these names agrees with posix_kernel.c. */
#define EPERM 1    /* Operation not permitted */
#define ENOENT 2   /* No such file or directory */
#define ESRCH 3    /* No such process */
#define EINTR 4    /* Interrupted system call */
#define EIO 5      /* Input/output error */
#define ENXIO 6    /* No such device or address */
#define EBADF 9    /* Bad file descriptor */
#define ENOMEM 12  /* Out of memory */
#define EACCES 13  /* Permission denied */
#define EFAULT 14  /* Bad address */
#define EBUSY 16   /* Device or resource busy */
#define EEXIST 17  /* File exists */
#define EINVAL 22  /* Invalid argument */
#define ENOSPC 28  /* No space left on device */
#define ENOSYS 38  /* Function not implemented */
#define ENOTSUP 95 /* Operation not supported */

/* errno is always 0 in the freestanding kernel context.  Read-only: this is an
 * rvalue, so it can be compared but never assigned or address-taken. */
#define errno 0
