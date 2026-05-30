#ifndef WASMOS_SYSINIT_TYPES_H
#define WASMOS_SYSINIT_TYPES_H

#include <stdint.h>

#define SYSINIT_MAX_SPAWN_ATTEMPTS 128u
#define SYSINIT_SCRIPT_PATH "/boot/system/sysinit.rc"

typedef struct {
    int32_t reply_endpoint;
    int32_t spawn_request_id;
    int32_t proc_endpoint;
} sysinit_state_t;

#endif
