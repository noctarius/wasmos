/* fs_fat.c - FAT backend service: the event reactor.
 *
 * Owns the driver's singleton state (block layer, mount geometry, open-file
 * pool) and a fixed pool + FIFO of in-flight operation contexts.  Accepts many
 * client requests but drives ONE active op at a time (the single block buffer
 * serializes physical I/O); each op is a resumable coroutine advanced across
 * block-I/O completions.  No blocking anywhere except the top-level select_wait.
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

/* In-flight op pool + FIFO of queued (not-yet-active) op indices. */
static fat_op_ctx_t g_ops[FAT_MAX_INFLIGHT];
static uint32_t g_fifo[FAT_MAX_INFLIGHT];
static uint32_t g_fifo_head = 0;
static uint32_t g_fifo_tail = 0;
static uint32_t g_fifo_len = 0;
static fat_op_ctx_t* g_active = 0;

static void fat_stall(void) {
    int32_t ep = wasmos_ipc_create_endpoint();
    for (;;) {
        if (ep >= 0) {
            (void)wasmos_ipc_select_one(ep);
        }
    }
}

/* --- init-time handshakes (one-time, pre-reactor; synchronous by nature). --- */

static int32_t fat_parse_requested_unit(void) {
    char args[64];
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

/* Answer an fs-manager FSMGR_IPC_BACKEND_INFO_REQ pull. */
static void fat_report_backend_info(int32_t dst, int32_t request_id) {
    int32_t mount_arg = 0;
    if (g_mount_bid >= 0 && g_mount_len > 0 &&
        wasmos_xfer_buffer_borrow(dst, g_mount_bid, WASMOS_BUFFER_GRANT_READ) >= 0) {
        mount_arg = (int32_t)(((uint32_t)g_mount_bid << 12) | ((uint32_t)g_mount_len & 0xFFFu));
    }
    (void)wasmos_ipc_send(dst, g_fs_endpoint, FSMGR_IPC_BACKEND_INFO_RESP, request_id,
                          FSMGR_BACKEND_BOOT, 0, mount_arg, (int32_t)g_mount_unit);
}

/* Resolve the mount alias + unit via BLOCK_IPC_IDENTIFY + devmgr query. */
static int fat_resolve_mount_alias(char* out_mount, uint32_t out_mount_len, uint8_t* out_unit) {
    int32_t reply = g_blk.reply_endpoint;
    int32_t devmgr = -1;
    int32_t req_id = 41;
    int32_t unit = 0;
    uint32_t packed[4];
    if (!out_mount || out_mount_len < 2u || !out_unit) {
        return -1;
    }
    out_mount[0] = '\0';
    *out_unit = 0;
    if (wasmos_ipc_send(g_blk.block_endpoint, reply, BLOCK_IPC_IDENTIFY_REQ, req_id,
                        g_requested_unit, 0, 0, 0) != 0 ||
        wasmos_ipc_select_one(reply) < 0) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != BLOCK_IPC_IDENTIFY_RESP ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req_id) {
        return -1;
    }
    unit = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
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
    case FAT_OP_MKDIR:
        return fat_op_mkdir(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_RMDIR:
        return fat_op_rmdir(op, &g_blk, &g_mnt, &g_pool);
    case FAT_OP_READDIR:
        return fat_op_readdir(op, &g_blk, &g_mnt, g_fs_endpoint);
    case FAT_OP_CHDIR:
        return fat_op_chdir(op, &g_blk, &g_mnt);
    default:
        op->err = -(int32_t)WASMOS_ERR_FS_UNSUPPORTED;
        return FAT_R_ERR;
    }
}

static void fat_send_response(fat_op_ctx_t* op, fat_r_t r) {
    if (r == FAT_R_DONE) {
        int32_t a0 = op->resp_override ? op->resp_arg0 : 0;
        int32_t a1 = op->resp_override ? op->resp_arg1 : 0;
        (void)wasmos_ipc_send(op->source, g_fs_endpoint, FS_IPC_RESP, op->request_id, a0, a1, 0, 0);
    } else {
        int32_t err = op->err ? op->err : -1;
        (void)wasmos_ipc_send(op->source, g_fs_endpoint, FS_IPC_ERROR, op->request_id, err, 0, 0,
                              0);
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

/* Drive the mount coroutine to completion at init (before signalling ready).
 * Reuses the reactor's own mount step + block-completion — not a separate
 * synchronous read path.  Blocking on the reply here is fine: it is one-time
 * bring-up with no client ops in flight (like the IDENTIFY / backend-info
 * handshakes).  A mount failure is fatal, as in the original driver. */
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

WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t block_endpoint,
                                      int32_t ignored_arg2, int32_t ignored_arg3) {
    int32_t reply_endpoint;
    int32_t sel;
    char mount_alias[16];
    char service_name[16];
    int32_t mount_alias_len;

    (void)ignored_arg2;
    (void)ignored_arg3;
    g_proc_endpoint = wasmos_startup_proc_endpoint();

    g_fs_endpoint = wasmos_ipc_create_endpoint();
    reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_fs_endpoint < 0 || reply_endpoint < 0) {
        fat_log("endpoint create failed\n");
        fat_stall();
    }

    g_requested_unit = fat_parse_requested_unit();
    block_endpoint = (g_requested_unit < 0 && block_endpoint > 0) ? block_endpoint : -1;
    if (block_endpoint <= 0) {
        for (;;) {
            block_endpoint = wasmos_svc_lookup(g_proc_endpoint, reply_endpoint, "block", 1);
            if (block_endpoint > 0) {
                break;
            }
            (void)wasmos_sched_yield();
        }
    }

    fat_block_configure(&g_blk, block_endpoint, reply_endpoint);
    if (fat_block_setup(&g_blk) != 0) {
        fat_log("block buffer missing\n");
        fat_stall();
    }
    fat_mount_init(&g_mnt);
    fat_open_pool_init(&g_pool);
    /* Mount eagerly, before signalling ready, so the driver advertises a
     * validated, parsed volume (matching the pre-rewrite fat_ensure_ready). */
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
    if (g_mount_bid < 0 || wasmos_xfer_buffer_write(g_mount_bid, addr_cast(int32_t, mount_alias),
                                                    mount_alias_len, 0) != 0) {
        fat_log("mount alias buffer write failed\n");
        fat_stall();
    }
    g_mount_len = mount_alias_len;
    if (wasmos_svc_register_class(g_proc_endpoint, g_fs_endpoint, service_name, FSMGR_BACKEND_CLASS,
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
                        g_active->err = -(int32_t)WASMOS_ERR_FS_IO;
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
                (void)wasmos_ipc_send(
                    wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE), g_fs_endpoint, FS_IPC_RESP,
                    wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID), 0, 0, 0, 0);
                continue;
            }
            op = fat_op_alloc();
            if (!op) {
                (void)wasmos_ipc_send(
                    wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE), g_fs_endpoint, FS_IPC_ERROR,
                    wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID), -(int32_t)WASMOS_ERR_FS_BUSY, 0, 0, 0);
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
                fat_unpack_name((uint32_t)op->arg0, (uint32_t)op->arg1, (uint32_t)op->arg2,
                                (uint32_t)op->arg3, op->dir_name, sizeof(op->dir_name));
            }
            fat_fifo_push(op);
        }
    }
    return 0;
}
