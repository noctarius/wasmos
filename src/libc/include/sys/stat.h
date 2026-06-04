/* stat.h - Minimal stat(2) struct and constants for WASM libc. */
#ifndef WASMOS_LIBC_SYS_STAT_H
#define WASMOS_LIBC_SYS_STAT_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stat {
    uint32_t st_mode;
    uint32_t st_size;
};

#define S_IFMT  0xF000u
#define S_IFREG 0x8000u
#define S_IFDIR 0x4000u

int mkdir(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
