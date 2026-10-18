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
/* The endpoint that pulled this backend's info: fs-manager, which owns the
 * mount. Only it may end this process by unmounting. -1 until the first pull. */
static int32_t g_mount_owner = -1;

static fat_block_t g_blk;
static fat_mount_t g_mnt;
static fat_open_pool_t g_pool;

/* Mount identity reported to fs-manager on each backend-info pull. */
/* This driver's mount name, reported on an fs-manager pull. Held as plain bytes:
 * fs-manager is the CLIENT of that pull and supplies the buffer, so this driver
 * owns no transfer buffer for it. */
static char g_mount_name[32];
static int32_t g_mount_len = 0;
/* The disk's backend-local device number, read off the descriptor this driver
 * IDENTIFYs -- not off a startup argument. Reported to fs-manager as diagnostic
 * identity only; nothing routes on it. */
static uint8_t g_mount_unit = 0;
/* The canonical id of this driver's disk, exactly as it was spawned with.
 *
 * Kept as the STRING as well as the fingerprint because fs-manager reports it in
 * the mount table: a mount names the device it sits on with the same id blkinfo
 * prints and `mount -t`/`fsck.wfs` take, so a reader can carry it between them.
 * Empty until the arguments are parsed. */
static char g_source_id[BLOCK_DESCRIPTOR_ID_MAX];

/* The `block` class instance of this driver's disk: the fingerprint of the
 * canonical id the device manager passed in `id=`. It is both how the provider
 * is found in the class registry and how the provider is told which of its disks
 * is meant, since several instances may share one endpoint. */
static uint32_t g_requested_instance = 0u;

/* The `volume` class instance of the volume this driver mounted: the
 * fingerprint of `volume:` prefixed to the device's canonical id
 * (architecture/37-volume-manager.md §3). Zero until the id is known good. */
static uint32_t g_volume_instance = 0u;

/* The endpoint this driver takes replies to requests it ISSUES on, as opposed
 * to g_fs_endpoint, where it receives the ones it serves. Held at file scope
 * because the volume is claimed from the mount path and released from the
 * unmount path, which are different scopes. */
static int32_t g_reply_endpoint = -1;

/* The volume manager's endpoint, resolved once when the claim is taken and kept
 * for the release.
 *
 * Resolved once because the release runs inside the reactor, and
 * wasmos_svc_lookup_class BLOCKS on g_reply_endpoint while DISCARDING every
 * message that is not its reply -- which is where this driver's outstanding
 * block replies land. A lookup there would consume one and leave the operation
 * it belonged to unable to complete. The claim is taken at bring-up, where
 * nothing is in flight. */
static int32_t g_volmgr_endpoint = -1;

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
 * `driver=<name> unit=<n> id=<canonical id> mount=<path>`, whose id alone may be
 * BLOCK_DESCRIPTOR_ID_MAX bytes; the process manager truncates at
 * WASMOS_STARTUP_ARGS_MAX, so nothing longer can arrive anyway. */
#define FAT_STARTUP_ARGS_MAX 192

/* The VFS path this volume mounts at, from `mount=`.
 *
 * The rule that spawned this driver chose it, and the device manager delivers it
 * at spawn. Nothing is asked back afterwards: the retired
 * DEVMGR_QUERY_BLOCK_MOUNT_REQ answered a query keyed on the disk unit, which
 * two volumes on one disk share, so it could not name which of them was being
 * asked about.
 *
 * Copied out because the token is not NUL-terminated in the blob -- tokens are
 * space-separated. Returns 0 on success; a value that does not fit is refused
 * rather than truncated, since a truncated path is a different, valid-looking
 * path. */
static int fat_parse_requested_mount(char* out, uint32_t out_cap) {
    char args[FAT_STARTUP_ARGS_MAX];
    const char* token = 0;
    uint32_t n = 0;
    if (!out || out_cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    token = fat_find_token_value(args, "mount=");
    if (!token) {
        return -1;
    }
    while (n + 1u < out_cap && token[n] != '\0' && token[n] != ' ') {
        out[n] = token[n];
        ++n;
    }
    out[n] = '\0';
    if (n == 0u || (token[n] != '\0' && token[n] != ' ')) {
        return -1;
    }
    return 0;
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
    /* Held only once the id is known good, so g_source_id never reports a disk
     * this driver refused to address. */
    str_copy(g_source_id, sizeof(g_source_id), id);
    return wasmos_block_fingerprint(id);
}

/* The `volume` class instance for `g_source_id`, derived rather than looked up:
 * a volume's canonical id is `volume:` prefixed to its backing device's, and
 * the instance is a fingerprint of that string. Returns 0 when the id does not
 * fit, which leaves the volume unclaimed rather than claiming the wrong one. */
static uint32_t fat_volume_instance_of(const char* id) {
    static const char prefix[] = "volume:";
    char volume_id[VOLUME_DESCRIPTOR_ID_MAX];
    uint32_t n = 0u;
    uint32_t i = 0u;

    if (!id || id[0] == '\0') {
        return 0u;
    }
    while (prefix[n] != '\0') {
        volume_id[n] = prefix[n];
        ++n;
    }
    while (id[i] != '\0') {
        if (n + 1u >= sizeof(volume_id)) {
            return 0u;
        }
        volume_id[n++] = id[i++];
    }
    volume_id[n] = '\0';
    return wasmos_block_fingerprint(volume_id);
}

/* Record or release this driver's claim on the volume it mounted.
 *
 * ADVISORY, and deliberately fire-and-forget, on the same terms as fs_wfs's:
 * the volume manager is not in the I/O path
 * (architecture/37-volume-manager.md §5), so the claim changes nothing about
 * this mount. What it changes is what everything else may do to the device
 * underneath it -- `fsck`, `mkfs`, and the partition manager, which refuses to
 * retire a partition whose volume is claimed. Without this a FAT mount was
 * invisible to all three: the claim was the only live record of what is mounted
 * and only WFS wrote to it.
 *
 * Blocking the mount on the round trip would make an advisory record able to
 * stall a boot, so no reply is awaited. A volume no volume manager published is
 * simply not claimed, which is why a failed lookup is not an error.
 */
static void fat_volume_claim(int32_t claim) {
    /* Sized to the volume manager's own capacity (MAX_VOLUMES in
     * volume_manager.zig). wasmos_svc_lookup_class writes min(total, cap)
     * entries and returns the TOTAL, so a smaller array does not fail -- it
     * silently returns a prefix, and a volume outside it would be read as
     * absent. A claim skipped that way leaves a mounted filesystem looking
     * unmounted to everything that consults the flag. */
    svc_class_entry_t providers[16];
    int32_t n;
    int32_t i;

    if (g_volume_instance == 0u) {
        return;
    }
    if (g_volmgr_endpoint < 0) {
        n = wasmos_svc_lookup_class(g_proc_endpoint,
                                    g_reply_endpoint,
                                    "volume",
                                    providers,
                                    (int32_t)(sizeof(providers) / sizeof(providers[0])),
                                    1);
        if (n < 0) {
            return;
        }
        if (n > (int32_t)(sizeof(providers) / sizeof(providers[0]))) {
            n = (int32_t)(sizeof(providers) / sizeof(providers[0]));
        }
        for (i = 0; i < n; ++i) {
            if (providers[i].instance == g_volume_instance) {
                g_volmgr_endpoint = (int32_t)providers[i].endpoint;
                break;
            }
        }
    }
    if (g_volmgr_endpoint < 0) {
        return;
    }
    (void)wasmos_ipc_send(g_volmgr_endpoint,
                          g_fs_endpoint,
                          VOLUME_IPC_CLAIM_REQ,
                          1,
                          (int32_t)g_volume_instance,
                          claim,
                          0,
                          0);
}

/* Resolve the block server for this instance's disk through the `block` service
 * CLASS, matching the instance the disk's canonical id fingerprints to.
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

/* Answer an fs-manager FSMGR_IPC_BACKEND_INFO_REQ pull: write this driver's
 * mount name into the buffer fs-manager supplied (arg0) and report its length.
 * The buffer belongs to fs-manager, which releases it; this driver only writes
 * into a grant it holds for the duration of the reply. */
static void fat_report_backend_info(int32_t dst, int32_t request_id, int32_t buffer_id) {
    int32_t name_len = 0;
    int32_t source_len = 0;
    /* The mount's owner, learned rather than configured: fs-manager is the only
     * thing that pulls backend info, and it does so before this driver reports
     * ready. Recorded because an UNMOUNT ends this process, and the sender of a
     * shutdown is otherwise unchecked -- IPC authenticates the SOURCE endpoint's
     * owner, not its authority. */
    g_mount_owner = dst;
    if (buffer_id > 0 && g_mount_len > 0 &&
        wasmos_xfer_buffer_write(buffer_id, g_mount_name, g_mount_len, 0) == 0) {
        name_len = g_mount_len;
    }
    /* The device id follows the mount name in the same buffer, delimited by the
     * lengths rather than by a terminator. Written only when the name was, since
     * its offset is the name's length: a reader given a source length but no name
     * could not find it. */
    if (name_len > 0) {
        int32_t id_len = (int32_t)strlen(g_source_id);
        if (id_len > 0 && wasmos_xfer_buffer_write(buffer_id, g_source_id, id_len, name_len) == 0) {
            source_len = id_len;
        }
    }
    (void)fat_send_reply(dst,
                         FSMGR_IPC_BACKEND_INFO_RESP,
                         (uint32_t)request_id,
                         FSMGR_BACKEND_BLOCK,
                         FS_TYPE_FAT,
                         name_len,
                         source_len);
}

/* IDENTIFY this driver's volume, filling *out_desc.
 *
 * The descriptor is what says WHAT this device is. `partition` nonzero means a
 * partition, which the partition manager serves by rebasing every transfer onto
 * its window -- so its LBA 0 is the volume's boot sector and it holds no table.
 * That is the whole reason this driver parses no partition table for a volume
 * the partition manager named: it read the table already, and a second reader is
 * a second answer. `lba_start` records where the window sits on the underlying
 * disk and is never an address a client sends.
 *
 * The buffer is acquired HERE and lent to the backend for the round trip, then
 * released on every path. The client of a request owns the transfer buffer and
 * the server is a transient grantee; release cascade-revokes the grant
 * (docs/architecture/12-dma-transfers.md).
 *
 * Returns 0 on success. */
static int fat_identify_device(wasmos_block_descriptor_t* out_desc) {
    int32_t reply = g_blk.reply_endpoint;
    int32_t req_id = 41;
    int32_t desc_bid = -1;
    int32_t rc = -1;
    if (!out_desc) {
        return -1;
    }
    desc_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(*out_desc));
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
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1) >= (int32_t)sizeof(*out_desc)) {
        rc = wasmos_xfer_buffer_read(desc_bid, out_desc, (int32_t)sizeof(*out_desc), 0);
    }
    (void)wasmos_xfer_buffer_release(desc_bid);
    if (rc != 0) {
        fat_log("identify failed\n");
        return -1;
    }
    if (out_desc->version != BLOCK_DESCRIPTOR_VERSION) {
        fat_log("identify descriptor version mismatch\n");
        return -1;
    }
    /* The block layer addresses sectors with a 32-bit LBA, so a volume longer
     * than that has a tail this driver cannot reach. Refused rather than
     * silently truncated: a filesystem mounted over part of itself corrupts the
     * rest of it on the first allocation past the boundary.
     * TODO: BLOCK_IPC_READ_REQ's LBA argument is what caps this; widening it is
     * what lifts the 2 TiB ceiling. */
    if (out_desc->lba_count > 0xFFFFFFFFull) {
        fat_log("volume extends beyond the addressable range\n");
        return -1;
    }
    return 0;
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
 * instance the `id=` this driver was spawned with fingerprints to; that
 * discovery loop spins with a yield until that disk's driver registers, so this
 * call does not return until it exists.
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
    char mount_alias[32];
    /* The registry's own capacity, NUL included. A longer name is truncated
     * silently -- by wasmos_svc_register_class_flags into the descriptor, and
     * again by the process manager reading it -- so sizing this buffer to match
     * is what keeps the name that registers equal to the name built here. */
    char service_name[WASMOS_SVC_NAME_MAX];
    int32_t mount_alias_len;
    wasmos_block_descriptor_t desc;

    g_proc_endpoint = wasmos_startup_proc_endpoint();

    g_fs_endpoint = wasmos_ipc_create_endpoint();
    reply_endpoint = wasmos_ipc_create_endpoint();
    g_reply_endpoint = reply_endpoint;
    if (g_fs_endpoint < 0 || reply_endpoint < 0) {
        fat_log("endpoint create failed\n");
        fat_stall();
    }

    /* The class instance that names this disk: the fingerprint of the canonical
     * id whoever spawned this driver passed through from the backend's own
     * publish. `id=` is the ONLY device token this driver takes -- a second place
     * that spells a disk is a second place that can disagree with the publisher,
     * and a disagreement leaves this driver waiting forever on an instance nobody
     * holds. The backend and unit are read off the descriptor below instead. */
    g_requested_instance = fat_parse_requested_instance();
    if (g_requested_instance == 0u) {
        fat_log("startup args missing id=; cannot resolve a block device\n");
        fat_stall();
    }
    /* Where this volume goes. The rule decided it and the device manager
     * delivered it; there is nothing to negotiate and nobody to ask. */
    if (fat_parse_requested_mount(mount_alias, sizeof(mount_alias)) != 0) {
        fat_log("startup args missing mount=; no mount point for this volume\n");
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
    /* IDENTIFY before mounting: the descriptor says whether this device is a
     * partition, and therefore whether its first sector is a boot sector or may
     * be a table. A driver that mounted first would have to work that out by
     * looking, which is the guess this change removes. */
    if (fat_identify_device(&desc) != 0) {
        fat_stall();
    }
    g_mount_unit = (uint8_t)(desc.unit & 0xFFu);
    fat_mount_init(&g_mnt, desc.partition != 0u ? 1u : 0u);
    fat_open_pool_init(&g_pool);
    /* Mount before signalling ready, so the driver only ever advertises a
     * validated, parsed volume. */
    fat_mount_bringup();

    /* Claimed the moment the volume is mounted rather than once the driver is
     * ready: between those two points the volume is already being read, and a
     * tool that checked the claim in that window would see an unheld volume it
     * is free to write over. */
    g_volume_instance = fat_volume_instance_of(g_source_id);
    fat_volume_claim(1);

    /* Named after the MOUNT, not the disk. Nothing looks a filesystem backend up
     * by name -- fs-manager finds them by CLASS -- but the kernel's panic dump
     * reads it, and it is the only copy of this mount's identity the kernel has;
     * see wasmos_svc_name_from_mount. */
    if (wasmos_svc_name_from_mount(service_name, sizeof(service_name), "fs.fat", mount_alias) ==
        0u) {
        fat_log("service name could not be built\n");
        fat_stall();
    }
    mount_alias_len = (int32_t)strlen(mount_alias);
    if (mount_alias_len <= 0 || mount_alias_len >= (int32_t)sizeof(g_mount_name)) {
        fat_log("mount alias size invalid\n");
        fat_stall();
    }
    str_copy(g_mount_name, sizeof(g_mount_name), mount_alias);
    g_mount_len = mount_alias_len;
    /* The class INSTANCE is a fingerprint of the MOUNT PATH, not a packed
     * (kind, unit) pair. A path is unique by construction -- fs-manager refuses
     * a second mount at one -- while a unit is not: it is backend-local, and a
     * partition reports the unit of the disk it lives on, so two partitions of
     * one disk derived the same instance and the second registration was refused
     * with its mount never appearing. fs_wfs and fs_tmpfs derive theirs the same
     * way. */
    if (wasmos_svc_register_class(g_proc_endpoint,
                                  g_fs_endpoint,
                                  service_name,
                                  FSMGR_BACKEND_CLASS,
                                  wasmos_class_instance_fingerprint(mount_alias),
                                  1) != 0) {
        fat_log("fs.backend register failed\n");
        fat_stall();
    }
    /* The MOUNT is named, not just the fact of registering. A boot carries more
     * than one FAT backend -- /boot and /user -- and a log that cannot tell
     * them apart cannot answer the one question a stalled boot raises: did
     * /boot's instance come up? One capture of `[BUG][P0]` showed a single
     * "fs.backend registered" and no /boot mount, and which of the two had
     * registered was unknowable from the log. */
    (void)printf("[fat] fs.backend registered mount=%s\n", mount_alias);

    /* Wait for fs-manager's first info pull before signalling ready. */
    for (;;) {
        if (wasmos_ipc_select_one(g_fs_endpoint) < 0) {
            continue;
        }
        if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) == FSMGR_IPC_BACKEND_INFO_REQ) {
            /* Read every field into a local before replying: the reply writes
             * into fs-manager's buffer, and a host call invalidates the
             * last-message fields. Evaluation order within one argument list is
             * unspecified, so the two must not be mixed there. */
            int32_t info_src = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
            int32_t info_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            int32_t info_bid = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            fat_report_backend_info(info_src, info_req, info_bid);
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
                int32_t info_src = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
                int32_t info_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
                int32_t info_bid = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
                fat_report_backend_info(info_src, info_req, info_bid);
                continue;
            }
            if (type == WASMOS_IPC_SHUTDOWN_REQ) {
                /* Quiesce. There is nothing to flush: this driver buffers no
                 * write -- every one is issued to the block layer as it arrives
                 * and is complete when its reply lands -- so a volume is exactly
                 * as durable here as it was one request ago. DONE is owed all the
                 * same: fs-manager waits for it before dropping the mount, and
                 * the machine shutdown sequence is sequential, so a participant
                 * that never answers stalls everyone behind it.
                 *
                 * An UNMOUNT quiesce ends this process, a machine shutdown does
                 * not. After an unmount the machine keeps running and the path
                 * may be mounted again, but this process still holds the
                 * registrations derived from that mount -- its `fs.backend` class
                 * instance and its service name, both keyed on the owner context
                 * -- so a surviving backend blocks its own replacement. Leaving
                 * the reactor releases them by ending the process. */
                int32_t sd_src = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
                int32_t sd_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
                int32_t sd_reason = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
                /* Released before DONE goes out, because once this driver
                 * answers DONE it may be torn down at any moment and a claim
                 * that outlives its holder makes the volume permanently
                 * unformattable and its partition permanently unretirable.
                 *
                 * Released only when this mount is actually ending, which is the
                 * same test that decides whether to leave the reactor below.
                 * IPC checks that a message came from the endpoint it claims,
                 * not that its sender is entitled to what it asks, so an
                 * ordinary client that found this backend through the
                 * `fs.backend` class can send an UNMOUNT this driver refuses --
                 * and clearing the claim for a refused unmount would leave a
                 * live mount looking unheld to everything that consults it. */
                if (sd_reason != WASMOS_SHUTDOWN_REASON_UNMOUNT ||
                    (g_mount_owner >= 0 && sd_src == g_mount_owner)) {
                    fat_volume_claim(0);
                }
                (void)fat_send_reply(
                    sd_src, WASMOS_IPC_SHUTDOWN_DONE, (uint32_t)sd_req, 0, 0, 0, 0);
                /* Only the mount's OWNER may end this process. IPC checks that a
                 * message came from the endpoint it claims, not that its sender
                 * is entitled to what it asks, so an ordinary client that found
                 * this backend through the `fs.backend` class could otherwise
                 * unmount it by naming reason 2 -- bypassing fs-manager's busy
                 * and bootstrap rules, `/boot` included. A machine shutdown is
                 * not gated: it does not end this process here. */
                if (sd_reason == WASMOS_SHUTDOWN_REASON_UNMOUNT) {
                    if (g_mount_owner >= 0 && sd_src == g_mount_owner) {
                        fat_log("unmounted\n");
                        return 0;
                    }
                    fat_log("ignoring an unmount from something that does not own this mount\n");
                }
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
            if (op->op == FAT_OP_READDIR) {
                /* READDIR carries the directory to list as a path in the
                 * sender's transfer buffer (arg0 = length, arg2 = buffer id),
                 * the same transport OPEN uses. Reading it here, before the op is
                 * queued, keeps the op steps free of buffer handling. A path that
                 * does not fit is left empty, which fat_resolve_dir reads as the
                 * mount root and would list the wrong directory -- so it is
                 * failed instead by clearing the op type. */
                uint32_t chdir_len = (uint32_t)op->arg0;
                wasmos_error_code_t chdir_rc = WASMOS_ERR_NONE;

                /* Three different failures, three codes. Collapsing them into
                 * PATH_TOO_LONG told a caller its name was too long when the
                 * borrow was missing, which is the ambiguity the packed codes
                 * exist to remove -- and the two are fixed by opposite actions. */
                if (chdir_len == 0u) {
                    chdir_rc = WASMOS_ERR_FS_BAD_ARGS;
                } else if (chdir_len >= sizeof(op->dir_name) ||
                           chdir_len + 1u > (uint32_t)wasmos_xfer_buffer_size()) {
                    chdir_rc = WASMOS_ERR_FS_PATH_TOO_LONG;
                } else if (wasmos_sys_buffer_read(op->arg2, op->dir_name, (int32_t)chdir_len, 0) !=
                           0) {
                    chdir_rc = WASMOS_ERR_FS_BUFFER;
                }
                if (chdir_rc != WASMOS_ERR_NONE) {
                    /* Answered here rather than queued: an empty dir_name reads
                     * as the mount root further down, which would list a
                     * directory the caller did not name. */
                    (void)fat_send_reply(
                        op->source, FS_IPC_ERROR, op->request_id, chdir_rc, 0, 0, 0);
                    continue;
                }
                op->dir_name[chdir_len] = '\0';
            }
            fat_fifo_push(op);
        }
    }
    return 0;
}
