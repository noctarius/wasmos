#ifndef WASMOS_LIBC_WASMOS_STARTUP_H
#define WASMOS_LIBC_WASMOS_STARTUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t wasmos_startup_arg(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif
