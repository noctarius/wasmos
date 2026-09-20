/* volinfo - list the VOLUMES a system holds, and what each one sits on.
 *
 * The rung between `blkinfo` and `mount`. blkinfo answers "what storage exists"
 * and mount answers "what is in the namespace"; neither answers "what could be
 * mounted", which is the volume layer -- a volume is a thing with a filesystem
 * on it, whether that is a partition or a whole unpartitioned disk, and it exists
 * whether or not anything has mounted it.
 *
 * Each volume is reported with the device beneath it, and for a partition-backed
 * volume the disk beneath THAT, so one listing spans all three layers:
 *
 *   volume:block:ata:0p1  fat  "QEMU VVFAT"  uuid=fd1abefa  1032129 sectors
 *     partition block:ata:0p1  slot 1  lba 63..1032192
 *     disk      block:ata:0    ata unit 0
 *
 * The ids printed are the ones other tools take: the volume's backing id is what
 * `mount -t <type> <path> <id>` and `fsck.wfs <id>` accept, so a reader can carry
 * a line from here to either without translating it.
 *
 * Every volume is listed, mounted or not: this reads the `volume` service class,
 * which is what the volume manager publishes when a recogniser finds a filesystem
 * on a device, and a volume stays published after whatever mounted it goes away.
 * That is the question neither neighbour answers -- blkinfo knows devices and
 * `mount` knows the namespace, and "what could be mounted" is in between.
 *
 * CLAIMED is not "is mounted". It is VOLUME_DESCRIPTOR_FLAG_CLAIMED, which a
 * filesystem sets over its volume while it holds it, and only fs_wfs does -- a
 * FAT volume reads unclaimed whether or not it is mounted. So the column answers
 * "would fsck.wfs refuse this", which is the question worth asking of a volume
 * and the one that otherwise has to be answered by being turned away. Read the
 * mount table for the namespace.
 *
 * Every provider is queried from ONE reply endpoint. An endpoint is released only
 * when its process exits -- there is no close -- so a tool that made one per item
 * spends them permanently and runs out partway through its own listing.
 *
 * Blocking IPC is correct here: this is a one-shot utility that runs, prints and
 * exits, so it may wait on a reply the way src/utils/blkinfo does. A SERVICE
 * could not -- it would stall its event loop.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"

/* Providers detailed per run. A volume exists per formatted partition, so a
 * machine with two partitioned disks already publishes several. */
#define VOLINFO_MAX_PROVIDERS 12

static int32_t g_reply_endpoint = -1;
static int32_t g_request_id = 1;

/* The filesystem a recogniser found on the volume. A value rather than a guess
 * from the device: an unformatted volume is not published at all, and an
 * unrecognised one reports UNKNOWN rather than being omitted. */
static const char* fs_type_name(uint32_t fs_type) {
    switch (fs_type) {
    case (uint32_t)FS_TYPE_FAT:
        return "fat";
    case (uint32_t)FS_TYPE_WFS:
        return "wfs";
    case (uint32_t)FS_TYPE_INITFS:
        return "initfs";
    case (uint32_t)FS_TYPE_TMPFS:
        return "tmpfs";
    default:
        return "unknown";
    }
}

static const char* block_backend_name(uint32_t backend) {
    switch (backend) {
    case (uint32_t)BLOCK_BACKEND_ATA:
        return "ata";
    case (uint32_t)BLOCK_BACKEND_VIRTIO_BLK:
        return "virtio-blk";
    default:
        return "unknown";
    }
}

/* The block device a volume sits on, as a canonical id.
 *
 * DERIVED, not asked for: a volume's canonical id is `volume:` prefixed to its
 * backing device's (architecture/37 section 3), so stripping the prefix yields
 * the backing id without a lookup -- and that string is exactly what `mount -t`
 * and `fsck.wfs` take. Returns 0 when the id does not carry the prefix, which
 * means the volume manager built it some other way and nothing here should
 * pretend to know how. */
static const char* backing_id_of(const char* volume_id) {
    static const char prefix[] = "volume:";
    uint32_t i = 0;
    if (!volume_id) {
        return 0;
    }
    while (prefix[i] != '\0') {
        if (volume_id[i] != prefix[i]) {
            return 0;
        }
        ++i;
    }
    return volume_id[i] != '\0' ? volume_id + i : 0;
}

/* The disk a partition id names, written into `out`.
 *
 * A partition's id is its disk's with `p<slot>` appended (`block:ata:0p1`), so
 * the disk is the text before the final 'p'. Returns 0 when there is no such
 * suffix, which is what a whole-disk volume looks like -- not an error, just a
 * volume with no partition between it and its disk. */
static const char* disk_id_of(const char* block_id, char* out, uint32_t out_cap) {
    uint32_t len = 0;
    uint32_t cut = 0;
    if (!block_id || !out || out_cap == 0u) {
        return 0;
    }
    while (block_id[len] != '\0') {
        ++len;
    }
    for (uint32_t i = len; i > 0u; --i) {
        if (block_id[i - 1u] == 'p') {
            cut = i - 1u;
            break;
        }
    }
    if (cut == 0u || cut + 1u >= out_cap) {
        return 0;
    }
    for (uint32_t i = 0; i < cut; ++i) {
        out[i] = block_id[i];
    }
    out[cut] = '\0';
    return out;
}

/* IDENTIFY one provider of `class_name` at `instance` into `out`, `out_bytes`
 * long. The client owns the buffer and holds its lifecycle; release
 * cascade-revokes the server's grant on every path out
 * (docs/architecture/12-dma-transfers.md). Returns a packed code. */
static int32_t identify(int32_t endpoint, int32_t req_type, uint32_t instance, void* out,
                        uint32_t out_bytes) {
    wasmos_ipc_message_t reply;
    int32_t bid;
    int32_t rc;

    bid = wasmos_xfer_buffer_acquire((int32_t)out_bytes);
    if (bid < 0) {
        return WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR;
    }
    if (wasmos_xfer_buffer_borrow(endpoint, bid, WASMOS_BUFFER_GRANT_WRITE) < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR;
    }
    rc = wasmos_ipc_call(
        endpoint, g_reply_endpoint, req_type, g_request_id++, (int32_t)instance, bid, 0, 0, &reply);
    if (rc != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_BLOCK_DEV_NOT_READY;
    }
    /* A reply that is not the RESP carries the server's own reason in arg0;
     * relayed unchanged, because "identify failed" with nothing behind it cannot
     * tell a device that refused from one that answered something unreadable. */
    if (reply.arg0 != 0) {
        int32_t code = reply.arg0;
        (void)wasmos_xfer_buffer_release(bid);
        return code;
    }
    rc = wasmos_xfer_buffer_read(bid, out, (int32_t)out_bytes, 0);
    (void)wasmos_xfer_buffer_release(bid);
    return rc == 0 ? 0 : WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR;
}

/* Print the block device `id` names, indented under its volume. `lead` labels
 * the layer, so one helper serves the partition line and the disk line. */
static void print_block(const svc_class_entry_t* providers, int32_t count, const char* lead,
                        const char* id) {
    wasmos_block_descriptor_t desc = {0};
    uint32_t instance = wasmos_block_fingerprint(id);
    int32_t rc = WASMOS_ERR_BLOCK_DEV_NO_SUCH_UNIT;

    for (int32_t i = 0; i < count; ++i) {
        if (providers[i].instance != instance) {
            continue;
        }
        rc = identify(
            (int32_t)providers[i].endpoint, BLOCK_IPC_IDENTIFY_REQ, instance, &desc, sizeof(desc));
        break;
    }
    if (rc != 0) {
        (void)printf("  %s %s (%s)\n", lead, id, wasmos_error_code_name(rc));
        return;
    }
    if (desc.version != BLOCK_DESCRIPTOR_VERSION) {
        (void)printf("  %s %s (descriptor version %u)\n", lead, id, (unsigned)desc.version);
        return;
    }
    if (desc.partition != 0u) {
        (void)printf("  %s %s  slot %u  lba %llu..%llu\n",
                     lead,
                     id,
                     (unsigned)desc.partition,
                     (unsigned long long)desc.lba_start,
                     (unsigned long long)(desc.lba_start + desc.lba_count));
        return;
    }
    (void)printf("  %s %s  %s unit %u  %llu sectors\n",
                 lead,
                 id,
                 block_backend_name(desc.backend),
                 (unsigned)desc.unit,
                 (unsigned long long)desc.lba_count);
}

int main(void) {
    svc_class_entry_t volumes[VOLINFO_MAX_PROVIDERS];
    svc_class_entry_t blocks[VOLINFO_MAX_PROVIDERS];
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();
    int32_t volume_count;
    int32_t block_count;
    int32_t shown;

    if (proc_endpoint < 0) {
        (void)printf("[volinfo] no process-manager endpoint\n");
        return 1;
    }
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        (void)printf("[volinfo] endpoint create failed\n");
        return 1;
    }

    volume_count = wasmos_svc_lookup_class(
        proc_endpoint, g_reply_endpoint, "volume", volumes, VOLINFO_MAX_PROVIDERS, g_request_id++);
    if (volume_count < 0) {
        (void)printf("[volinfo] no volume manager\n");
        return 1;
    }
    /* Resolved once, not per volume: every backing device is a provider of the
     * same class, and the lookup is the expensive half of finding one. */
    block_count = wasmos_svc_lookup_class(
        proc_endpoint, g_reply_endpoint, "block", blocks, VOLINFO_MAX_PROVIDERS, g_request_id++);
    if (block_count < 0) {
        block_count = 0;
    }
    if (block_count > VOLINFO_MAX_PROVIDERS) {
        block_count = VOLINFO_MAX_PROVIDERS;
    }

    (void)printf("[volinfo] volumes=%d\n", (int)volume_count);
    if (volume_count == 0) {
        return 0;
    }
    shown = volume_count < VOLINFO_MAX_PROVIDERS ? volume_count : VOLINFO_MAX_PROVIDERS;
    for (int32_t i = 0; i < shown; ++i) {
        /* Zeroed rather than left indeterminate: it is filled through
         * wasmos_xfer_buffer_read, an address-as-integer host call whose write no
         * analyser can see. */
        wasmos_volume_descriptor_t desc = {0};
        char disk[VOLUME_DESCRIPTOR_ID_MAX];
        const char* backing;
        int32_t rc = identify((int32_t)volumes[i].endpoint,
                              VOLUME_IPC_IDENTIFY_REQ,
                              volumes[i].instance,
                              &desc,
                              sizeof(desc));
        if (rc != 0) {
            (void)printf("[volinfo] instance=%u identify failed: %s\n",
                         (unsigned)volumes[i].instance,
                         wasmos_error_code_name(rc));
            continue;
        }
        if (desc.version != VOLUME_DESCRIPTOR_VERSION) {
            (void)printf("[volinfo] instance=%u descriptor version %u, not %u\n",
                         (unsigned)volumes[i].instance,
                         (unsigned)desc.version,
                         (unsigned)VOLUME_DESCRIPTOR_VERSION);
            continue;
        }

        (void)printf("[volinfo] %s %s", desc.canonical_id, fs_type_name(desc.fs_type));
        /* A volume may genuinely be named "", which is not the same as a format
         * that carries no label -- HAS_LABEL is what separates them, so the
         * quotes appear only when the volume actually has one. */
        if ((desc.flags & (uint32_t)VOLUME_DESCRIPTOR_FLAG_HAS_LABEL) != 0u) {
            (void)printf(" \"%s\"", desc.label);
        }
        if (desc.uuid_len > 0u) {
            (void)printf(" uuid=");
            for (uint32_t b = 0; b < desc.uuid_len && b < VOLUME_DESCRIPTOR_UUID_MAX; ++b) {
                (void)printf("%02x", (unsigned)desc.uuid[b]);
            }
        }
        (void)printf(" %llu sectors", (unsigned long long)desc.lba_count);
        /* What fsck.wfs refuses on -- NOT whether the volume is mounted; see the
         * header. Printed from the volume's own descriptor rather than inferred
         * from the mount table, because a claim outlives a holder that died, and
         * a volume nothing will ever agree to check is exactly the case worth
         * seeing. */
        if ((desc.flags & (uint32_t)VOLUME_DESCRIPTOR_FLAG_CLAIMED) != 0u) {
            (void)printf(" claimed");
        }
        (void)printf("\n");

        backing = backing_id_of(desc.canonical_id);
        if (!backing) {
            continue;
        }
        if (disk_id_of(backing, disk, sizeof(disk))) {
            print_block(blocks, block_count, "partition", backing);
            print_block(blocks, block_count, "disk     ", disk);
        } else {
            print_block(blocks, block_count, "disk     ", backing);
        }
    }
    if (volume_count > shown) {
        (void)printf("[volinfo] %d more not shown\n", (int)(volume_count - shown));
    }
    return 0;
}
