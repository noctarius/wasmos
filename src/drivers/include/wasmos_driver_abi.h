#ifndef WASMOS_DRIVER_ABI_H
#define WASMOS_DRIVER_ABI_H

#include <stdint.h>

#define CONSOLE_RING_DATA_SIZE 4080u

typedef struct {
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    uint32_t capacity;
    uint32_t _pad;
    uint8_t data[CONSOLE_RING_DATA_SIZE];
} console_ring_t;

enum {
    WASM_CHARDEV_IPC_READ_REQ = 0x100,
    WASM_CHARDEV_IPC_WRITE_REQ = 0x101,
    WASM_CHARDEV_IPC_READ_RESP = 0x180,
    WASM_CHARDEV_IPC_WRITE_RESP = 0x181,
    WASM_CHARDEV_IPC_ERROR_RESP = 0x1FF
};

enum {
    PROC_IPC_SPAWN = 0x200,
    PROC_IPC_WAIT = 0x201,
    PROC_IPC_KILL = 0x202,
    PROC_IPC_STATUS = 0x203,
    PROC_IPC_SPAWN_NAME = 0x204,
    PROC_IPC_SPAWN_CAPS = 0x205,
    PROC_IPC_MODULE_META = 0x206,
    PROC_IPC_MODULE_META_PATH = 0x207,
    /* Spawn with extended capability descriptor payload:
     * arg0=module_index arg1=user_ptr(wasmos_spawn_caps_v2_t + windows[])
     * arg2=payload_size_bytes arg3=reserved(0). */
    PROC_IPC_SPAWN_CAPS_V2 = 0x208,
    PROC_IPC_RESP = 0x280,
    PROC_IPC_ERROR = 0x2FF
};

enum {
    PROC_MODULE_SOURCE_INITFS = 0,
    PROC_MODULE_SOURCE_FS = 1
};

enum {
    SVC_IPC_REGISTER_REQ = 0x220,
    SVC_IPC_LOOKUP_REQ = 0x221,
    SVC_IPC_REGISTER_RESP = 0x2A0,
    SVC_IPC_LOOKUP_RESP = 0x2A1,
    SVC_IPC_ERROR = 0x2AF
};

enum {
    PROC_STATUS_UNKNOWN = 0,
    PROC_STATUS_RUNNING = 1,
    PROC_STATUS_ZOMBIE = 2
};

enum {
    BLOCK_IPC_READ_REQ = 0x300,
    BLOCK_IPC_WRITE_REQ = 0x301,
    BLOCK_IPC_IDENTIFY_REQ = 0x302,
    BLOCK_IPC_READ_RESP = 0x380,
    BLOCK_IPC_WRITE_RESP = 0x381,
    BLOCK_IPC_IDENTIFY_RESP = 0x382,
    BLOCK_IPC_ERROR = 0x3FF
};

enum {
    FS_IPC_OPEN_REQ = 0x400,
    FS_IPC_READ_REQ = 0x401,
    FS_IPC_CLOSE_REQ = 0x402,
    FS_IPC_STAT_REQ = 0x403,
    FS_IPC_READY_REQ = 0x404,
    FS_IPC_SEEK_REQ = 0x405,
    FS_IPC_WRITE_REQ = 0x406,
    FS_IPC_UNLINK_REQ = 0x407,
    FS_IPC_MKDIR_REQ = 0x408,
    FS_IPC_RMDIR_REQ = 0x409,
    FS_IPC_READDIR_REQ = 0x410,
    FS_IPC_CHDIR_REQ = 0x412,
    FS_IPC_READ_APP_REQ = 0x413,
    FS_IPC_RESP = 0x480,
    FS_IPC_STREAM = 0x481,
    FS_IPC_ERROR = 0x4FF
};

enum {
    FSMGR_IPC_REGISTER_BACKEND_REQ = 0x420,
    FSMGR_IPC_REGISTER_BACKEND_RESP = 0x4A0
};

enum {
    FSMGR_BACKEND_BOOT = 1,
    FSMGR_BACKEND_INIT = 2
};

enum {
    FBTEXT_IPC_CELL_WRITE_REQ  = 0x600,
    FBTEXT_IPC_CURSOR_SET_REQ  = 0x601,
    FBTEXT_IPC_SCROLL_REQ      = 0x602,
    FBTEXT_IPC_CLEAR_REQ       = 0x603,
    FBTEXT_IPC_CONSOLE_MODE_REQ = 0x604, /* arg0: 0=ring off, 1=ring on */
    FBTEXT_IPC_GEOMETRY_REQ    = 0x605,  /* resp: arg0=cols arg1=rows */
    FBTEXT_IPC_RESP            = 0x680,
    FBTEXT_IPC_ERROR           = 0x6FF
};

enum {
    VT_IPC_WRITE_REQ    = 0x700,
    VT_IPC_READ_REQ     = 0x701,
    VT_IPC_SET_ATTR_REQ = 0x702,
    VT_IPC_SWITCH_TTY   = 0x703,
    VT_IPC_GET_ACTIVE_TTY = 0x704,
    VT_IPC_REGISTER_WRITER = 0x705,
    VT_IPC_SET_MODE_REQ = 0x706,
    VT_IPC_RESP         = 0x780,
    VT_IPC_ERROR        = 0x7FF
};

enum {
    VT_INPUT_MODE_RAW = 0,
    VT_INPUT_MODE_CANONICAL = 1 << 0,
    VT_INPUT_MODE_ECHO = 1 << 1
};

enum {
    KBD_IPC_SUBSCRIBE_REQ  = 0x800,
    KBD_IPC_SUBSCRIBE_RESP = 0x880,
    KBD_IPC_KEY_NOTIFY     = 0x801
};

enum {
    DEVMGR_PUBLISH_DEVICE = 0x900,
    DEVMGR_PCI_SCAN_DONE  = 0x901,
    DEVMGR_QUERY_MOUNT_REQ = 0x902,
    DEVMGR_MOUNT_INFO = 0x980,
    DEVMGR_QUERY_DONE = 0x981
};

enum {
    DEVMGR_CAP_IO_PORT  = 1 << 0,
    DEVMGR_CAP_MMIO_MAP = 1 << 1,
    DEVMGR_CAP_IRQ      = 1 << 2,
    DEVMGR_CAP_DMA      = 1 << 3
};

enum {
    WASMOS_DMA_DIR_TO_DEVICE   = 1 << 0,
    WASMOS_DMA_DIR_FROM_DEVICE = 1 << 1,
    WASMOS_DMA_DIR_BIDIR = WASMOS_DMA_DIR_TO_DEVICE | WASMOS_DMA_DIR_FROM_DEVICE
};

enum {
    WASMOS_DMA_STATUS_OK = 0,
    WASMOS_DMA_STATUS_DENY = -1,
    WASMOS_DMA_STATUS_INVALID = -2,
    WASMOS_DMA_STATUS_RANGE = -3,
    WASMOS_DMA_STATUS_UNAVAILABLE = -4
};

enum {
    WASMOS_DMA_SYNC_TO_DEVICE = 1,
    WASMOS_DMA_SYNC_FROM_DEVICE = 2,
    WASMOS_DMA_SYNC_BIDIR = 3
};

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
} wasmos_dma_window_t;

typedef struct __attribute__((packed)) {
    uint32_t direction_flags;
    uint32_t max_bytes;
    uint32_t window_count;
    uint32_t reserved0;
} wasmos_spawn_dma_caps_t;

typedef struct __attribute__((packed)) {
    uint32_t cap_flags;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint16_t irq_mask;
    uint16_t reserved0;
    wasmos_spawn_dma_caps_t dma;
    wasmos_dma_window_t windows[];
} wasmos_spawn_caps_v2_t;

#define WASMOS_SPAWN_CAPS_V2_SIZE(window_count) \
    (sizeof(wasmos_spawn_caps_v2_t) + ((uint32_t)(window_count) * (uint32_t)sizeof(wasmos_dma_window_t)))

enum {
    PROC_IPC_DMA_MAP_BORROW_REQ = 0x230,
    PROC_IPC_DMA_SYNC_BORROW_REQ = 0x231,
    PROC_IPC_DMA_UNMAP_BORROW_REQ = 0x232,
    PROC_IPC_DMA_BORROW_RESP = 0x2B0,
    PROC_IPC_DMA_BORROW_ERROR = 0x2BF
};

enum {
    WASMOS_IPC_FIELD_TYPE = 0,
    WASMOS_IPC_FIELD_REQUEST_ID = 1,
    WASMOS_IPC_FIELD_ARG0 = 2,
    WASMOS_IPC_FIELD_ARG1 = 3,
    WASMOS_IPC_FIELD_SOURCE = 4,
    WASMOS_IPC_FIELD_DESTINATION = 5,
    WASMOS_IPC_FIELD_ARG2 = 6,
    WASMOS_IPC_FIELD_ARG3 = 7
};

#endif
