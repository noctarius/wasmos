#pragma once
/* compat/errno.h — freestanding errno stub for bare-metal kernel.
 *
 * Provides the POSIX error-number constants listed below and no errno.
 * compat/cerrno is a one-line wrapper around this file.
 *
 * The kernel has no per-thread errno cell, so errno is not defined: reading,
 * assigning or taking the address of it is a compile error.  No WARP
 * translation unit in the kernel build names it — the one WARP source that
 * does, utils/ExecutableMemory.cpp, is not compiled here.  The constants exist
 * so that WARP code naming them compiles with the Linux values.
 *
 * TODO: give errno real per-thread storage before compiling a WARP source that
 * reads it; until then such a source does not build. */

/* Common POSIX error numbers, with the Linux/glibc values. */
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
