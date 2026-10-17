/* mkfs.wfs - write a fresh WFS volume onto a block device, from inside the OS.
 *
 * The layout and every byte of it come from src/tools/mkfs_wfs/wfs_mkfs.c, the
 * same formatter the host tool runs and the host unit suites check. This file is
 * only the part that cannot be shared: finding the disk, refusing one that is in
 * use, and moving blocks over the block protocol.
 *
 * Usage: mkfs.wfs [--force] [--zero] [--block-size N] <canonical-id>
 *
 *   canonical-id  the disk's id as blkinfo prints it, e.g. `block:ata:2`. The
 *                 `block` class instance is its fingerprint, so the id is both
 *                 how the provider is found and how it is told which of its
 *                 disks is meant. There is no default, and there will not be
 *                 one: this tool destroys a disk, and a destructive tool whose
 *                 worst invocation is one omitted argument away from an
 *                 arbitrary device is a bad tool.
 *
 * This ALWAYS writes. There is no dry run, because the formatter's planning half
 * already answers every question a dry run could -- `volinfo` and `blkinfo`
 * report what is there now, and a plan that was not written changes nothing.
 *
 * WHAT IS WRITTEN, AND WHY NOT EVERY BLOCK
 *
 * By default only the blocks a format DEFINES are written: the superblock, the
 * group descriptors, the journal, the object table, the bitmaps, the root's
 * directory blocks, and the backup superblock of every group that carries one.
 * The free data area is left alone.
 *
 * That is sound because nothing reaches a free block. Every region is located
 * from the superblock, every allocation is governed by the bitmaps, and the only
 * object on a fresh volume is the root -- so a stale block is unreferenced by
 * construction. The one thing a stale block COULD be mistaken for is a backup
 * superblock, which a scan looks for at group boundaries (§5), and those are
 * written rather than skipped for exactly that reason.
 *
 * It matters because the difference is not small. Each block is one awaited
 * BLOCK request, and a 16 MiB volume is 4096 of them: measured at 110s written
 * whole against 4s written this way. A disk of any real size is unusable
 * otherwise.
 *
 * --zero writes every block, which is what the host tool does when it builds an
 * image file. Use it to erase what was there; it costs the full time.
 *
 * WHY THE FORMATTER IS NOT CALLED THE WAY THE HOST TOOL CALLS IT
 *
 * wfs_mkfs_format drives a sink whose write_block returns inline. In a guest a
 * block write is a BLOCK request that has to be awaited, so the sink cannot be
 * implemented here at all. Instead this uses the formatter's two halves
 * directly: wfs_mkfs_plan_tree settles the layout once, and wfs_mkfs_block_at
 * serializes one block at a time. block_at is pure, so the loop below may await
 * between blocks and may repeat a block after a failed write.
 *
 * Every request rides the SYSTEM IPC-future bridge through wfs_block.c -- the
 * WFS driver's own block client, reused rather than reimplemented. Nothing here
 * sends and then waits for its own answer; the format is a coroutine that parks
 * at each write and is resumed by the loop. Service DISCOVERY at startup is the
 * one synchronous step, as it is in every driver and utility.
 *
 * THE BLOCK SIZE IS CAPPED BELOW WHAT THE FORMAT PERMITS
 *
 * WFS allows 4096, 8192 and 16384. The per-process block buffer every transfer
 * passes through is 8 KiB, so a 16384-byte block cannot be moved from a guest
 * and this tool refuses it. That is a limit of the guest transport, not of the
 * format: the host tool writes such volumes, and fsck.wfs reports the same limit
 * from the reading side.
 *
 * THE VOLUME MUST NOT BE MOUNTED, AND THIS TOOL IS WHAT CHECKS THAT
 *
 * It asks the volume manager whether a volume on this device is CLAIMED -- the
 * flag a filesystem service sets while it holds one -- and refuses if so.
 * Formatting under a mounted filesystem destroys the volume beneath a service
 * that is still writing to it, and the damage surfaces much later as corruption
 * nobody can attribute.
 *
 * The check is ADVISORY on both sides (architecture/37-volume-manager.md §5):
 * the volume manager is not in the I/O path, and nothing beneath this tool
 * refuses a second client, so a tool that skipped the question would reach the
 * sectors unimpeded.
 *
 * Unlike fsck.wfs, "no volume covers this device" is the NORMAL case here and is
 * allowed: a blank or repurposed disk has no volume until this tool puts a
 * filesystem on it. What is refused is the case fsck also refuses -- no volume
 * manager to ask, so a mounted volume cannot be ruled out. --force overrides
 * both, and exists because a claim can outlive its holder.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"

/* wfs_ops.h is included for WFS_AWAIT, the await-and-resume-here macro every
 * WFS task is written with. Its other declarations belong to the driver and are
 * unused here. */
#include "wfs_block.h"
#include "wfs_mkfs.h"
#include "wfs_ops.h"
#include "wfs_super.h"

#define MKFS_MAX_PROVIDERS 8
/* The per-process block buffer bounds one transfer; see the header. */
#define MKFS_BUFFER_BYTES 8192u
#define MKFS_DEFAULT_BLOCK_SIZE 4096u

static wasmos_wasm_runtime_t g_runtime;
static wasmos_sys_event_loop_t g_loop;
static wfs_block_t g_blk;
static int32_t g_proc_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static int32_t g_request_id = 1;

/* Run one task to completion. The poll parks when nothing is queued, so a format
 * waiting on a device costs no CPU. */
static int32_t mkfs_run(wasmos_wasm_task_resume_fn fn, void* ctx) {
    wasmos_wasm_coroutine_t task;
    uint8_t* p = (uint8_t*)&task;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(task); ++i) {
        p[i] = 0u;
    }
    if (!wasmos_async_start(&g_runtime, &task, fn, ctx)) {
        return WASMOS_ERR_FS_BUSY;
    }
    for (;;) {
        int32_t status = 0;

        (void)wasmos_wasm_coroutine_run_budget(&g_runtime, 32u);
        if (task.state == WASMOS_WASM_COROUTINE_DEAD) {
            return wasmos_wasm_coroutine_join(&task, &status);
        }
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
    }
}

/* ---- the format task ------------------------------------------------------ */

enum {
    MKFS_PC_START = 0,
    MKFS_PC_WRITTEN,
    MKFS_PC_FLUSHED,
};

typedef struct {
    uint32_t pc;
    wasmos_error_code_t err;

    wfs_mkfs_params_t params;
    wfs_mkfs_layout_t layout;
    /* One slot, for the root: this tool formats an EMPTY volume, so the tree has
     * no entries. wfs_mkfs_plan_tree writes the root into slot `count`. */
    wfs_mkfs_node_t plan[1];

    uint32_t block;
    /* Blocks actually sent, which is the whole volume only under --zero. */
    uint32_t written;
    uint8_t zero_everything;
} mkfs_ctx_t;

/* Serialize `block` straight into the block client's staging buffer, so a block
 * exists in exactly one place and is never copied. */
static wasmos_error_code_t stage(mkfs_ctx_t* c) {
    return wfs_mkfs_block_at(
        &c->params, 0, 0u, c->plan, &c->layout, c->block, wfs_block_data(&g_blk));
}

/* Whether the format DEFINES this block, as opposed to leaving it free.
 *
 * Everything below first_data_block is metadata, and the root's directory blocks
 * sit at the head of the data area. Past that a fresh volume holds nothing but
 * free space -- used_blocks is 0 with no entries -- except the backup
 * superblocks, which a scan looks for at group boundaries (§5) and which must
 * therefore be written over whatever was there. */
static int block_is_defined(const mkfs_ctx_t* c, uint32_t block) {
    const wfs_mkfs_layout_t* L = &c->layout;

    if (block < L->first_data_block + L->root_blocks) {
        return 1;
    }
    return (block % L->blocks_per_group) == 0u &&
           wfs_super_group_has_backup(block / L->blocks_per_group);
}

/* Advance to the next block this run writes, or past the end when none is left.
 * Under --zero every block qualifies, so this is the identity plus one. */
static void advance(mkfs_ctx_t* c) {
    do {
        c->block++;
    } while (c->block < c->layout.total_blocks && !c->zero_everything &&
             !block_is_defined(c, c->block));
}

static int32_t mkfs_format_task(void* user, uintptr_t* out_value) {
    mkfs_ctx_t* ctx = (mkfs_ctx_t*)user;
    wfs_block_t* b = &g_blk;

    (void)out_value;
    switch (ctx->pc) {
    case MKFS_PC_START:
        ctx->err = wfs_mkfs_plan_tree(&ctx->params, 0, 0u, ctx->plan, &ctx->layout);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->err = wfs_block_set_block_size(b, ctx->layout.block_size);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->block = 0u;
        ctx->err = stage(ctx);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->block), MKFS_PC_WRITTEN);
        /* fall through */

    case MKFS_PC_WRITTEN:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        ctx->written++;
        advance(ctx);
        if (ctx->block < ctx->layout.total_blocks) {
            /* The staged buffer is the one just written, so it is free to hold
             * the next block. block_at fills it whole, zeroes included, which is
             * what keeps the previous block's bytes out of this one. */
            ctx->err = stage(ctx);
            if (ctx->err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->err;
            }
            WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->block), MKFS_PC_WRITTEN);
            return WASMOS_WASM_TASK_YIELDED;
        }
        /* Durability is the point of the last step: without a flush the volume
         * exists only in the device's write cache, and a power loss between here
         * and the first mount leaves a half-written filesystem that reads as
         * damage rather than as an interrupted format. */
        WFS_AWAIT(ctx, wfs_block_flush_begin(b), MKFS_PC_FLUSHED);
        /* fall through */

    case MKFS_PC_FLUSHED:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        break;
    }
    return (int32_t)WASMOS_ERR_FS_CORRUPT;
}

/* ---- startup -------------------------------------------------------------- */

static int str_prefix(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static uint32_t parse_u32(const char** p) {
    uint32_t v = 0;
    const char* s = *p;

    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        ++s;
    }
    *p = s;
    return v;
}

/* The endpoint serving `instance` in class `name`, or -1.
 *
 * Synchronous, and deliberately: discovery runs once at startup, before any
 * volume is touched, and is the same helper every driver and utility uses. */
static int32_t lookup(const char* name, uint32_t instance, int* out_class_present) {
    svc_class_entry_t providers[MKFS_MAX_PROVIDERS];
    int32_t count;
    int32_t i;

    count = wasmos_svc_lookup_class(
        g_proc_endpoint, g_reply_endpoint, name, providers, MKFS_MAX_PROVIDERS, g_request_id++);
    if (out_class_present) {
        *out_class_present = count > 0;
    }
    if (count <= 0) {
        return -1;
    }
    if (count > MKFS_MAX_PROVIDERS) {
        count = MKFS_MAX_PROVIDERS;
    }
    for (i = 0; i < count; ++i) {
        if (providers[i].instance == instance) {
            return (int32_t)providers[i].endpoint;
        }
    }
    return -1;
}

/* Whether a filesystem holds a volume on `device_id`.
 *
 * Returns 1 claimed, 0 safe to format -- which covers both "a volume is there
 * and nothing holds it" and "no volume is there at all" -- and -1 when the
 * question cannot be answered because no volume manager is running.
 *
 * The distinction between the last two is the whole of why this is not shared
 * with fsck.wfs: a device with no volume is the ordinary input here and an
 * impossible one there.
 *
 * The volume's class instance is DERIVED: a volume's canonical id is `volume:`
 * prefixed to its backing device's (architecture/37-volume-manager.md §3). */
static int volume_is_claimed(const char* device_id) {
    wasmos_volume_descriptor_t desc;
    wasmos_ipc_message_t reply;
    char volume_id[VOLUME_DESCRIPTOR_ID_MAX];
    static const char prefix[] = "volume:";
    uint32_t n = 0;
    uint32_t i = 0;
    int32_t endpoint;
    int32_t bid;
    int class_present = 0;
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

    endpoint = lookup("volume", wasmos_block_fingerprint(volume_id), &class_present);
    if (endpoint < 0) {
        /* A volume manager that answers but knows no volume here means the disk
         * carries nothing anyone has claimed, which is exactly what this tool
         * expects to be handed. */
        return class_present ? 0 : -1;
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
                        (int32_t)wasmos_block_fingerprint(volume_id),
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

/* The disk's geometry, which is what the format has to be sized against. */
static int identify(int32_t endpoint, uint32_t instance, wasmos_block_descriptor_t* out) {
    wasmos_ipc_message_t reply;
    int32_t bid;
    int rc = WASMOS_ERR_BLOCK_DEV_NOT_READY;
    uint32_t i;

    /* Zeroed before the request, so every field this reads afterwards was either
     * written by the backend or is zero -- never whatever the caller's stack
     * held. The short-reply guard below makes that unreachable in practice; this
     * makes it unreachable by construction. */
    for (i = 0; i < (uint32_t)sizeof(*out); ++i) {
        ((uint8_t*)out)[i] = 0u;
    }

    bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(*out));
    if (bid < 0) {
        return WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR;
    }
    if (wasmos_xfer_buffer_borrow(endpoint, bid, WASMOS_BUFFER_GRANT_WRITE) < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR;
    }
    if (wasmos_ipc_call(endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_IDENTIFY_REQ,
                        g_request_id++,
                        (int32_t)instance,
                        bid,
                        0,
                        0,
                        &reply) != 0) {
        rc = WASMOS_ERR_BLOCK_DEV_NOT_READY;
    } else if (reply.type == BLOCK_IPC_ERROR) {
        rc = (int)reply.arg0;
    } else if (reply.type != BLOCK_IPC_IDENTIFY_RESP || reply.arg0 != 0) {
        rc = WASMOS_ERR_BLOCK_DEV_UNSUPPORTED_REQUEST;
    } else if (reply.arg1 < (int32_t)sizeof(*out) ||
               wasmos_xfer_buffer_read(bid, out, (int32_t)sizeof(*out), 0) != 0) {
        rc = WASMOS_ERR_BLOCK_DEV_DESCRIPTOR_MALFORMED;
    } else {
        rc = 0;
    }
    (void)wasmos_xfer_buffer_release(bid);
    if (rc != 0) {
        return rc;
    }
    if (out->version != BLOCK_DESCRIPTOR_VERSION) {
        return WASMOS_ERR_BLOCK_DEV_DESCRIPTOR_VERSION;
    }
    return 0;
}

/* Seed the volume's uuid.
 *
 * Every checksum on the volume is seeded with it (§10), which is what stops a
 * block transplanted from another volume from verifying here. Two volumes
 * sharing a uuid lose that, so this wants real entropy and asks the hrng service
 * for it.
 *
 * Without an hrng service the uuid is derived from the device and the volume's
 * geometry instead. That is NOT equivalent -- reformatting the same disk at the
 * same size reproduces it -- and it is accepted only because the alternative is
 * refusing to format a system that has no entropy source at all. Reported so the
 * operator knows which one they got. */
typedef struct {
    int done;
    int32_t status;
} random_state_t;

static void on_random(void* user, int32_t status) {
    random_state_t* st = (random_state_t*)user;

    st->status = status;
    st->done = 1;
}

static int fill_uuid(uint8_t* uuid, uint32_t target, uint64_t lba_count) {
    wasmos_sys_random_request_t request;
    random_state_t st;
    int32_t hrng;
    uint32_t i;
    uint64_t mix;

    hrng = lookup("hrng", 0u, 0);
    if (hrng < 0) {
        /* Class lookup by instance 0 misses a provider that registered a real
         * instance, so fall back to the first provider in the class. */
        svc_class_entry_t providers[MKFS_MAX_PROVIDERS];
        int32_t count = wasmos_svc_lookup_class(g_proc_endpoint,
                                                g_reply_endpoint,
                                                "hrng",
                                                providers,
                                                MKFS_MAX_PROVIDERS,
                                                g_request_id++);
        if (count > 0) {
            hrng = (int32_t)providers[0].endpoint;
        }
    }
    if (hrng >= 0) {
        st.done = 0;
        st.status = 0;
        if (wasmos_sys_random_bytes_async(
                &g_loop, hrng, uuid, (int32_t)WFS_UUID_LEN, &request, on_random, &st) == 0) {
            while (!st.done) {
                (void)wasmos_sys_event_loop_poll(&g_loop, 8);
            }
            if (st.status == 0) {
                return 1;
            }
        }
    }

    /* FNV-1a over the two things that identify this format: which disk, and how
     * big the volume on it is. */
    mix = 0xCBF29CE484222325ull ^ target;
    for (i = 0; i < 8u; ++i) {
        mix = (mix ^ ((lba_count >> (i * 8u)) & 0xFFu)) * 0x100000001B3ull;
    }
    for (i = 0; i < WFS_UUID_LEN; ++i) {
        mix = (mix ^ (i + 1u)) * 0x100000001B3ull;
        uuid[i] = (uint8_t)(mix >> 24);
    }
    return 0;
}

/* Ask the volume manager to re-probe the device, so the volume layer describes
 * the filesystem that was just written rather than whatever was there at boot.
 *
 * A device is probed once, when it arrives under the `block` class. Without this
 * `volinfo` keeps reporting the old type until a reboot, and so does every
 * `SUBSYSTEM=="volume"` rule -- the format would be invisible to the layer that
 * decides what gets mounted.
 *
 * Sent AFTER the flush, because a probe reads the disk: asking before the last
 * write reached media would re-probe the volume this tool is still writing.
 *
 * Advisory. The volume is on the disk whether or not anyone was told, so a
 * system with no volume manager is reported and not treated as a failure. */
static void request_rescan(const char* id, uint32_t target) {
    svc_class_entry_t providers[MKFS_MAX_PROVIDERS];
    wasmos_ipc_message_t reply;
    int32_t count;
    int32_t i;
    int32_t endpoint = -1;

    count = wasmos_svc_lookup_class(
        g_proc_endpoint, g_reply_endpoint, "volume", providers, MKFS_MAX_PROVIDERS, g_request_id++);
    if (count <= 0) {
        (void)printf("[mkfs.wfs] no volume manager; %s keeps its old volume record until "
                     "the next boot\n",
                     id);
        return;
    }
    if (count > MKFS_MAX_PROVIDERS) {
        count = MKFS_MAX_PROVIDERS;
    }
    /* Any provider of the class answers: a rescan names the DEVICE, and the
     * service serving one volume serves them all. */
    for (i = 0; i < count; ++i) {
        endpoint = (int32_t)providers[i].endpoint;
        break;
    }
    if (endpoint < 0) {
        return;
    }
    if (wasmos_ipc_call(endpoint,
                        g_reply_endpoint,
                        VOLUME_IPC_RESCAN_REQ,
                        g_request_id++,
                        (int32_t)target,
                        0,
                        0,
                        0,
                        &reply) != 0) {
        (void)printf("[mkfs.wfs] volume rescan not answered; %s keeps its old volume record\n", id);
        return;
    }
    if (reply.type == VOLUME_IPC_ERROR) {
        (void)printf("[mkfs.wfs] volume rescan refused: %s\n",
                     wasmos_error_code_name((wasmos_error_code_t)reply.arg0));
        return;
    }
    if (reply.type != VOLUME_IPC_RESP || reply.arg0 != 0) {
        (void)printf("[mkfs.wfs] volume rescan reported nothing usable\n");
        return;
    }
    if (reply.arg1 == 0) {
        /* The write succeeded and the probe found nothing, which means the two
         * disagree about what is on the disk -- worth saying plainly, because
         * the volume is the layer a mount goes through. */
        (void)printf("[mkfs.wfs] the volume manager re-probed %s and found no volume\n", id);
        return;
    }
    (void)printf("[mkfs.wfs] volume record updated\n");
}

int main(void) {
    char args[64];
    wasmos_block_descriptor_t desc;
    mkfs_ctx_t ctx;
    char id[BLOCK_DESCRIPTOR_ID_MAX];
    const char* rest;
    uint32_t block_size = MKFS_DEFAULT_BLOCK_SIZE;
    uint32_t target;
    uint32_t n = 0;
    uint32_t i;
    int32_t block_endpoint;
    int32_t status;
    int force = 0;
    int zero_everything = 0;
    int claimed;
    int real_entropy;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    rest = args;
    for (;;) {
        if (str_prefix(rest, "--force")) {
            force = 1;
            rest += 7;
        } else if (str_prefix(rest, "--zero")) {
            zero_everything = 1;
            rest += 6;
        } else if (str_prefix(rest, "--block-size")) {
            rest += 12;
            while (*rest == ' ' || *rest == '=') {
                rest++;
            }
            block_size = parse_u32(&rest);
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
        (void)printf("usage: mkfs.wfs [--force] [--zero] [--block-size N] <canonical-id>   "
                     "(blkinfo lists ids)\n");
        return 1;
    }
    if (block_size != 4096u && block_size != 8192u) {
        (void)printf("[mkfs.wfs] block size must be 4096 or 8192; 16384 is a valid WFS size but "
                     "exceeds the %u-byte block buffer a process gets\n",
                     (unsigned)MKFS_BUFFER_BYTES);
        return 1;
    }
    target = wasmos_block_fingerprint(id);

    g_proc_endpoint = wasmos_startup_proc_endpoint();
    if (g_proc_endpoint < 0) {
        (void)printf("[mkfs.wfs] no process-manager endpoint\n");
        return 1;
    }
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_reply_endpoint < 0) {
        (void)printf("[mkfs.wfs] no reply endpoint\n");
        return 1;
    }

    /* Before the device is even resolved: everything below this line writes. */
    claimed = volume_is_claimed(id);
    if (claimed > 0 && !force) {
        (void)printf("[mkfs.wfs] %s is mounted; unmount it before formatting\n", id);
        return 1;
    }
    if (claimed < 0 && !force) {
        (void)printf("[mkfs.wfs] cannot tell whether %s is mounted -- no volume manager is "
                     "running; pass --force if it is idle\n",
                     id);
        return 1;
    }
    if (claimed > 0) {
        (void)printf("[mkfs.wfs] %s is mounted and --force was given; the filesystem holding it "
                     "will be writing to a volume that no longer exists\n",
                     id);
    }

    block_endpoint = lookup("block", target, 0);
    if (block_endpoint < 0) {
        (void)printf("[mkfs.wfs] no block device with id %s\n", id);
        return 1;
    }
    status = identify(block_endpoint, target, &desc);
    if (status != 0) {
        (void)printf("[mkfs.wfs] %s: identify failed: %s\n",
                     id,
                     wasmos_error_code_name((wasmos_error_code_t)status));
        return 1;
    }
    if (desc.sector_bytes == 0u || desc.lba_count == 0u) {
        (void)printf("[mkfs.wfs] %s reports no capacity\n", id);
        return 1;
    }

    wasmos_wasm_runtime_init(&g_runtime);
    wasmos_sys_event_loop_init(&g_loop, g_reply_endpoint, 0x7000);
    if (wfs_block_configure(&g_blk,
                            &g_loop,
                            block_endpoint,
                            g_reply_endpoint,
                            wasmos_block_buffer_phys(),
                            target) != WASMOS_ERR_NONE ||
        g_blk.buf_id < 0) {
        (void)printf("[mkfs.wfs] no block buffer\n");
        return 1;
    }

    for (i = 0; i < (uint32_t)sizeof(ctx); ++i) {
        ((uint8_t*)&ctx)[i] = 0u;
    }
    ctx.params.size_bytes = desc.lba_count * desc.sector_bytes;
    ctx.params.block_size = block_size;
    /* Timestamps are left at zero. The root's are the only ones a fresh volume
     * has, and reading a clock would mean another service this tool does not
     * otherwise need; the first write through a mounted filesystem sets a real
     * time. */
    ctx.params.now_ns = 0u;
    ctx.zero_everything = (uint8_t)zero_everything;
    real_entropy = fill_uuid(ctx.params.uuid, target, desc.lba_count);

    (void)printf("[mkfs.wfs] formatting %s: %llu sectors of %u bytes, %u-byte blocks\n",
                 id,
                 (unsigned long long)desc.lba_count,
                 (unsigned)desc.sector_bytes,
                 (unsigned)block_size);
    if (!real_entropy) {
        (void)printf("[mkfs.wfs] no hrng service: the volume uuid is derived from the device and "
                     "its size, so reformatting reproduces it\n");
    }

    status = mkfs_run(mkfs_format_task, &ctx);
    if (status != 0) {
        (void)printf("[mkfs.wfs] format failed at block %u of %u: %s\n",
                     (unsigned)ctx.block,
                     (unsigned)ctx.layout.total_blocks,
                     wasmos_error_code_name((wasmos_error_code_t)status));
        return 1;
    }

    (void)printf("[mkfs.wfs] %s: %u blocks, %u objects, %u written, uuid=",
                 id,
                 (unsigned)ctx.layout.total_blocks,
                 (unsigned)ctx.layout.total_objects,
                 (unsigned)ctx.written);
    for (i = 0; i < WFS_UUID_LEN; ++i) {
        (void)printf("%02x", (unsigned)ctx.params.uuid[i]);
    }
    (void)printf("\n");
    request_rescan(id, target);
    (void)printf("[mkfs.wfs] done; mount it with: mount -t wfs <path> %s\n", id);
    return 0;
}
