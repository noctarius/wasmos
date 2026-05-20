#ifndef WASMOS_SYSINIT_TYPES_H
#define WASMOS_SYSINIT_TYPES_H

#include <stdint.h>

#define BOOT_CONFIG_MAGIC "WCFG0001"
#define BOOT_CONFIG_VERSION 1u
#define SYSINIT_MAX_TARGET_NAME_LEN 16u
#define SYSINIT_MAX_TARGETS 16
#define SYSINIT_MAX_PROC_SNAPSHOT 64
#define SYSINIT_DEP_SETTLE_YIELDS 16u
#define SYSINIT_MAX_SPAWN_ATTEMPTS 128u

typedef struct {
    int32_t reply_endpoint;
    int32_t spawn_request_id;
    int32_t proc_endpoint;
    int32_t target_index;
    int32_t target_count;
    const char *targets[SYSINIT_MAX_TARGETS];
} sysinit_state_t;

#endif
