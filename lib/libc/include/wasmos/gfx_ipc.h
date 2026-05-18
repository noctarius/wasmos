#ifndef WASMOS_LIBC_WASMOS_GFX_IPC_H
#define WASMOS_LIBC_WASMOS_GFX_IPC_H

#include <stdint.h>

#define FB_IPC_ABI_MAGIC   0x46424950u /* FBIP */
#define FB_IPC_ABI_VERSION 1u

#define GFX_IPC_ABI_MAGIC   0x47465850u /* GFXP */
#define GFX_IPC_ABI_VERSION 1u

enum {
    FB_IPC_GET_INFO = 0x0100,
    FB_IPC_SET_MODE = 0x0101,
    FB_IPC_MAP_SCANOUT = 0x0102,
    FB_IPC_ALLOC_BUFFER = 0x0103,
    FB_IPC_PRESENT = 0x0104,
    FB_IPC_WAIT_VBLANK = 0x0105
};

enum {
    GFX_IPC_CREATE_WINDOW = 0x0200,
    GFX_IPC_DESTROY_WINDOW = 0x0201,
    GFX_IPC_RESIZE_WINDOW = 0x0202,
    GFX_IPC_ALLOC_SHARED_BUFFER = 0x0203,
    GFX_IPC_SUBMIT_COMMANDS = 0x0204,
    GFX_IPC_PRESENT_WINDOW = 0x0205,
    GFX_IPC_POLL_EVENT = 0x0206,
    GFX_IPC_RESP = 0x0280,
    GFX_IPC_ERROR = 0x02FF
};

enum {
    GFX_STATUS_OK = 0,
    GFX_STATUS_INVALID = -1,
    GFX_STATUS_PERMISSION = -2,
    GFX_STATUS_UNSUPPORTED = -3,
    GFX_STATUS_BUSY = -4,
    GFX_STATUS_IO = -5
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t request_id;
    int32_t status;
} gfx_ipc_hdr_t;

static inline uint32_t
gfx_ipc_header_pack(uint16_t version, uint16_t opcode)
{
    return ((uint32_t)version << 16) | (uint32_t)opcode;
}

static inline int
gfx_ipc_header_valid(uint32_t magic, uint32_t ver_opcode)
{
    uint16_t version = (uint16_t)(ver_opcode >> 16);
    return magic == GFX_IPC_ABI_MAGIC && version == GFX_IPC_ABI_VERSION;
}

#endif
