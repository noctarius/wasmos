#ifndef WASMOS_PROCESS_MANAGER_BUFFER_H
#define WASMOS_PROCESS_MANAGER_BUFFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_BUFFER_KIND_FILESYSTEM  1u
#define PM_BUFFER_KIND_FRAMEBUFFER 2u

#define PM_BUFFER_BORROW_READ  0x1u
#define PM_BUFFER_BORROW_WRITE 0x2u

#ifdef __cplusplus
}
#endif

#endif
