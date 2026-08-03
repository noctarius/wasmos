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
 *   0xA00–0xAFF  generic network-adapter drivers
 *   0xB00–0xBFF  network stack services
 *
 * All request/response pairs follow the pattern: REQ = base, RESP = base+0x80,
 * ERROR = base+0xFF.  Fields (type, request_id, source, destination, arg0..arg3)
 * match the ipc_message_t layout in the kernel. */
#ifndef WASMOS_DRIVER_ABI_H
#define WASMOS_DRIVER_ABI_H

#include <stdint.h>
#include "../../../abi/generated/c/wasmos_opcodes.h" /* generated opcode enums (abi/opcodes.yaml) */

#ifndef WASMOS_CONSOLE_RING_SHARED_H
#define WASMOS_CONSOLE_RING_SHARED_H
#define CONSOLE_RING_DATA_SIZE 4080u

typedef struct {
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    uint32_t capacity;
    uint32_t _pad;
    uint8_t data[CONSOLE_RING_DATA_SIZE];
} console_ring_t;
#endif

enum { PROC_MODULE_SOURCE_INITFS = 0, PROC_MODULE_SOURCE_FS = 1 };

/* arg0 flags for PROC_IPC_SPAWN_PATH (request). */
#define PROC_SPAWN_PATH_FLAG_DETACH (1u << 0) /* skip ready-wait even for service/driver */
/* Reap the child's process slot automatically when it exits, instead of leaving
 * it as a zombie until a waiter consumes its status.  Orthogonal to DETACH
 * (which is about ready-gating at spawn): set this for fire-and-forget one-shot
 * children that nobody PROC_IPC_WAITs on (bus enumerators, boot self-tests, …).
 * Must NOT be set when the spawner will PROC_IPC_WAIT for the exit status. */
#define PROC_SPAWN_PATH_FLAG_AUTOREAP (1u << 1)

enum {
    /* Distinct path-spawn failure reasons, returned as the rc in
     * PROC_IPC_ERROR.arg1 so a failed spawn reports WHY instead of a blanket
     * "exec failed".  Kept as small negative ints so they don't collide with
     * success (0). */
    PROC_SPAWN_ERR_BAD_ENDPOINT = -10, /* request endpoint owner lookup failed */
    PROC_SPAWN_ERR_NO_CALLER = -11,    /* caller process/context not found */
    PROC_SPAWN_ERR_BAD_PATH = -12,     /* fs endpoint missing or bad path length */
    PROC_SPAWN_ERR_CALLER_FSBUF = -13, /* caller xfer buffer missing / path too big */
    PROC_SPAWN_ERR_ARGS_TOOBIG = -14,  /* args exceed the xfer buffer */
    PROC_SPAWN_ERR_NO_PM_FSBUF = -15,  /* PM xfer buffer missing */
    PROC_SPAWN_ERR_FS_READ = -16,      /* reading the app blob from FS failed */
    PROC_SPAWN_ERR_SPAWN_FAILED = -17, /* process create/start failed (e.g. no free slot) */
    PROC_SPAWN_ERR_BROKER_IPC = -18,   /* broker plan IPC transport/reply failed */
    PROC_SPAWN_ERR_BROKER_PLAN = -19,  /* broker replied with malformed/unsupported plan */
    PROC_SPAWN_ERR_BROKER_DEFERRED =
        -20 /* valid broker plan returned; PM launch step still deferred */
};

enum {
    /* Non-path PM IPC failures used by spawn/module-meta service entry points
     * that previously collapsed to raw -1/-2 error responses. */
    PROC_PM_ERR_BUSY = -40,            /* PM already has an incompatible in-flight operation */
    PROC_PM_ERR_BAD_ENDPOINT = -41,    /* source endpoint owner lookup failed */
    PROC_PM_ERR_NO_CALLER = -42,       /* source endpoint owner has no live process */
    PROC_PM_ERR_INVALID_NAME = -43,    /* packed module/service name was empty or invalid */
    PROC_PM_ERR_INVALID_MODULE = -44,  /* requested module or descriptor was invalid */
    PROC_PM_ERR_FS_UNAVAILABLE = -45,  /* PM filesystem service channel was unavailable */
    PROC_PM_ERR_FS_REQUEST = -46,      /* PM could not issue the filesystem read request */
    PROC_PM_ERR_BAD_PATH = -47,        /* explicit path input was empty or invalid */
    PROC_PM_ERR_PATH_RESOLVE = -48,    /* path-based resolve/classify/reload failed */
    PROC_PM_ERR_SPAWN_FAILED = -49,    /* PM failed to create or prepare the child process */
    PROC_PM_ERR_CAPS_APPLY = -50,      /* capability profile application failed after spawn */
    PROC_PM_ERR_BAD_CAPS = -51,        /* capability payload or compact cap fields were invalid */
    PROC_PM_ERR_BAD_USER_PTR = -52,    /* supplied user pointer could not be resolved safely */
    PROC_PM_ERR_USER_COPY = -53,       /* mm_copy_from_user failed for PM input payload */
    PROC_PM_ERR_META_LOOKUP = -54,     /* module metadata lookup failed */
    PROC_PM_ERR_META_NOT_DRIVER = -55, /* requested module metadata was not for a driver */
    PROC_PM_ERR_META_BAD_INDEX = -56,  /* requested driver match index was out of range */
    PROC_PM_ERR_META_BAD_SOURCE = -57, /* unsupported module metadata source selector */
    PROC_PM_ERR_CALLER_FSBUF = -58,    /* caller filesystem transfer buffer was missing/invalid */
    PROC_PM_ERR_REPLY_SEND = -59,      /* PM failed to send the final IPC response */
    PROC_PM_ERR_FS_REPLY = -60,        /* PM received an unexpected filesystem reply */
    PROC_PM_ERR_BAD_BROKER = -61,      /* broker registration payload or endpoint was invalid */
    PROC_PM_ERR_BAD_HANDLER = -62,     /* exec-handler registration payload was invalid */
    PROC_PM_ERR_SUBSYSTEM_REG = -63,   /* subsystem broker registration failed */
    PROC_PM_ERR_HANDLER_REG = -64,     /* exec-handler registration failed */
    PROC_PM_ERR_NOT_AUTHORIZED = -65,  /* caller lacks the subsystem.register capability */
    PROC_PM_ERR_NO_PM_FSBUF = -66      /* PM could not acquire its own xfer buffer */
};

/* Distinct shmem map/map_auto failure reasons, returned (as a negative int) by
 * wasmos_shmem_map / wasmos_shmem_map_auto instead of a blanket -1, so a failed
 * map reports WHY.  Mirrored in both runtimes (warp/link.cpp, wasm3/link.c).
 * Distinct -30 range so they don't collide with PROC_SPAWN_ERR_* (-10..-20). */
#define SHMEM_ERR_BAD_ARGS (-30)  /* id/size invalid or size not page-aligned */
#define SHMEM_ERR_NO_CAP (-31)    /* caller lacks the DMA capability / no context */
#define SHMEM_ERR_BAD_ID (-32)    /* shmem id unknown / no backing pages */
#define SHMEM_ERR_BAD_SIZE (-33)  /* requested size smaller than the shared region */
#define SHMEM_ERR_UNALIGNED (-34) /* fixed offset cannot yield a page-aligned host addr */
#define SHMEM_ERR_NO_WINDOW (-35) /* no free page-aligned window fits in linear memory */
#define SHMEM_ERR_MAP (-36)       /* paging/linear-memory mapping step failed */

/* Distinct filesystem failure reasons returned (as a negative int in the
 * FS_IPC_ERROR / FS_IPC_RESP arg0) by the FAT backend instead of a blanket -1,
 * so a failed FS op reports WHY.  fs-manager relays the backend's arg0 to the
 * client unchanged.  Distinct -70 range so they don't collide with
 * PROC_SPAWN_ERR_* (-10..-20), SHMEM_ERR_* (-30..-36) or PROC_PM_ERR_* (-40..-66). */
enum {
    FS_ERR_BAD_ARGS = -70,      /* invalid flags/args (len 0, bad access mode, reserved arg set) */
    FS_ERR_PATH_TOO_LONG = -71, /* path length exceeds the path buffer or the xfer buffer */
    FS_ERR_BUFFER = -72,        /* xfer-buffer read/write/size call failed */
    FS_ERR_TRANSLATE = -73,     /* vfs path translation failed / path routed to init overlay */
    FS_ERR_NOT_FOUND = -74,     /* path component or target entry does not exist */
    FS_ERR_IS_DIR = -75,        /* target is a directory where a file was required */
    FS_ERR_NOT_DIR = -76,       /* a path component that must be a directory is not one */
    FS_ERR_EXISTS = -77,        /* create target already exists (fail-if-exists) */
    FS_ERR_NOT_EMPTY = -78,     /* rmdir target directory is not empty */
    FS_ERR_NO_FD = -79,         /* open-file table is full */
    FS_ERR_BUSY = -80,          /* backend has no free op-context slot (retryable) */
    FS_ERR_IO = -81,            /* block-device I/O error */
    FS_ERR_NOT_READY = -82,     /* mount/backend not ready */
    FS_ERR_NO_SPACE = -83,      /* no free cluster or no free directory slot (disk full) */
    FS_ERR_NAME = -84,          /* invalid name (LFN validation / short-name encode failed) */
    FS_ERR_ACCESS = -85,        /* access-mode violation (e.g. read on a write-only fd) */
    FS_ERR_RANGE = -86,         /* seek/offset out of range */
    FS_ERR_UNSUPPORTED = -87,   /* unknown/unsupported request type */
    FS_ERR_OPEN = -88,          /* operation forbidden on a currently-open file */
    FS_ERR_CORRUPT = -89        /* on-disk structure inconsistency detected */
};

/* Flags returned in arg1 of PROC_IPC_RESP for PROC_IPC_SPAWN_PATH.
 * Mirror of WASMOS_APP_FLAG_* in the kernel's wasmos_app.h. */
#define WASMOS_SPAWN_FLAG_DRIVER (1u << 0)
#define WASMOS_SPAWN_FLAG_SERVICE (1u << 1)
#define WASMOS_SPAWN_FLAG_APP (1u << 2)

#define WASMOS_BROKER_SPAWN_PLAN_VERSION 1u

#define WASMOS_SUBSYSTEM_TAG_LEN 8u
#define WASMOS_EXEC_HANDLER_NAME_LEN 32u
#define WASMOS_EXEC_MATCH_TEXT_LEN 32u
#define WASMOS_EXEC_MATCH_MAX_BYTES 16u
#define WASMOS_EXEC_MATCH_MAX_NODES 16u

typedef enum {
    WASMOS_EXEC_MATCH_PREFIX = 0,
    WASMOS_EXEC_MATCH_EXTENSION = 1,
    WASMOS_EXEC_MATCH_FILENAME = 2,
    WASMOS_EXEC_MATCH_AND = 3,
    WASMOS_EXEC_MATCH_OR = 4,
    WASMOS_EXEC_MATCH_NOT = 5,
} wasmos_exec_match_kind_t;

typedef struct {
    wasmos_exec_match_kind_t kind;
    uint16_t left_index;
    uint16_t right_index;
    uint8_t value_len;
    union {
        uint8_t prefix[WASMOS_EXEC_MATCH_MAX_BYTES];
        char text[WASMOS_EXEC_MATCH_TEXT_LEN + 1];
    } value;
} wasmos_exec_match_node_t;

enum {
    WASMOS_BROKER_PLAN_KIND_NONE = 0,
    /* Broker resolved the guest workload to a built-in `.wap` path that PM
     * can later launch through the ordinary path-based spawn flow. */
    WASMOS_BROKER_PLAN_KIND_WAP_PATH = 1
};

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t spawn_flags;
    uint32_t blob_offset;
    uint32_t blob_size;
    uint32_t path_offset;
    uint32_t path_len;
    uint32_t args_offset;
    uint32_t args_len;
    uint32_t handler_name_offset;
    uint32_t handler_name_len;
    char request_tag[9];
    char runtime_tag[9];
    char broker_name[9];
} wasmos_broker_spawn_plan_request_t;

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t plan_kind;
    uint32_t plan_flags;
    uint32_t host_path_offset;
    uint32_t host_path_len;
    uint32_t host_args_offset;
    uint32_t host_args_len;
    char request_tag[9];
    char runtime_tag[9];
} wasmos_broker_spawn_plan_response_t;

#define WASMOS_SUBSYSTEM_REGISTER_BROKER_DESC_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t broker_endpoint;
    uint32_t flags;
    uint8_t uses_wasm_payload;
    uint8_t needs_runtime_lock;
    uint8_t gates_ready_for_services;
    uint8_t reserved0;
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char runtime_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char broker_name[WASMOS_SUBSYSTEM_TAG_LEN + 1];
} wasmos_subsystem_broker_register_desc_t;

#define WASMOS_EXEC_HANDLER_REGISTER_DESC_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t priority;
    uint32_t max_probe_bytes;
    uint32_t node_count;
    uint32_t root_index;
    char request_tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char handler_name[WASMOS_EXEC_HANDLER_NAME_LEN + 1];
} wasmos_exec_handler_register_desc_t;

#define WASMOS_SVC_REGISTER_DESC_VERSION 2u
#define WASMOS_SVC_NAME_MAX 36u
#define WASMOS_SVC_CLASS_MAX 16u /* incl. NUL; keep == SVC_CLASS_NAME_MAX */

/* Existence-event kinds carried in SVC_IPC_CLASS_EVENT arg0. Keep in sync with
 * SVC_CLASS_EVENT_* in src/kernel/include/service_class_registry.h. */
#define SVC_CLASS_EVENT_ADD 1u
#define SVC_CLASS_EVENT_REMOVE 2u

/* Register descriptor written to the xfer buffer for SVC_IPC_REGISTER_DESC_REQ.
 * Extensible: bump WASMOS_SVC_REGISTER_DESC_VERSION and append fields.  Mirror
 * this layout in any non-C binding that registers services.  v2 appended the
 * class/instance fields; PM accepts a v1-length descriptor (no class) for
 * back-compat by checking the byte length, not just the version. */
typedef struct {
    uint32_t version;               /* = WASMOS_SVC_REGISTER_DESC_VERSION */
    uint32_t service_endpoint;      /* endpoint clients send requests to */
    uint32_t flags;                 /* reserved, 0 */
    char name[WASMOS_SVC_NAME_MAX]; /* NUL-terminated service name */
    /* v2+ (present iff the descriptor byte length covers these fields): */
    uint32_t instance;                     /* provider instance index within the class */
    char class_name[WASMOS_SVC_CLASS_MAX]; /* NUL-term; "" = no class */
} svc_register_desc_t;

/* v1 descriptor length: fields up to and including name[], no class/instance. */
#define WASMOS_SVC_REGISTER_DESC_V1_BYTES (3u * (uint32_t)sizeof(uint32_t) + WASMOS_SVC_NAME_MAX)

/* One resolved provider returned by SVC_IPC_LOOKUP_CLASS_REQ (wire layout;
 * matches service_class_provider_t). */
typedef struct {
    uint32_t instance;
    uint32_t endpoint;
    uint32_t pid;
} svc_class_entry_t;

enum { PROC_STATUS_UNKNOWN = 0, PROC_STATUS_RUNNING = 1, PROC_STATUS_ZOMBIE = 2 };

/* Virtual class FS backends register under. Class instances must be unique per
 * provider, so backends encode (kind, unit) into the registry instance while
 * still reporting the plain kind over FSMGR_IPC_BACKEND_INFO_RESP arg0. */
#define FSMGR_BACKEND_CLASS "fs.backend"

enum { FSMGR_BACKEND_BOOT = 1, FSMGR_BACKEND_INIT = 2 };

#define FSMGR_BACKEND_INSTANCE(kind, unit) ((((uint32_t)(kind)) << 8) | ((uint32_t)(unit) & 0xFFu))

/* One cell in a shared FBTEXT_IPC_BLIT grid buffer.  Layout is identical to the
 * framebuffer driver's fbtext_cell_t and the VT's vt_cell_t (8 bytes), so both
 * sides copy grids without per-cell conversion. */
typedef struct {
    uint32_t ch;  /* Unicode codepoint; 0 or ' ' = blank */
    uint8_t fg;   /* 4-bit foreground palette index */
    uint8_t bg;   /* 4-bit background palette index */
    uint8_t attr; /* reserved (bold/underline/blink) */
    uint8_t _pad;
} fbtext_blit_cell_t;

enum { FBTEXT_CAP_SET_RESOLUTION = 1u << 0, FBTEXT_CAP_QUERY_MODES = 1u << 1 };

/* Serial driver subscription: a client (the vt service) registers to receive
 * COM1 RX bytes as VT_IPC_SERIAL_INPUT_REQ pushes.  Mirrors the keyboard
 * driver's subscribe/notify pattern. */

enum { VT_INPUT_MODE_RAW = 0, VT_INPUT_MODE_CANONICAL = 1 << 0, VT_INPUT_MODE_ECHO = 1 << 1 };

enum {
    DEVMGR_CAP_IO_PORT = 1 << 0,
    DEVMGR_CAP_MMIO_MAP = 1 << 1,
    DEVMGR_CAP_IRQ = 1 << 2,
    DEVMGR_CAP_DMA = 1 << 3
};

/* Generic hardware-RNG service protocol (virtual class "hrng"). Backend-neutral:
 * any entropy source (virtio-rng, RDRAND, ...) registers under class "hrng" and
 * speaks this. A client stages an output buffer via the xfer/FS-buffer API and
 * asks the provider to fill up to `len` bytes of entropy into it; libc layers
 * random-int / random-byte-array helpers on top of GET_BYTES. */

enum {
    HRNG_STATUS_OK = 0,
    HRNG_STATUS_INVALID = -2,
    HRNG_STATUS_NOT_READY = -3,
    HRNG_STATUS_IO_ERROR = -5,
    HRNG_STATUS_TIMEOUT = -9
};

/* Largest single GET_BYTES request the provider fills in one round-trip (its DMA
 * pool is one page). Clients wanting more loop. */
#define HRNG_MAX_BYTES_PER_REQ 4096u

enum {
    NET_STATUS_OK = 0,
    NET_STATUS_WOULD_BLOCK = -1,
    NET_STATUS_INVALID = -2,
    NET_STATUS_NOT_READY = -3,
    NET_STATUS_DENIED = -4,
    NET_STATUS_IO_ERROR = -5,
    NET_STATUS_QUEUE_FULL = -6,
    NET_STATUS_NO_MEM = -7,
    NET_STATUS_ADDR_IN_USE = -8,
    NET_STATUS_TIMEOUT = -9
};

/* Socket control-plane constants. Socket payload is never carried by IPC:
 * SOCKET_OPEN arg0=descriptor buffer_id, arg1=descriptor borrow_id,
 * arg2=descriptor bytes, arg3=0. The descriptor transfers persistent grants
 * for the client-owned TX/RX SPSC rings, while TX/RX_NOTIFY are
 * empty-to-non-empty doorbells. */
enum {
    NET_SOCKET_AF_INET = 2,
    NET_SOCKET_AF_INET6 = 10,
    NET_SOCKET_STREAM = 1,
    NET_SOCKET_DGRAM = 2
};

#define NET_SOCKET_OPEN_DESCRIPTOR_VERSION 1u

/* net_socket_open_descriptor_v1_t.flags bits. */
#define NET_SOCKET_OPEN_FLAG_TLS 1u /* stream socket is wrapped in TLS (altcp_tls) */

/* Maximum SNI / certificate-verification hostname carried in the open descriptor
 * (a DNS name is at most 253 bytes; the extra room leaves space for the NUL that
 * mbedtls_ssl_set_hostname needs). Milestone C: for a TLS socket this is the name
 * checked against the server certificate CN/SAN and sent in the SNI extension. */
#define NET_SOCKET_SNI_MAX 256u

/* Datagram ring records carry endpoint metadata as well as payload. A client
 * writes destination fields for an unconnected sendto; RX records contain the
 * source fields supplied by lwIP. Connected sockets may leave destination at
 * zero and use their connected peer. */
#define NET_UDP_DATAGRAM_RECORD_VERSION 1u
#define NET_UDP_DATAGRAM_FLAG_DESTINATION 1u

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t flags;
    uint32_t addr_v4;
    uint16_t port;
    uint16_t payload_bytes;
} net_udp_datagram_record_v1_t;

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t bytes;
    uint32_t family;
    uint32_t type;
    uint32_t stack_id;
    uint32_t flags;
    uint32_t tx_buffer_id;
    uint32_t tx_borrow_id;
    uint32_t tx_bytes;
    uint32_t rx_buffer_id;
    uint32_t rx_borrow_id;
    uint32_t rx_bytes;
    /* SNI / certificate-verification hostname for a TLS stream socket (milestone
     * C). sni_len is the byte count (<= NET_SOCKET_SNI_MAX - 1, not NUL
     * terminated in-band); 0 for plain TCP and UDP. */
    uint16_t sni_len;
    uint8_t sni[NET_SOCKET_SNI_MAX];
} net_socket_open_descriptor_v1_t;

/* Interface-address record for NET_IPC_IFADDR_ADD/DEL/LIST. ADD carries one
 * record in a borrowed xfer buffer (arg0=buffer_id, arg1=borrow_id,
 * arg2=bytes); LIST fills an array of these into a client buffer and returns
 * the count in the reply arg0. All IPv4 words are network byte order. */
#define NET_IFADDR_RECORD_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t if_index;
    uint32_t addr_v4;
    uint32_t netmask_v4;
    uint32_t gateway_v4;
    uint32_t flags; /* bit0: link up, bit1: administratively up */
} net_ifaddr_record_v1_t;

#define NET_IFADDR_FLAG_LINK_UP 1u
#define NET_IFADDR_FLAG_ADMIN_UP 2u
#define NET_IFADDR_FLAG_DHCP 4u /* address is (or is being) assigned by DHCP */

typedef struct {
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_drops;
    uint32_t tx_drops;
    uint32_t rx_errors;
    uint32_t tx_errors;
} netdrv_stats_t;

enum {
    WASMOS_DMA_DIR_TO_DEVICE = 1 << 0,
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

enum { WASMOS_DMA_SYNC_TO_DEVICE = 1, WASMOS_DMA_SYNC_FROM_DEVICE = 2, WASMOS_DMA_SYNC_BIDIR = 3 };

/* Cache policy for wasmos_region_alloc (driver-owned pinned DMA regions). */
enum {
    WASMOS_REGION_CACHE_WB = 0, /* write-back: coherent, virtqueue rings on x86 */
    WASMOS_REGION_CACHE_WC = 1  /* write-combining: framebuffer/scanout (TODO) */
};

/* Trigger/polarity flags for wasmos_irq_configure. Default (no flags) is
 * edge-triggered active-high (ISA). PCI INTx lines are level + active-low. */
enum { WASMOS_IRQ_TRIGGER_LEVEL = 1 << 0, WASMOS_IRQ_POLARITY_LOW = 1 << 1 };

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

#define WASMOS_SPAWN_CAPS_V2_SIZE(window_count)                                                    \
    (sizeof(wasmos_spawn_caps_v2_t) +                                                              \
     ((uint32_t)(window_count) * (uint32_t)sizeof(wasmos_dma_window_t)))

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
