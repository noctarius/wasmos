/* fsck.wfs - check a WFS volume on a block device, from inside the OS.
 *
 * The checks are wfs_fsck.c, which is the same code the host unit suites run
 * against volumes mkfs_wfs produced. This file is only the part that cannot be
 * shared: finding the disk, and moving blocks over the block protocol.
 *
 * Usage: fsck.wfs [--repair] [--force] <canonical-id>
 *
 *   canonical-id  the disk's id as blkinfo prints it, e.g. `block:ata:2`. The
 *                 `block` class instance is its fingerprint, so the id is both
 *                 how the provider is found and how it is told which of its
 *                 disks is meant. There is no default: checking is harmless, but
 *                 --repair writes, and a tool whose most destructive invocation
 *                 is one omitted argument away from an arbitrary disk is a bad
 *                 tool.
 *
 * WITHOUT --repair nothing is written: findings are printed and the volume is
 * left exactly as it was. That is the default because a volume that fails its
 * checks is evidence, and the first thing anyone wants is to look at it.
 *
 * Blocking IPC is correct here. This is a one-shot utility: it runs, does its
 * work and exits, so it may wait on a BLOCK request the way src/utils/blkinfo
 * does. A SERVICE could not -- it would stall its event loop -- which is the
 * distinction, not host versus guest.
 *
 * The volume must NOT be mounted, and this tool is what checks that. It asks the
 * volume manager whether the volume carrying this device is CLAIMED -- a flag a
 * filesystem service sets when it mounts -- and refuses one that is. Checking a
 * volume someone else is writing reports damage that is merely a race, and
 * --repair would then "fix" a consistent volume into an inconsistent one.
 *
 * The check is ADVISORY on both sides (architecture/37-volume-manager.md §5).
 * The volume manager is not in the I/O path, so it records a claim rather than
 * enforcing one; this tool is the one that has to ask. Nothing beneath it
 * refuses a second client -- the block layer's per-unit arbitration was removed
 * when a request began naming its own target -- so a tool that skipped the
 * question would reach the sectors unimpeded.
 *
 * --force overrides, and exists because a claim can outlive its holder: a
 * filesystem service killed before it releases leaves a volume nothing will ever
 * agree to check. It is also the answer when no volume manager is running, since
 * "cannot tell" is reported as a refusal rather than waved through.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"

#include "wfs_fsck.h"

#define FSCK_MAX_PROVIDERS 8
#define FSCK_SECTOR_BYTES 512u
/* The per-process block buffer every transfer goes through is 8 KiB (see the
 * block_buffer_phys host call). It bounds the block size this tool can handle:
 * a 16384-byte volume needs a transfer twice the buffer, and the host call
 * refuses it. Reported as geometry rather than as an I/O error, because it is a
 * limit of the tool and not damage on the disk. */
#define FSCK_BUFFER_BYTES 8192u

static int32_t g_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static int32_t g_request_id = 1;
static int32_t g_buffer_phys;
/* The disk every request names, and the buffer the request descriptor travels
 * in -- acquired once and borrowed READ to the block endpoint, which is the
 * direction docs/architecture/12-dma-transfers.md requires: the requester owns
 * the object, the backend is a transient grantee. */
static uint32_t g_target;
static int32_t g_req_bid = -1;

/* Whether a filesystem service holds the volume on `device_id`.
 *
 * Returns 1 claimed, 0 not claimed, and -1 when the question cannot be answered:
 * no volume manager is running, or no volume covers this device. Those are
 * distinct from "not claimed" and are reported as such -- a check that proceeded
 * silently because it could not find the owner would be exactly as dangerous as
 * one that ignored a claim, and the operator is the one who can tell whether the
 * disk is really idle.
 *
 * The volume's class instance is DERIVED, not asked for: a volume's canonical id
 * is `volume:` prefixed to its backing device's, so this builds the same string
 * the volume manager did (architecture/37-volume-manager.md §3).
 *
 * The descriptor lands in a buffer THIS process owns and lends WRITE to the
 * volume manager for the request; release cascade-revokes that grant on every
 * path out (docs/architecture/12-dma-transfers.md). */
static int volume_is_claimed(int32_t proc_endpoint, const char* device_id) {
    svc_class_entry_t providers[FSCK_MAX_PROVIDERS];
    wasmos_volume_descriptor_t desc;
    wasmos_ipc_message_t reply;
    char volume_id[VOLUME_DESCRIPTOR_ID_MAX];
    static const char prefix[] = "volume:";
    uint32_t n = 0;
    uint32_t i = 0;
    int32_t endpoint = -1;
    int32_t count;
    int32_t bid;
    uint32_t instance;
    int claimed;

    while (prefix[n] != '\0') {
        volume_id[n] = prefix[n];
        ++n;
    }
    while (device_id[i] != '\0') {
        if (n + 1u >= sizeof(volume_id)) {
            return -1;
        }
        volume_id[n++] = device_id[i++];
    }
    volume_id[n] = '\0';
    instance = wasmos_block_fingerprint(volume_id);

    count = wasmos_svc_lookup_class(
        proc_endpoint, g_reply_endpoint, "volume", providers, FSCK_MAX_PROVIDERS, g_request_id++);
    if (count <= 0) {
        return -1;
    }
    if (count > FSCK_MAX_PROVIDERS) {
        count = FSCK_MAX_PROVIDERS;
    }
    for (i = 0; i < (uint32_t)count; ++i) {
        if (providers[i].instance == instance) {
            endpoint = (int32_t)providers[i].endpoint;
            break;
        }
    }
    if (endpoint < 0) {
        return -1;
    }

    bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(desc));
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_borrow(endpoint, bid, WASMOS_BUFFER_GRANT_WRITE) < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    if (wasmos_ipc_call(endpoint,
                        g_reply_endpoint,
                        VOLUME_IPC_IDENTIFY_REQ,
                        g_request_id++,
                        (int32_t)instance,
                        bid,
                        0,
                        0,
                        &reply) != 0 ||
        reply.type != VOLUME_IPC_IDENTIFY_RESP ||
        wasmos_xfer_buffer_read(bid, &desc, (int32_t)sizeof(desc), 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    if (desc.version != VOLUME_DESCRIPTOR_VERSION) {
        return -1;
    }
    claimed = (desc.flags & VOLUME_DESCRIPTOR_FLAG_CLAIMED) != 0u;
    return claimed;
}

static int str_prefix(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/* One whole filesystem block, as sectors.
 *
 * The byte offset is `block * len` in every call the checker makes -- it reads
 * block 0 at the largest permitted block size before any geometry is known, and
 * everything after at the volume's own -- so the conversion needs no separate
 * notion of the block size. */
/* Stage one transfer descriptor and send it. The arguments name only where the
 * request is (buffer, offset, size); everything about the transfer -- the disk,
 * a 64-bit LBA, where the payload goes -- is in the descriptor. */
static wasmos_error_code_t submit(int32_t type, uint64_t offset, uint32_t len,
                                  wasmos_ipc_message_t* reply) {
    wasmos_block_request_t req = {0};

    req.version = BLOCK_REQUEST_VERSION;
    req.target = g_target;
    req.lba = offset / FSCK_SECTOR_BYTES;
    req.sector_count = len / FSCK_SECTOR_BYTES;
    req.dst_kind = (uint32_t)BLOCK_DST_BLOCK_BUFFER;
    req.dst_phys = (uint32_t)g_buffer_phys;
    if (g_req_bid < 0 || wasmos_xfer_buffer_write(g_req_bid, &req, (int32_t)sizeof(req), 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_ipc_call(g_endpoint,
                        g_reply_endpoint,
                        type,
                        g_request_id++,
                        g_req_bid,
                        0,
                        (int32_t)sizeof(req),
                        0,
                        reply) != 0) {
        return WASMOS_ERR_FS_IO;
    }
    return WASMOS_ERR_NONE;
}

static wasmos_error_code_t io_read(void* user, uint32_t block, void* out, uint32_t len) {
    wasmos_ipc_message_t reply;
    wasmos_error_code_t rc;

    (void)user;
    if ((len % FSCK_SECTOR_BYTES) != 0u || len > FSCK_BUFFER_BYTES) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    rc = submit(BLOCK_IPC_READ_REQ, (uint64_t)block * len, len, &reply);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    if (reply.type == BLOCK_IPC_ERROR) {
        return reply.arg0 ? (wasmos_error_code_t)reply.arg0 : WASMOS_ERR_FS_IO;
    }
    if (reply.type != BLOCK_IPC_READ_RESP || reply.arg0 != 0) {
        return WASMOS_ERR_FS_IO;
    }
    if (wasmos_block_buffer_copy(g_buffer_phys, addr_cast(int32_t, out), (int32_t)len, 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    return WASMOS_ERR_NONE;
}

static wasmos_error_code_t io_write(void* user, uint32_t block, const void* in, uint32_t len) {
    wasmos_ipc_message_t reply;
    wasmos_error_code_t rc;

    (void)user;
    if ((len % FSCK_SECTOR_BYTES) != 0u || len > FSCK_BUFFER_BYTES) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    if (wasmos_block_buffer_write(
            g_buffer_phys, addr_cast(int32_t, (void*)(uintptr_t)in), (int32_t)len, 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    rc = submit(BLOCK_IPC_WRITE_REQ, (uint64_t)block * len, len, &reply);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    if (reply.type == BLOCK_IPC_ERROR) {
        return reply.arg0 ? (wasmos_error_code_t)reply.arg0 : WASMOS_ERR_FS_IO;
    }
    return (reply.type == BLOCK_IPC_WRITE_RESP && reply.arg0 == 0) ? WASMOS_ERR_NONE
                                                                   : WASMOS_ERR_FS_IO;
}

/* Every finding, as it is found. A checker that printed only totals would say a
 * volume has four extent problems without saying where any of them is. */
static void on_finding(void* user, const char* what, uint32_t block, uint32_t object) {
    (void)user;
    if (object != 0u) {
        (void)printf("[fsck.wfs] %s (object %u)\n", what, (unsigned)object);
    } else if (block != 0u) {
        (void)printf("[fsck.wfs] %s (block %u)\n", what, (unsigned)block);
    } else {
        (void)printf("[fsck.wfs] %s\n", what);
    }
}

int main(void) {
    char args[64];
    svc_class_entry_t providers[FSCK_MAX_PROVIDERS];
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    wfs_fsck_report_t rep;
    wfs_fsck_io_t io;
    wasmos_error_code_t rc;
    const char* rest;
    char id[BLOCK_DESCRIPTOR_ID_MAX];
    uint32_t n = 0;
    int32_t count;
    int32_t i;
    int repair = 0;
    int force = 0;
    int claimed;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    rest = args;
    /* Both flags, in either order, before the id. */
    for (;;) {
        if (str_prefix(rest, "--repair")) {
            repair = 1;
            rest += 8;
        } else if (str_prefix(rest, "--force")) {
            force = 1;
            rest += 7;
        } else {
            break;
        }
        while (*rest == ' ') {
            rest++;
        }
    }
    while (n + 1u < sizeof(id) && rest[n] != '\0' && rest[n] != ' ') {
        id[n] = rest[n];
        ++n;
    }
    id[n] = '\0';
    /* A truncated id fingerprints to a value no backend registered, so it would
     * look like an absent disk rather than a mistyped argument. */
    if (n == 0u || (rest[n] != '\0' && rest[n] != ' ')) {
        (void)printf("usage: fsck.wfs [--repair] [--force] <canonical-id>   (blkinfo lists ids)\n");
        return 1;
    }
    g_target = wasmos_block_fingerprint(id);

    if (proc_endpoint < 0) {
        (void)printf("[fsck.wfs] no process-manager endpoint\n");
        return 1;
    }
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        (void)printf("[fsck.wfs] no reply endpoint\n");
        return 1;
    }
    /* Before any I/O, and before the block device is even resolved: checking a
     * volume a filesystem is writing reports damage that is only a race, and
     * --repair would then "fix" a consistent volume into an inconsistent one. */
    claimed = volume_is_claimed(proc_endpoint, id);
    if (claimed > 0 && !force) {
        (void)printf("[fsck.wfs] %s is mounted; unmount it or pass --force\n", id);
        return 1;
    }
    if (claimed < 0 && !force) {
        (void)printf("[fsck.wfs] cannot tell whether %s is mounted -- no volume manager, or no "
                     "volume on it; pass --force if it is idle\n",
                     id);
        return 1;
    }
    if (claimed > 0) {
        /* --force on a volume that IS claimed. Named plainly: the findings below
         * may be artefacts of a concurrent writer rather than damage. */
        (void)printf("[fsck.wfs] %s is mounted and --force was given; findings may be races\n", id);
    }

    count = wasmos_svc_lookup_class(
        proc_endpoint, g_reply_endpoint, "block", providers, FSCK_MAX_PROVIDERS, g_request_id++);
    if (count <= 0) {
        (void)printf("[fsck.wfs] no block devices\n");
        return 1;
    }
    for (i = 0; i < count; ++i) {
        if (providers[i].instance == g_target) {
            g_endpoint = (int32_t)providers[i].endpoint;
            break;
        }
    }
    if (g_endpoint < 0) {
        (void)printf("[fsck.wfs] no block device with id %s\n", id);
        return 1;
    }
    g_buffer_phys = wasmos_block_buffer_phys();
    if (g_buffer_phys < 0) {
        (void)printf("[fsck.wfs] no block buffer\n");
        return 1;
    }

    /* The descriptor buffer is acquired AFTER the endpoint is known, because the
     * borrow names that endpoint. */
    g_req_bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(wasmos_block_request_t));
    if (g_req_bid < 0 ||
        wasmos_xfer_buffer_borrow(g_endpoint, g_req_bid, WASMOS_BUFFER_GRANT_READ) < 0) {
        (void)printf("[fsck.wfs] no request buffer\n");
        return 1;
    }

    (void)printf("[fsck.wfs] checking %s%s\n", id, repair ? ", repairing" : ", read-only");

    io.read_block = io_read;
    io.write_block = repair ? io_write : 0;
    io.user = 0;
    rc = wfs_fsck_run(&io, on_finding, 0, &rep);

    (void)printf("[fsck.wfs] %u objects, %u blocks in use\n",
                 (unsigned)rep.objects_in_use,
                 (unsigned)rep.blocks_in_use);
    if (rep.bitmap_errors || rep.counter_errors) {
        (void)printf("[fsck.wfs] derived state: %u bitmap, %u counter, %u written back\n",
                     (unsigned)rep.bitmap_errors,
                     (unsigned)rep.counter_errors,
                     (unsigned)rep.repaired);
    }
    if (rc == WASMOS_ERR_FS_CORRUPT) {
        (void)printf("[fsck.wfs] structural damage: this tool does not rewrite it, and the "
                     "volume stays unclean\n");
        return 2;
    }
    if (rc == WASMOS_ERR_FS_GEOMETRY) {
        (void)printf("[fsck.wfs] this volume's block size is larger than the %u-byte block "
                     "buffer a process gets; cannot check it from the guest\n",
                     (unsigned)FSCK_BUFFER_BYTES);
        return 1;
    }
    if (rc != WASMOS_ERR_NONE) {
        (void)printf("[fsck.wfs] check failed: %s\n", wasmos_strerror(rc));
        return 1;
    }
    if (rep.cleared_state) {
        (void)printf("[fsck.wfs] clean\n");
    } else {
        (void)printf("[fsck.wfs] consistent (run with --repair to record it clean)\n");
    }
    return 0;
}
