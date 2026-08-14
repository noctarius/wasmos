/* stat.h - Minimal stat(2) struct and constants for WASM libc. */
#ifndef WASMOS_LIBC_SYS_STAT_H
#define WASMOS_LIBC_SYS_STAT_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two fields stat() fills; there is no ownership, timestamp or link count in
 * this system. st_size is a byte count limited to 32 bits, st_mode carries only
 * the file-type bits below. */
struct stat {
    uint32_t st_mode;
    uint32_t st_size;
};

/* File-type field of st_mode, matching the POSIX values: mask st_mode with
 * S_IFMT and compare against S_IFREG or S_IFDIR. No permission bits are set, so
 * testing individual mode bits other than these is meaningless. */
#define S_IFMT 0xF000u
#define S_IFREG 0x8000u
#define S_IFDIR 0x4000u

/* Create a directory. `mode` is accepted and ignored — there are no permission
 * semantics. Returns 0 on success, -1 on any FS failure. */
int mkdir(const char* path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
