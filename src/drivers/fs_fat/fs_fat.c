/* fs_fat.c - FAT backend service: the event reactor.
 *
 * Owns the driver's singleton state (block layer, mount geometry, open-file
 * pool) and a fixed pool + FIFO of in-flight operation contexts.  Accepts many
 * client requests but drives ONE active op at a time to completion (the single
 * block buffer serializes physical I/O); each op is a resumable coroutine
 * advanced across block-I/O completions.  The reactor loop blocks only in its
 * top-level select_wait; the one-time init handshakes above it are synchronous.
 *
 * The intricate FAT logic lives in fat_geom/fat_name/fat_alloc/fat_dir/fat_file;
 * this file is only the transport + scheduling glue. */
#include <stdint.h>
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#include "fat_block.h"
#include "fat_dir.h"
#include "fat_file.h"
#include "fat_geom.h"
#include "fat_types.h"
#include "fat_util.h"

/* --- Driver singleton state (owned here; passed by pointer to the modules). --- */
static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1; /* the fs.backend service endpoint (clients) */

static fat_block_t g_blk;
static fat_mount_t g_mnt;
static fat_open_pool_t g_pool;

/* Mount identity reported to fs-manager on each backend-info pull. */
static int32_t g_mount_bid = -1;
static int32_t g_mount_len = 0;
static uint8_t g_mount_unit = 0;
static int32_t g_requested_unit = -1;
/* Which backend serves the disk this instance was spawned for, as the device
 * manager named it. A unit alone does not identify a disk -- it is
 * backend-local -- so both halves are needed to find the right server. */
static uint8_t g_requested_backend = (uint8_t)BLOCK_BACKEND_UNKNOWN;
/* The `block` class instance of this driver's disk: the fingerprint of the
 * canonical id the device manager passed in `id=`. It is both how the provider
 * is found in the class registry and how the provider is told which of its disks
 * is meant, since several instances may share one endpoint. */
static uint32_t g_requested_instance = 0u;

/* In-flight op pool + FIFO of queued (not-yet-active) op indices. */
static fat_op_ctx_t g_ops[FAT_MAX_INFLIGHT];
static uint32_t g_fifo[FAT_MAX_INFLIGHT];
static uint32_t g_fifo_head = 0;
static uint32_t g_fifo_tail = 0;
static uint32_t g_fifo_len = 0;
static fat_op_ctx_t* g_active = 0;

/* Park the driver permanently after a fatal init failure.  Never returns: it
 * waits on a private endpoint nobody sends to, keeping the process alive instead
 * of exiting.  Only when the endpoint itself cannot be created does the loop
 * degenerate into a spin. */
static void fat_stall(void) {
    int32_t ep = wasmos_ipc_create_endpoint();
    for (;;) {
        if (ep >= 0) {
            (void)wasmos_ipc_select_one(ep);
        }
    }
}

/* --- init-time handshakes (one-time, pre-reactor; synchronous by nature). --- */

/* Startup-argument blob this driver reads. Large enough for
 * `driver=<name> unit=<n> id=<canonical id>`, whose id alone may be
 * BLOCK_DESCRIPTOR_ID_MAX bytes; the process manager truncates at
 * WASMOS_STARTUP_ARGS_MAX, so nothing longer can arrive anyway. */
#define FAT_STARTUP_ARGS_MAX 128

static int32_t fat_parse_requested_unit(void) {
    char args[FAT_STARTUP_ARGS_MAX];
    char* end = 0;
    const char* unit = 0;
    long value = 0;
    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    unit = fat_find_token_value(args, "unit=");
    if (!unit) {
        return -1;
    }
    value = strtol(unit, &end, 10);
    if (end == unit || (*end != '\0' && *end != ' ') || value < 0 || value > 255) {
        return -1;
    }
    return (int32_t)value;
}

/* Map the device manager's DRIVER name to the backend it stands for. The names
 * are the drivers' manifest package names, the same spelling a rule's DRIVER==
 * uses and block_backend_from_name() in the rule parser maps. */
static uint8_t fat_backend_from_name(const char* name) {
    if (!name) {
        return (uint8_t)BLOCK_BACKEND_UNKNOWN;
    }
    if (strncmp(name, "ata", 3) == 0 && (name[3] == '\0' || name[3] == ' ')) {
        return (uint8_t)BLOCK_BACKEND_ATA;
    }
    if (strncmp(name, "virtio-blk", 10) == 0 && (name[10] == '\0' || name[10] == ' ')) {
        return (uint8_t)BLOCK_BACKEND_VIRTIO_BLK;
    }
    return (uint8_t)BLOCK_BACKEND_UNKNOWN;
}

static uint8_t fat_parse_requested_backend(void) {
    char args[FAT_STARTUP_ARGS_MAX];
    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return (uint8_t)BLOCK_BACKEND_UNKNOWN;
    }
    return fat_backend_from_name(fat_find_token_value(args, "driver="));
}

/* The `block` class instance of the disk this driver was spawned for: the
 * fingerprint of the canonical id in `id=`.
 *
 * The id is copied out of the argument blob because it is not NUL-terminated
 * there -- tokens are space-separated -- and the fingerprint is defined over the
 * id alone. Returns 0 when there is no usable id, which no valid device has.  */
static uint32_t fat_parse_requested_instance(void) {
    char args[FAT_STARTUP_ARGS_MAX];
    char id[BLOCK_DESCRIPTOR_ID_MAX];
    const char* token = 0;
    uint32_t n = 0;
    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return 0u;
    }
    token = fat_find_token_value(args, "id=");
    if (!token) {
        return 0u;
    }
    while (n + 1u < sizeof(id) && token[n] != '\0' && token[n] != ' ') {
        id[n] = token[n];
        ++n;
    }
    id[n] = '\0';
    /* A truncated id fingerprints to a different value than the backend
     * registered, so refuse it rather than search for a disk that cannot answer. */
    if (n == 0u || (token[n] != '\0' && token[n] != ' ')) {
        return 0u;
    }
    return wasmos_block_fingerprint(id);
}

/* Resolve the block server for this instance's disk through the `block` service
 * CLASS, matching the instance that encodes its (backend, unit).
 *
 * Resolution is by class rather than by the plain `block` NAME because a name
 * resolves to one provider for the whole system: it could only ever reach the
 * ATA driver, so no rule could mount a filesystem on any other backend however
 * well that backend worked. The instance is derived from the pair, so the same
 * disk answers every boot regardless of probe order.
 *
 * Returns the provider's endpoint, or -1 while it has not registered yet -- the
 * caller retries, because a filesystem may well be spawned before its disk's
 * driver has finished coming up. */
static int32_t fat_lookup_block_server(int32_t reply_endpoint, uint32_t instance,
                                       int32_t request_id) {
    svc_class_entry_t providers[8];
    int32_t n = wasmos_svc_lookup_class(g_proc_endpoint,
                                        reply_endpoint,
                                        "block",
                                        providers,
                                        (int32_t)(sizeof(providers) / sizeof(providers[0])),
                                        request_id);
    if (n < 0) {
        return -1;
    }
    if (n > (int32_t)(sizeof(providers) / sizeof(providers[0]))) {
        n = (int32_t)(sizeof(providers) / sizeof(providers[0]));
    }
    for (int32_t i = 0; i < n; ++i) {
        if (providers[i].instance == instance) {
            return (int32_t)providers[i].endpoint;
        }
    }
    return -1;
}

/* Send a reply, treating a full receiver queue as backpressure rather than
 * failure: retry up to FAT_STREAM_SEND_RETRIES times, yielding between tries.
 *
 * A dropped reply is unrecoverable for the peer. It is waiting on exactly this
 * message and has no timeout, so IPC_ERR_FULL discarded here strands the
 * requester, and behind it every client of that requester -- which is how a
 * single refused FS_IPC_RESP wedges a whole session.
 *
 * READDIR makes that the common case, not a rare one: fat_readdir_stream fills
 * the relay's queue with FS_IPC_STREAM frames (retrying, so they get through)
 * and the terminating response follows immediately, at the moment the queue is
 * at its fullest. Whether it lands depends on how much the relay happened to
 * drain in between, which is why the resulting wedge was intermittent and
 * SMP-timing dependent.
 *
 * Returns the final send result; a caller with nowhere to report it may ignore
 * it, but exhausting the retries means the peer is stranded. */
static int32_t fat_send_reply(int32_t dest, int32_t type, uint32_t request_id, int32_t a0,
                              int32_t a1, int32_t a2, int32_t a3) {
    return wasmos_sys_ipc_send_retry(dest,
                                     g_fs_endpoint,
                                     type,
                                     (int32_t)request_id,
                                     a0,
                                     a1,
                                     a2,
                                     a3,
                                     (int32_t)FAT_STREAM_SEND_RETRIES);
}

/* Answer an fs-manager FSMGR_IPC_BACKEND_INFO_REQ pull. */
static void fat_report_backend_info(int32_t dst, int32_t request_id) {
    int32_t mount_arg = 0;
    if (g_mount_bid >= 0 && g_mount_len > 0 &&
        wasmos_xfer_buffer_borrow(dst, g_mount_bid, WASMOS_BUFFER_GRANT_READ) >= 0) {
        mount_arg = (int32_t)(((uint32_t)g_mount_bid << 12) | ((uint32_t)g_mount_len & 0xFFFu));
    }
    (void)fat_send_reply(dst,
                         FSMGR_IPC_BACKEND_INFO_RESP,
                         (uint32_t)request_id,
                         FSMGR_BACKEND_BOOT,
                         0,
                         mount_arg,
                         (int32_t)g_mount_unit);
}

/* Resolve the mount alias + unit via BLOCK_IPC_IDENTIFY + devmgr query.
 *
 * IDENTIFY answers with a block descriptor written into a buffer THIS driver
 * owns and lends to the backend for the request, so the unit is read out of that
 * record rather than out of an argument word. The client holds the lifecycle
 * (docs/architecture/12-dma-transfers.md); the backend is a transient grantee.
 *
 * TODO: the mount name still comes from a device-manager query keyed on the
 * unit, which cannot name one partition of a disk. It is the descriptor's
 * canonical_id that identifies the volume; passing the mount in the startup
 * arguments retires DEVMGR_QUERY_BLOCK_MOUNT_REQ altogether. */
static int fat_resolve_mount_alias(char* out_mount, uint32_t out_mount_len, uint8_t* out_unit) {
    int32_t reply = g_blk.reply_endpoint;
    int32_t devmgr = -1;
    int32_t req_id = 41;
    int32_t unit = 0;
    int32_t desc_bid = -1;
    int32_t rc = -1;
    wasmos_block_descriptor_t desc;
    uint32_t packed[4];
    if (!out_mount || out_mount_len < 2u || !out_unit) {
        return -1;
    }
    out_mount[0] = '\0';
    *out_unit = 0;
    /* This driver owns the buffer and lends it to the backend for the round
     * trip; release below cascade-revokes that grant on every path. */
    desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (desc_bid < 0) {
        fat_log("identify buffer unavailable\n");
        return -1;
    }
    if (wasmos_xfer_buffer_borrow(g_blk.block_endpoint, desc_bid, WASMOS_BUFFER_GRANT_WRITE) < 0) {
        fat_log("identify buffer grant failed\n");
        (void)wasmos_xfer_buffer_release(desc_bid);
        return -1;
    }
    if (wasmos_ipc_send(g_blk.block_endpoint,
                        reply,
                        BLOCK_IPC_IDENTIFY_REQ,
                        req_id,
                        (int32_t)g_requested_instance,
                        desc_bid,
                        0,
                        0) == 0 &&
        wasmos_ipc_select_one(reply) >= 0 &&
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) == BLOCK_IPC_IDENTIFY_RESP &&
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) == req_id &&
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0) == 0 &&
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1) >= (int32_t)sizeof(desc)) {
        rc = wasmos_xfer_buffer_read(desc_bid, &desc, (int32_t)sizeof(desc), 0);
    }
    (void)wasmos_xfer_buffer_release(desc_bid);
    if (rc != 0) {
        fat_log("identify failed\n");
        return -1;
    }
    if (desc.version != BLOCK_DESCRIPTOR_VERSION) {
        fat_log("identify descriptor version mismatch\n");
        return -1;
    }
    unit = (int32_t)desc.unit;
    *out_unit = (uint8_t)(unit & 0xFF);
    devmgr = wasmos_svc_lookup(g_proc_endpoint, reply, "devmgr.query", 1);
    if (devmgr < 0) {
        return -1;
    }
    req_id++;
    if (wasmos_ipc_send(devmgr, reply, DEVMGR_QUERY_BLOCK_MOUNT_REQ, req_id, unit, 0, 0, 0) != 0 ||
        wasmos_ipc_select_one(reply) < 0) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != DEVMGR_BLOCK_MOUNT_INFO ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req_id) {
        return -1;
    }
    packed[0] = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
    packed[1] = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
    packed[2] = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    packed[3] = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
    fat_unpack_name(packed[0], packed[1], packed[2], packed[3], out_mount, out_mount_len);
    return out_mount[0] ? 0 : -1;
}

/* --- op pool / FIFO --- */

static fat_op_ctx_t* fat_op_alloc(void) {
    for (uint32_t i = 0; i < FAT_MAX_INFLIGHT; ++i) {
        if (!g_ops[i].in_use) {
            memset(&g_ops[i], 0, sizeof(g_ops[i]));
            g_ops[i].in_use = 1;
            return &g_ops[i];
        }
    }
    return 0;
}

static void fat_op_free(fat_op_ctx_t* op) {
    /* Drop the grant the read path may have extended to the block server. Doing
     * it here — the single teardown path for both success and failure — is what
     * keeps a request from leaking a writable grant to the client's buffer. */
    if (op->zc_borrow > 0) {
        (void)wasmos_xfer_buffer_unborrow(op->zc_borrow);
    }
    op->zc_borrow = 0;
    fat_block_release(&g_blk, op);
    op->in_use = 0;
}

static void fat_fifo_push(fat_op_ctx_t* op) {
    g_fifo[g_fifo_tail] = (uint32_t)(op - g_ops);
    g_fifo_tail = (g_fifo_tail + 1u) % FAT_MAX_INFLIGHT;
    g_fifo_len++;
}

static fat_op_ctx_t* fat_fifo_pop(void) {
    fat_op_ctx_t* op;
    if (g_fifo_len == 0) {
        return 0;
    }
    op = &g_ops[g_fifo[g_fifo_head]];
    g_fifo_head = (g_fifo_head + 1u) % FAT_MAX_INFLIGHT;
    g_fifo_len--;
    return op;
}

/* Map a forwarded FS request type to the op enum.  Returns 0 for a type this
 * backend does not implement. */
static fat_op_t fat_op_for_type(int32_t type) {
    switch (type) {
    case FS_IPC_OPEN_REQ:
        return FAT_OP_OPEN;
    case FS_IPC_READ_REQ:
        return FAT_OP_READ;
    case FS_IPC_WRITE_REQ:
        return FAT_OP_WRITE;
    case FS_IPC_STAT_REQ:
        return FAT_OP_STAT;
    case FS_IPC_SEEK_REQ:
        return FAT_OP_SEEK;
    case FS_IPC_CLOSE_REQ:
        return FAT_OP_CLOSE;
    case FS_IPC_UNLINK_REQ:
        return FAT_OP_UNLINK;
    case FS_IPC_RENAME_REQ:
        return FAT_OP_RENAME;
    case FS_IPC_MKDIR_REQ:
        return FAT_OP_MKDIR;
    case FS_IPC_RMDIR_REQ:
        return FAT_OP_RMDIR;
    case FS_IPC_READDIR_REQ:
        return FAT_OP_READDIR;
    case FS_IPC_CHDIR_REQ:
        return FAT_OP_CHDIR;
    default:
        return FAT_OP_NONE;
    }
}

/* Advance the active op one step (dispatch to its coroutine). */
static fat_r_t fat_op_dispatch(fat_op_ctx_t* op) {
    switch (op->op) {
    case FAT_OP_OPEN:
        return fat_op_open(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_READ:
        return fat_op_read(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_WRITE:
        return fat_op_write(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_STAT:
        return fat_op_stat(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_SEEK:
        return fat_op_seek(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_CLOSE:
        return fat_op_close(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_UNLINK:
        return fat_op_unlink(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_RENAME:
        return fat_op_rename(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_MKDIR:
        return fat_op_mkdir(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_RMDIR:
        return fat_op_rmdir(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_READDIR:
        return fat_op_readdir(op, &g_blk, &g_mnt, g_fs_endpoint);
    case FAT_OP_CHDIR:
        return fat_op_chdir(op, &g_blk, &g_mnt);
    default:
        op->err = WASMOS_ERR_FS_UNSUPPORTED;
        return FAT_R_ERR;
    }
}

static void fat_send_response(fat_op_ctx_t* op, fat_r_t r) {
    if (r == FAT_R_DONE) {
        int32_t a0 = op->resp_override ? op->resp_arg0 : 0;
        int32_t a1 = op->resp_override ? op->resp_arg1 : 0;
        (void)fat_send_reply(op->source, FS_IPC_RESP, op->request_id, a0, a1, 0, 0);
    } else {
        /* A step that returns FAT_R_ERR without recording a code -- the
         * fat_block_read_direct submit paths -- failed talking to the block
         * device, which is what IO names. */
        int32_t err = op->err ? op->err : WASMOS_ERR_FS_IO;
        (void)fat_send_reply(op->source, FS_IPC_ERROR, op->request_id, err, 0, 0, 0);
    }
}

/* Drive the active op: run the mount prerequisite (if needed) then dispatch,
 * completing (send + free) on DONE/ERR or leaving it active on WAIT. */
static void fat_drive_active(void) {
    fat_r_t r;
    if (!g_active) {
        return;
    }
    fat_block_set_owner(&g_blk, g_active);

    if (!fat_mount_ready(&g_mnt)) {
        r = fat_geom_mount_step(&g_mnt, &g_blk);
        if (r == FAT_R_WAIT) {
            return;
        }
        if (r == FAT_R_ERR) {
            fat_send_response(g_active, FAT_R_ERR);
            fat_op_free(g_active);
            g_active = 0;
            return;
        }
        /* mounted: fall through to the op's own work */
    }

    r = fat_op_dispatch(g_active);
    if (r == FAT_R_WAIT) {
        return;
    }
    fat_send_response(g_active, r);
    fat_op_free(g_active);
    g_active = 0;
}

/* Pull the next queued op into the active slot and drive it, repeating while ops
 * complete synchronously (no I/O) so the FIFO drains promptly. */
static void fat_activate_next(void) {
    while (!g_active && g_fifo_len > 0) {
        g_active = fat_fifo_pop();
        fat_drive_active();
    }
}

/* Drive the mount coroutine to completion at init, using the reactor's own mount
 * step and block-completion path.  Blocking on the reply is admissible here and
 * only here: it is one-time bring-up with no client ops in flight, like the
 * IDENTIFY / backend-info handshakes.  A mount failure is fatal (fat_stall). */
static void fat_mount_bringup(void) {
    static fat_op_ctx_t boot;
    memset(&boot, 0, sizeof(boot));
    boot.in_use = 1;
    for (;;) {
        fat_r_t r;
        fat_block_set_owner(&g_blk, &boot);
        r = fat_geom_mount_step(&g_mnt, &g_blk);
        if (r == FAT_R_DONE) {
            return;
        }
        if (r == FAT_R_ERR) {
            fat_log("mount failed\n");
            fat_stall();
        }
        /* r == FAT_R_WAIT: a block read was submitted; wait for its reply. */
        for (;;) {
            int ok = 0;
            if (fat_block_complete(&g_blk, &ok) != &boot) {
                continue; /* spurious / unmatched reply */
            }
            if (!ok) {
                fat_log("mount io error\n");
                fat_stall();
            }
            break;
        }
    }
}

/* Driver entry point: acquire a block server, mount the volume, register under
 * the fs.backend class, then run the reactor loop forever.
 *
 * The block server is discovered through the `block` service CLASS, by the
 * instance encoding the (backend, unit) the device manager spawned this
 * instance for; that discovery loop spins with a yield until that disk's driver
 * registers, so this call does not return until it exists.
 *
 * Mounting happens BEFORE the ready notification, so the driver never advertises
 * a volume it has not parsed. On success this does not return: the reactor loop
 * is unbounded. Every bring-up failure calls fat_stall() rather than returning,
 * so a failed mount leaves the process parked instead of exiting -- there is no
 * failure status on this path for a caller to observe. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    int32_t block_endpoint;
    int32_t reply_endpoint;
    int32_t sel;
    char mount_alias[16];
    char service_name[16];
    int32_t mount_alias_len;

    g_proc_endpoint = wasmos_startup_proc_endpoint();

    g_fs_endpoint = wasmos_ipc_create_endpoint();
    reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_fs_endpoint < 0 || reply_endpoint < 0) {
        fat_log("endpoint create failed\n");
        fat_stall();
    }

    g_requested_unit = fat_parse_requested_unit();
    g_requested_backend = fat_parse_requested_backend();
    if (g_requested_backend == (uint8_t)BLOCK_BACKEND_UNKNOWN || g_requested_unit < 0) {
        /* Without both halves there is no disk to IDENTIFY. The device manager
         * always sends them; a spawn that did not is a configuration error, and
         * guessing a backend would mount the wrong volume. */
        fat_log("startup args missing driver= or unit=; cannot resolve a block device\n");
        fat_stall();
    }
    /* The class instance that names this disk: the fingerprint of the canonical
     * id the device manager passed through from the backend's own publish. It is
     * NOT rebuilt from driver= and unit= here -- a second place that spells the
     * id is a second place that can disagree with the publisher, and a
     * disagreement leaves this driver waiting forever on an instance nobody
     * holds. */
    g_requested_instance = fat_parse_requested_instance();
    if (g_requested_instance == 0u) {
        fat_log("startup args missing id=; cannot resolve a block device\n");
        fat_stall();
    }
    const uint32_t instance = g_requested_instance;
    for (int32_t attempt = 0;; ++attempt) {
        block_endpoint = fat_lookup_block_server(reply_endpoint, instance, 1 + attempt);
        if (block_endpoint > 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }

    fat_block_configure(&g_blk, block_endpoint, reply_endpoint, g_requested_instance);
    if (fat_block_setup(&g_blk) != 0) {
        fat_log("block buffer missing\n");
        fat_stall();
    }
    fat_mount_init(&g_mnt);
    fat_open_pool_init(&g_pool);
    /* Mount before signalling ready, so the driver only ever advertises a
     * validated, parsed volume. */
    fat_mount_bringup();

    /* Resolve mount identity and register under the fs.backend class. */
    if (fat_resolve_mount_alias(mount_alias, sizeof(mount_alias), &g_mount_unit) != 0) {
        fat_log("mount alias resolve failed\n");
        fat_stall();
    }
    if (snprintf(service_name, sizeof(service_name), "fs.boot%u", (unsigned)g_mount_unit) <= 0) {
        fat_stall();
    }
    mount_alias_len = (int32_t)strlen(mount_alias);
    if (mount_alias_len <= 0 || mount_alias_len >= 0xFFF ||
        mount_alias_len >= wasmos_xfer_buffer_size()) {
        fat_log("mount alias size invalid\n");
        fat_stall();
    }
    g_mount_bid = wasmos_xfer_buffer_acquire(mount_alias_len);
    if (g_mount_bid < 0 ||
        wasmos_xfer_buffer_write(g_mount_bid, mount_alias, mount_alias_len, 0) != 0) {
        fat_log("mount alias buffer write failed\n");
        fat_stall();
    }
    g_mount_len = mount_alias_len;
    if (wasmos_svc_register_class(g_proc_endpoint,
                                  g_fs_endpoint,
                                  service_name,
                                  FSMGR_BACKEND_CLASS,
                                  FSMGR_BACKEND_INSTANCE(FSMGR_BACKEND_BOOT, g_mount_unit),
                                  1) != 0) {
        fat_log("fs.backend register failed\n");
        fat_stall();
    }
    fat_log("fs.backend registered\n");

    /* Wait for fs-manager's first info pull before signalling ready. */
    for (;;) {
        if (wasmos_ipc_select_one(g_fs_endpoint) < 0) {
            continue;
        }
        if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) == FSMGR_IPC_BACKEND_INFO_REQ) {
            fat_report_backend_info(wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
                                    wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID));
            break;
        }
    }
    wasmos_sys_notify_ready(g_proc_endpoint, reply_endpoint);

    /* Reactor: select on the service endpoint + the block reply endpoint. */
    sel = wasmos_ipc_select_create();
    if (sel < 0 || wasmos_ipc_select_add(sel, g_fs_endpoint) != 0 ||
        wasmos_ipc_select_add(sel, reply_endpoint) != 0) {
        fat_log("select setup failed\n");
        fat_stall();
    }

    for (;;) {
        int32_t ready;
        fat_activate_next();
        ready = wasmos_ipc_select_wait(sel);
        if (ready < 0) {
            continue;
        }
        if (ready == reply_endpoint) {
            int ok = 0;
            fat_op_ctx_t* o = fat_block_complete(&g_blk, &ok);
            if (o && o == g_active) {
                if (!ok) {
                    if (!g_active->err) {
                        g_active->err = WASMOS_ERR_FS_IO;
                    }
                    fat_send_response(g_active, FAT_R_ERR);
                    fat_op_free(g_active);
                    g_active = 0;
                } else {
                    fat_drive_active();
                }
            }
            continue;
        }
        if (ready == g_fs_endpoint) {
            int32_t type = -1;
            fat_op_ctx_t* op;
            if (wasmos_ipc_select_one(g_fs_endpoint) < 0) {
                continue;
            }
            type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
            if (type == FSMGR_IPC_BACKEND_INFO_REQ) {
                fat_report_backend_info(wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
                                        wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID));
                continue;
            }
            if (type == FS_IPC_READY_REQ) {
                (void)fat_send_reply(wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
                                     FS_IPC_RESP,
                                     (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID),
                                     0,
                                     0,
                                     0,
                                     0);
                continue;
            }
            op = fat_op_alloc();
            if (!op) {
                (void)fat_send_reply(wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
                                     FS_IPC_ERROR,
                                     (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID),
                                     WASMOS_ERR_FS_BUSY,
                                     0,
                                     0,
                                     0);
                continue;
            }
            op->type = type;
            op->op = fat_op_for_type(type);
            op->request_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            op->arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            op->arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
            op->arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
            op->arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
            op->source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
            if (op->op == FAT_OP_CHDIR) {
                /* CHDIR carries the target name packed in arg0..arg3. */
                fat_unpack_name((uint32_t)op->arg0,
                                (uint32_t)op->arg1,
                                (uint32_t)op->arg2,
                                (uint32_t)op->arg3,
                                op->dir_name,
                                sizeof(op->dir_name));
            }
            fat_fifo_push(op);
        }
    }
    return 0;
}
