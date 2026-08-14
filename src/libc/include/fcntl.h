/* fcntl.h - Minimal file-descriptor flag constants for WASM libc (O_RDONLY, etc.). */
#ifndef WASMOS_LIBC_FCNTL_H
#define WASMOS_LIBC_FCNTL_H

/* Access mode, occupying the low two bits and selected by exactly one of these
 * three values. O_RDONLY is 0, so a caller passing only creation flags gets a
 * read-only open. open() extracts the mode as flags & (O_WRONLY | O_RDWR). */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
/* Creation/positioning flags, ORed onto the access mode and forwarded verbatim
 * as the FS_IPC_OPEN_REQ flag word (the FAT backend's FAT_OPEN_* bits have the
 * same values; the initfs backend ignores them). O_APPEND and O_TRUNC require a
 * writable access mode, and any bit outside this set makes open() fail. */
#define O_CREAT 0x0040
#define O_APPEND 0x0008
#define O_TRUNC 0x0200

#endif
