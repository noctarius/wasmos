/* startup.h - access to the startup arguments passed by PM at process launch */
#ifndef WASMOS_LIBC_WASMOS_STARTUP_H
#define WASMOS_LIBC_WASMOS_STARTUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return one of the four 32-bit startup args (index 0..3) stored by
 * wasmos_main before calling the application's initialize() entry point. */
int32_t wasmos_startup_arg(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif
