/* types.h - Basic POSIX type aliases (off_t, ssize_t, mode_t) for WASM libc. */
#ifndef WASMOS_LIBC_SYS_TYPES_H
#define WASMOS_LIBC_SYS_TYPES_H

#include <stddef.h>

/* Both signed types are `long`, which is 32-bit on wasm32 and 64-bit on the
 * native x86_64 builds; code that stores a byte count or file offset in a fixed
 * width must not assume 64 bits. lseek() additionally clamps offsets to the
 * int32 range the IPC arguments carry. mode_t is accepted by mkdir() and
 * ignored — there are no permission bits in this system. */
typedef long ssize_t;
typedef long off_t;
typedef unsigned int mode_t;

#endif
