#ifndef WASMOS_FS_MANAGER_TYPES_H
#define WASMOS_FS_MANAGER_TYPES_H

#include <stdint.h>

#define FS_CLIENT_CAP 16
#define FS_BACKEND_CAP 8

typedef enum {
    FS_MOUNT_ROOT = 0,
    FS_MOUNT_BACKEND = 1
} fs_mount_t;

typedef struct {
    uint8_t in_use;
    uint8_t kind;
    int32_t endpoint;
    uint8_t slot;
    uint8_t has_meta;
    uint8_t unit;
    uint8_t bus;
    uint8_t device_fn;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    char mount_name[16];
} fs_backend_t;

typedef struct {
    uint8_t in_use;
    int32_t context_id;
    fs_mount_t mount;
    int32_t backend_endpoint;
    uint16_t mount_depth;
} fs_client_state_t;

#endif
