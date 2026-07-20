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
     * caller must place path bytes at xfer buffer offset 0.
     * optional raw command argument text is placed at offset (path_len + 1).
     * arg0=reserved(0) arg1=path_len arg2=args_len arg3=reserved.
     * On success (app kind): PROC_IPC_RESP, arg0=child_pid, arg1=app_flags.
     * For service/driver kinds the PM delays the PROC_IPC_RESP until the child
     * calls PROC_IPC_NOTIFY_READY (behaves like SPAWN_PATH_SYNC internally). */
    PROC_IPC_SPAWN_PATH = 0x209,
    /* Spawn from explicit app path with I/O-port + IRQ capabilities:
     * caller must place path bytes at xfer buffer offset 0.
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
    PROC_IPC_SPAWN_CAPS_SYNC = 0x20D,
    PROC_IPC_SPAWN_PATH_SYNC = 0x20E,
    PROC_IPC_SPAWN_PATH_CAPS_SYNC = 0x20F,
    /* Descriptor-based broker subsystem registration.
     * arg0=offset(0) arg1=byte_len(sizeof(wasmos_subsystem_broker_register_desc_t))
     * arg2=reserved(0) arg3=reserved(0). */
    PROC_IPC_SUBSYSTEM_REGISTER_BROKER = 0x210,
    /* Descriptor-based exec handler registration.
     * arg0=offset(0) arg1=byte_len(sizeof(desc)+node_bytes)
     * arg2=reserved(0) arg3=reserved(0). */
    PROC_IPC_EXEC_HANDLER_REGISTER = 0x211,
    PROC_IPC_RESP = 0x280,
    PROC_IPC_ERROR = 0x2FF
};

enum {
    /* Broker spawn-plan handoff:
     * PM lends its xfer buffer to the broker read-only, writes a
     * wasmos_broker_spawn_plan_request_t into that borrowed view, then sends
     * this request with arg0=request_offset and arg1=request_size.
     *
     * The broker replies on msg->source with the same request_id. On success
     * arg0=plan_offset and arg1=plan_size in the broker's own xfer buffer. */
    PROC_BROKER_IPC_SPAWN_PLAN_REQ = 0x223,
    PROC_BROKER_IPC_SPAWN_PLAN_RESP = 0x2A3,
    PROC_BROKER_IPC_SPAWN_PLAN_ERROR = 0x2E3
};

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
    /* Class-based discovery (see docs/architecture/09-process-and-ipc.md).
     * LOOKUP_CLASS: enumerate every provider registered under a virtual class.
     *   req  arg0=buffer_id (class name NUL-terminated at offset 0 on input;
     *        PM overwrites it with a svc_class_entry_t[] on output),
     *        arg1=max_entries the buffer can hold;
     *   resp SVC_IPC_LOOKUP_CLASS_RESP arg0=provider count (may exceed
     *        max_entries; only min(count,max_entries) entries are written).
     * SUBSCRIBE_CLASS: receive existence events for a class.
     *   req  arg0=notify_endpoint (where SVC_IPC_CLASS_EVENT is delivered),
     *        arg1=buffer_id (class name NUL-terminated at offset 0);
     *   resp SVC_IPC_SUBSCRIBE_CLASS_RESP arg0=0.
     * CLASS_EVENT is pushed to a subscriber's notify_endpoint on add/remove/die:
     *        arg0=SVC_CLASS_EVENT_* arg1=instance arg2=endpoint arg3=pid. */
    SVC_IPC_LOOKUP_CLASS_REQ = 0x223,
    SVC_IPC_SUBSCRIBE_CLASS_REQ = 0x224,
    SVC_IPC_REGISTER_RESP = 0x2A0,
    SVC_IPC_LOOKUP_RESP = 0x2A1,
    SVC_IPC_LOOKUP_CLASS_RESP = 0x2A2,
    SVC_IPC_SUBSCRIBE_CLASS_RESP = 0x2A3,
    SVC_IPC_CLASS_EVENT = 0x2A4,
    SVC_IPC_ERROR = 0x2AF
};

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
    /* fs-manager -> backend pull: report kind/mount/unit. Reply RESP packs
     * arg0=kind, arg2=(mount_buffer_id<<12)|mount_len (backend owns the buffer
     * and borrows it READ to fs-manager), arg3=unit. Backends are discovered
     * via svc class FSMGR_BACKEND_CLASS, not a push, so fs-manager rebuilds its
     * backend set from the registry on (re)start. */
    FSMGR_IPC_BACKEND_INFO_REQ = 0x420,
    FSMGR_IPC_CLONE_CWD_REQ = 0x421,
    FSMGR_IPC_QUERY_MOUNTS_REQ = 0x422,
    FSMGR_IPC_BACKEND_INFO_RESP = 0x4A0,
    FSMGR_IPC_CLONE_CWD_RESP = 0x4A1,
    FSMGR_IPC_QUERY_MOUNTS_RESP = 0x4A2
};

/* Virtual class FS backends register under. Class instances must be unique per
 * provider, so backends encode (kind, unit) into the registry instance while
 * still reporting the plain kind over FSMGR_IPC_BACKEND_INFO_RESP arg0. */
#define FSMGR_BACKEND_CLASS "fs.backend"

enum { FSMGR_BACKEND_BOOT = 1, FSMGR_BACKEND_INIT = 2 };

#define FSMGR_BACKEND_INSTANCE(kind, unit) ((((uint32_t)(kind)) << 8) | ((uint32_t)(unit) & 0xFFu))

enum {
    FBTEXT_IPC_CELL_WRITE_REQ = 0x600,
    FBTEXT_IPC_CURSOR_SET_REQ = 0x601,
    FBTEXT_IPC_SCROLL_REQ = 0x602,
    FBTEXT_IPC_CLEAR_REQ = 0x603,
    FBTEXT_IPC_CONSOLE_MODE_REQ = 0x604,   /* arg0: 0=ring off, 1=ring on */
    FBTEXT_IPC_GEOMETRY_REQ = 0x605,       /* resp: arg0=cols arg1=rows */
    FBTEXT_IPC_GFX_OVERLAY_REQ = 0x606,    /* arg0: 0=unlock, 1=lock */
    FBTEXT_IPC_QUERY_CAPS_REQ = 0x607,     /* resp: arg0=FBTEXT_CAP_* bitmask */
    FBTEXT_IPC_QUERY_MODES_REQ = 0x608,    /* req: arg0=index, resp: arg0=w arg1=h arg2=stride */
    FBTEXT_IPC_SET_RESOLUTION_REQ = 0x609, /* req: arg0=w arg1=h */
    FBTEXT_IPC_RESP = 0x680,
    FBTEXT_IPC_ERROR = 0x6FF
};

enum { FBTEXT_CAP_SET_RESOLUTION = 1u << 0, FBTEXT_CAP_QUERY_MODES = 1u << 1 };

enum {
    VT_IPC_WRITE_REQ = 0x700, /* arg0[27:24]=byte_count(1-4), arg0[7:0]..arg3[7:0]=bytes */
    VT_IPC_READ_REQ = 0x701,
    VT_IPC_SET_ATTR_REQ = 0x702,
    VT_IPC_SWITCH_TTY = 0x703,
    VT_IPC_GET_ACTIVE_TTY = 0x704,
    VT_IPC_REGISTER_WRITER = 0x705,
    VT_IPC_SET_MODE_REQ = 0x706,
    VT_IPC_RESP = 0x780,
    VT_IPC_ERROR = 0x7FF
};

enum { VT_INPUT_MODE_RAW = 0, VT_INPUT_MODE_CANONICAL = 1 << 0, VT_INPUT_MODE_ECHO = 1 << 1 };

enum { KBD_IPC_SUBSCRIBE_REQ = 0x800, KBD_IPC_SUBSCRIBE_RESP = 0x880, KBD_IPC_KEY_NOTIFY = 0x801 };

enum {
    MOUSE_IPC_SUBSCRIBE_REQ = 0x810,
    MOUSE_IPC_SUBSCRIBE_RESP = 0x890,
    /* arg0=dx (signed 8-bit in low byte), arg1=dy (signed 8-bit in low byte),
     * arg2=buttons (bit0=left bit1=right bit2=middle), arg3=flags reserved. */
    MOUSE_IPC_MOVE_NOTIFY = 0x811
};

enum {
    RTC_IPC_READ_REQ = 0x820,
    RTC_IPC_SET_REQ = 0x821,
    RTC_IPC_READ_RESP = 0x8A0,
    RTC_IPC_SET_RESP = 0x8A1,
    RTC_IPC_ERROR = 0x8FF
};

enum {
    VIRTIO_SERIAL_IPC_QUERY_REQ = 0x830,
    VIRTIO_SERIAL_IPC_READ_REG32_REQ = 0x831,
    VIRTIO_SERIAL_IPC_WRITE_REG32_REQ = 0x832,
    VIRTIO_SERIAL_IPC_RESP = 0x8B0,
    VIRTIO_SERIAL_IPC_ERROR = 0x8BF
};

enum {
    DEVMGR_PUBLISH_DEVICE = 0x900,
    DEVMGR_PCI_SCAN_DONE = 0x901,
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
    DEVMGR_CAP_IO_PORT = 1 << 0,
    DEVMGR_CAP_MMIO_MAP = 1 << 1,
    DEVMGR_CAP_IRQ = 1 << 2,
    DEVMGR_CAP_DMA = 1 << 3
};

enum {
    NETDRV_IPC_LINK_GET = 0xA00,
    NETDRV_IPC_TX_FRAME = 0xA01,
    NETDRV_IPC_RX_POLL = 0xA02,
    NETDRV_IPC_STATS_GET = 0xA03,
    NETDRV_IPC_RX_FRAME_NOTIFY = 0xA04,
    /* Pushed to the interface subscriber when carrier changes. arg0=link_up,
     * arg1=driver status word, arg2=MTU. LINK_GET also establishes/refreshes
     * the subscriber, so old clients need no separate subscribe request. */
    NETDRV_IPC_LINK_NOTIFY = 0xA05,
    NETDRV_IPC_RESP = 0xA80,
    NETDRV_IPC_ERROR = 0xAFF
};

/* Generic hardware-RNG service protocol (virtual class "hrng"). Backend-neutral:
 * any entropy source (virtio-rng, RDRAND, ...) registers under class "hrng" and
 * speaks this. A client stages an output buffer via the xfer/FS-buffer API and
 * asks the provider to fill up to `len` bytes of entropy into it; libc layers
 * random-int / random-byte-array helpers on top of GET_BYTES. */
enum {
    HRNG_IPC_GET_BYTES_REQ = 0xC00, /* arg0=buffer_id, arg1=len (bytes requested) */
    HRNG_IPC_RESP = 0xC80,          /* arg0=bytes written into the buffer */
    HRNG_IPC_ERROR = 0xCFF          /* arg0=HRNG_STATUS_* */
};

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
    NET_IPC_SOCKET_OPEN = 0xB00,
    NET_IPC_BIND = 0xB01,
    NET_IPC_CONNECT = 0xB02,
    NET_IPC_SEND = 0xB03,
    NET_IPC_RECV = 0xB04,
    NET_IPC_CLOSE = 0xB05,
    NET_IPC_POLL = 0xB06,
    NET_IPC_IFADDR_ADD = 0xB07,
    NET_IPC_IFADDR_DEL = 0xB08,
    NET_IPC_IFADDR_LIST = 0xB09,
    NET_IPC_STACK_CREATE = 0xB0A,
    NET_IPC_STACK_DESTROY = 0xB0B,
    NET_IPC_STACK_SELECT = 0xB0C,
    NET_IPC_DATA_NOTIFY = 0xB0D,
    NET_IPC_TX_NOTIFY = 0xB0E,
    NET_IPC_RX_NOTIFY = 0xB0F,
    NET_IPC_RESP = 0xB80,
    NET_IPC_ERROR = 0xBFF
};

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
} net_socket_open_descriptor_v1_t;

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
