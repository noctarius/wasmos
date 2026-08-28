/* fsck.wfs - check a WFS volume on a block device, from inside the OS.
 *
 * The checks are wfs_fsck.c, which is the same code the host unit suites run
 * against volumes mkfs_wfs produced. This file is only the part that cannot be
 * shared: finding the disk, and moving blocks over the block protocol.
 *
 * Usage: fsck.wfs [--repair] <instance>
 *
 *   instance   a `block` class instance, as blkinfo prints it: (backend << 8) |
 *              unit. There is no default. Checking is harmless, but --repair
 *              writes, and a tool whose most destructive invocation is one
 *              omitted argument away from an arbitrary disk is a bad tool.
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
 * The volume must NOT be mounted, and that is enforced BENEATH this tool: a
 * block driver binds a unit to one client exclusively, so a disk a filesystem
 * driver holds is refused with block_dev.UNIT_CLAIMED before a single sector is
 * read. That is the right answer -- checking a volume someone else is writing
 * reports damage that is merely a race -- but it also means that in a default
 * boot, where the device-manager rules spawn a filesystem driver for every disk
 * it recognises, there is no disk left for this tool to check.
 * TODO: a way to release a mounted volume from the guest. Until one exists this
 * runs against a disk no rule claimed, and the host unit suite
 * (tests/unit/test_wfs_fsck.c) is what exercises the checks themselves.
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

static int32_t g_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static int32_t g_request_id = 1;
static int32_t g_buffer_phys;

static uint8_t instance_unit(uint32_t instance) {
    return (uint8_t)(instance & 0xFFu);
}

static uint32_t parse_u32(const char* s, uint32_t fallback) {
    uint32_t v = 0;
    int any = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        any = 1;
        s++;
    }
    return any ? v : fallback;
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
static wasmos_error_code_t io_read(void* user, uint32_t block, void* out, uint32_t len) {
    wasmos_ipc_message_t reply;
    uint64_t offset = (uint64_t)block * len;

    (void)user;
    if ((len % FSCK_SECTOR_BYTES) != 0u) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    if (wasmos_ipc_call(g_endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_READ_REQ,
                        g_request_id++,
                        g_buffer_phys,
                        (int32_t)(offset / FSCK_SECTOR_BYTES),
                        (int32_t)(len / FSCK_SECTOR_BYTES),
                        0,
                        &reply) != 0) {
        return WASMOS_ERR_FS_IO;
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
    uint64_t offset = (uint64_t)block * len;

    (void)user;
    if ((len % FSCK_SECTOR_BYTES) != 0u) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    if (wasmos_block_buffer_write(
            g_buffer_phys, addr_cast(int32_t, (void*)(uintptr_t)in), (int32_t)len, 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_ipc_call(g_endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_WRITE_REQ,
                        g_request_id++,
                        g_buffer_phys,
                        (int32_t)(offset / FSCK_SECTOR_BYTES),
                        (int32_t)(len / FSCK_SECTOR_BYTES),
                        0,
                        &reply) != 0) {
        return WASMOS_ERR_FS_IO;
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
    uint32_t instance;
    int32_t count;
    int32_t i;
    int repair = 0;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    rest = args;
    if (str_prefix(rest, "--repair")) {
        repair = 1;
        rest += 8;
        while (*rest == ' ') {
            rest++;
        }
    }
    if (*rest < '0' || *rest > '9') {
        (void)printf("usage: fsck.wfs [--repair] <instance>   (blkinfo lists instances)\n");
        return 1;
    }
    instance = parse_u32(rest, 0u);

    if (proc_endpoint < 0) {
        (void)printf("[fsck.wfs] no process-manager endpoint\n");
        return 1;
    }
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        (void)printf("[fsck.wfs] no reply endpoint\n");
        return 1;
    }
    count = wasmos_svc_lookup_class(
        proc_endpoint, g_reply_endpoint, "block", providers, FSCK_MAX_PROVIDERS, g_request_id++);
    if (count <= 0) {
        (void)printf("[fsck.wfs] no block devices\n");
        return 1;
    }
    for (i = 0; i < count; ++i) {
        if (providers[i].instance == instance) {
            g_endpoint = (int32_t)providers[i].endpoint;
            break;
        }
    }
    if (g_endpoint < 0) {
        (void)printf("[fsck.wfs] no block device with instance %u\n", (unsigned)instance);
        return 1;
    }
    g_buffer_phys = wasmos_block_buffer_phys();
    if (g_buffer_phys < 0) {
        (void)printf("[fsck.wfs] no block buffer\n");
        return 1;
    }

    (void)printf("[fsck.wfs] checking instance %u (unit %u)%s\n",
                 (unsigned)instance,
                 (unsigned)instance_unit(instance),
                 repair ? ", repairing" : ", read-only");

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
