/* wasmos_driver_abi.h - Shared IPC message type constants used by all drivers and services.
 *
 * Each subsystem owns a range of IPC type values:
 *   0x100–0x1FF  chardev (character device read/write)
 *   0x200–0x2FF  process manager (spawn, wait, kill, service registration)
 *   0x300–0x3FF  block device (ATA/sector read/write)
 *   0x400–0x4FF  filesystem (open/read/stat/write/unlink/mkdir/readdir)
 *   0x420–0x43F  fs-manager VFS router (backend registration, mount queries)
 *   0x600–0x6FF  framebuffer text layer
 *   0x700–0x7FF  virtual terminal
 *   0x800–0x8FF  keyboard, mouse, RTC, virtio-serial
 *   0x900–0x9FF  device manager
 *
 * All request/response pairs follow the pattern: REQ = base, RESP = base+0x80,
 * ERROR = base+0xFF.  Fields (type, request_id, source, destination, arg0..arg3)
 * match the ipc_message_t layout in the kernel. */
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
    /* Spawn from explicit app path:
     * caller must place path bytes at FS buffer offset 0.
     * optional raw command argument text is placed at offset (path_len + 1).
     * arg0=reserved(0) arg1=path_len arg2=args_len arg3=reserved.
     * On success (app kind): PROC_IPC_RESP, arg0=child_pid, arg1=app_flags.
     * For service/driver kinds the PM delays the PROC_IPC_RESP until the child
     * calls PROC_IPC_NOTIFY_READY (behaves like SPAWN_PATH_SYNC internally). */
    PROC_IPC_SPAWN_PATH = 0x209,
    /* Spawn from explicit app path with I/O-port + IRQ capabilities:
     * caller must place path bytes at FS buffer offset 0.
     * arg0=((irq_mask<<16)|(cap_flags&0xFFFF)) arg1=path_len
     * arg2=((io_port_max<<16)|io_port_min)     arg3=reserved. */
    PROC_IPC_SPAWN_PATH_CAPS = 0x20A,
    /* Spawn by module index and block until the child calls NOTIFY_READY (or
     * first blocks on IPC as an implicit signal), or until the timeout expires.
     * arg0=module_index  arg1=timeout_ms (0 = wait forever)
     * arg2=reserved(0)   arg3=reserved(0).
     * On success: PROC_IPC_RESP, arg0=child_pid.
     * On timeout or child death before ready: PROC_IPC_ERROR, arg1=error_code. */
    PROC_IPC_SPAWN_SYNC = 0x20B,
    /* Service sends this to proc_endpoint when it has finished initialising and
     * is ready to accept requests.  Fire-and-forget — no reply is sent back.
     * arg0..arg3 = reserved(0). */
    PROC_IPC_NOTIFY_READY = 0x20C,
    /* Sync variants of SPAWN_CAPS / SPAWN_PATH / SPAWN_PATH_CAPS.
     * Same cap/path encoding as their async counterparts, with one arg
     * repurposed for timeout_ms:
     *   SPAWN_CAPS_SYNC:      arg0=module_index arg1=cap_flags arg2=io_packed
     *                         arg3=(irq_mask&0xFFFF)|((timeout_ms&0xFFFF)<<16)
     *   SPAWN_PATH_SYNC:      path at FS buf[0], arg0=0 arg1=path_len
     *                         arg2=0 arg3=timeout_ms
     *   SPAWN_PATH_CAPS_SYNC: path at FS buf[0],
     *                         arg0=(irq<<16)|cap_flags arg1=path_len
     *                         arg2=io_packed           arg3=timeout_ms
     * On success: PROC_IPC_RESP, arg0=child_pid.
     * On timeout or child death before ready: PROC_IPC_ERROR. */
    PROC_IPC_SPAWN_CAPS_SYNC      = 0x20D,
    PROC_IPC_SPAWN_PATH_SYNC      = 0x20E,
    PROC_IPC_SPAWN_PATH_CAPS_SYNC = 0x20F,
    PROC_IPC_RESP = 0x280,
    PROC_IPC_ERROR = 0x2FF
};

enum {
    PROC_MODULE_SOURCE_INITFS = 0,
    PROC_MODULE_SOURCE_FS = 1
};

/* arg0 flags for PROC_IPC_SPAWN_PATH (request). */
#define PROC_SPAWN_PATH_FLAG_DETACH (1u << 0) /* skip ready-wait even for service/driver */

/* Distinct spawn failure reasons, returned as the rc in PROC_IPC_ERROR.arg1 so
 * a failed spawn reports WHY instead of a blanket "exec failed".  Kept as small
 * negative ints so they don't collide with success (0). */
#define PROC_SPAWN_ERR_BAD_ENDPOINT (-10) /* request endpoint owner lookup failed */
#define PROC_SPAWN_ERR_NO_CALLER    (-11) /* caller process/context not found */
#define PROC_SPAWN_ERR_BAD_PATH     (-12) /* fs endpoint missing or bad path length */
#define PROC_SPAWN_ERR_CALLER_FSBUF (-13) /* caller xfer buffer missing / path too big */
#define PROC_SPAWN_ERR_ARGS_TOOBIG  (-14) /* args exceed the xfer buffer */
#define PROC_SPAWN_ERR_NO_PM_FSBUF  (-15) /* PM xfer buffer missing */
#define PROC_SPAWN_ERR_FS_READ      (-16) /* reading the app blob from FS failed */
#define PROC_SPAWN_ERR_SPAWN_FAILED (-17) /* process create/start failed (e.g. no free slot) */

/* Flags returned in arg1 of PROC_IPC_RESP for PROC_IPC_SPAWN_PATH.
 * Mirror of WASMOS_APP_FLAG_* in the kernel's wasmos_app.h. */
#define WASMOS_SPAWN_FLAG_DRIVER  (1u << 0)
#define WASMOS_SPAWN_FLAG_SERVICE (1u << 1)
#define WASMOS_SPAWN_FLAG_APP     (1u << 2)

enum {
    /* Legacy arg-packed register (reply lands on the service endpoint).
     * TODO: migrate the remaining senders (AssemblyScript rtc/mouse/keyboard,
     * native zig libsys) to SVC_IPC_REGISTER_DESC_REQ and remove this path. */
    SVC_IPC_REGISTER_REQ = 0x220,
    SVC_IPC_LOOKUP_REQ = 0x221,
    /* Descriptor-based register: the request payload is a svc_register_desc_t
     * placed by the caller at FS-buffer offset 0; arg0=offset(0), arg1=byte len.
     * msg->source is a DEDICATED reply endpoint (not the service endpoint), so
     * the SVC_IPC_REGISTER_RESP cannot collide with serve traffic on the service
     * endpoint.  This replaces the arg-packed SVC_IPC_REGISTER_REQ, whose 16-byte
     * name consumed all four args and forced the reply onto the serve endpoint
     * (a latent races that deadlocked boot once PM stopped busy-polling). */
    SVC_IPC_REGISTER_DESC_REQ = 0x222,
    SVC_IPC_REGISTER_RESP = 0x2A0,
    SVC_IPC_LOOKUP_RESP = 0x2A1,
    SVC_IPC_ERROR = 0x2AF
};

#define WASMOS_SVC_REGISTER_DESC_VERSION 1u
#define WASMOS_SVC_NAME_MAX 36u

/* Register descriptor written to the FS buffer for SVC_IPC_REGISTER_DESC_REQ.
 * Extensible: bump WASMOS_SVC_REGISTER_DESC_VERSION and append fields.  Mirror
 * this layout in any non-C binding that registers services. */
typedef struct {
    uint32_t version;          /* = WASMOS_SVC_REGISTER_DESC_VERSION */
    uint32_t service_endpoint; /* endpoint clients send requests to */
    uint32_t flags;            /* reserved, 0 */
    char     name[WASMOS_SVC_NAME_MAX]; /* NUL-terminated service name */
} svc_register_desc_t;

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
    FS_IPC_READ_PATH_REQ = 0x414,
    FS_IPC_RESP = 0x480,
    FS_IPC_STREAM = 0x481,
    FS_IPC_ERROR = 0x4FF
};

enum {
    FSMGR_IPC_REGISTER_BACKEND_REQ = 0x420,
    FSMGR_IPC_CLONE_CWD_REQ = 0x421,
    FSMGR_IPC_QUERY_MOUNTS_REQ = 0x422,
    FSMGR_IPC_REGISTER_BACKEND_RESP = 0x4A0,
    FSMGR_IPC_CLONE_CWD_RESP = 0x4A1,
    FSMGR_IPC_QUERY_MOUNTS_RESP = 0x4A2
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
    FBTEXT_IPC_GFX_OVERLAY_REQ = 0x606,  /* arg0: 0=unlock, 1=lock */
    FBTEXT_IPC_QUERY_CAPS_REQ  = 0x607,  /* resp: arg0=FBTEXT_CAP_* bitmask */
    FBTEXT_IPC_QUERY_MODES_REQ = 0x608,  /* req: arg0=index, resp: arg0=w arg1=h arg2=stride */
    FBTEXT_IPC_SET_RESOLUTION_REQ = 0x609, /* req: arg0=w arg1=h */
    FBTEXT_IPC_RESP            = 0x680,
    FBTEXT_IPC_ERROR           = 0x6FF
};

enum {
    FBTEXT_CAP_SET_RESOLUTION = 1u << 0,
    FBTEXT_CAP_QUERY_MODES    = 1u << 1
};

enum {
    VT_IPC_WRITE_REQ    = 0x700, /* arg0[27:24]=byte_count(1-4), arg0[7:0]..arg3[7:0]=bytes */
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
    MOUSE_IPC_SUBSCRIBE_REQ  = 0x810,
    MOUSE_IPC_SUBSCRIBE_RESP = 0x890,
    /* arg0=dx (signed 8-bit in low byte), arg1=dy (signed 8-bit in low byte),
     * arg2=buttons (bit0=left bit1=right bit2=middle), arg3=flags reserved. */
    MOUSE_IPC_MOVE_NOTIFY    = 0x811
};

enum {
    RTC_IPC_READ_REQ  = 0x820,
    RTC_IPC_SET_REQ   = 0x821,
    RTC_IPC_READ_RESP = 0x8A0,
    RTC_IPC_SET_RESP  = 0x8A1,
    RTC_IPC_ERROR     = 0x8FF
};

enum {
    VIRTIO_SERIAL_IPC_QUERY_REQ       = 0x830,
    VIRTIO_SERIAL_IPC_READ_REG32_REQ  = 0x831,
    VIRTIO_SERIAL_IPC_WRITE_REG32_REQ = 0x832,
    VIRTIO_SERIAL_IPC_RESP            = 0x8B0,
    VIRTIO_SERIAL_IPC_ERROR           = 0x8BF
};

enum {
    DEVMGR_PUBLISH_DEVICE = 0x900,
    DEVMGR_PCI_SCAN_DONE  = 0x901,
    DEVMGR_QUERY_MOUNT_REQ = 0x902,
    DEVMGR_PUBLISH_BLOCK_DEVICE = 0x903,
    DEVMGR_QUERY_BLOCK_MOUNT_REQ = 0x904,
    /* ISA/ACPI devices: bus=0xFF in PUBLISH_DEVICE marks a non-PCI device;
     * device_id field carries the I/O base address for serial (class 0x07). */
    DEVMGR_ACPI_SCAN_DONE = 0x905,
    DEVMGR_MOUNT_INFO = 0x980,
    DEVMGR_BLOCK_MOUNT_INFO = 0x982,
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
