/* fs_wfs.c - the WFS filesystem driver.
 *
 * A `.wap` the device manager spawns for a block unit carrying a WFS volume.
 * Mounts it, registers under the fs-manager backend class, and serves FS IPC.
 *
 * Every operation is a task on the system coroutine runtime (see wfs_ops.h and
 * docs/architecture/32); this file is the pump and the wire format around them.
 * The ops themselves — mount, path resolution, directory scan, extent walk,
 * read — are host-tested without any of this, which is why this file stays thin.
 *
 * One request is served at a time. A task yields at every block boundary, so the
 * driver is not blocked on the device, but it does not interleave clients: the
 * block layer stages a single block and a second concurrent op would fight it
 * for the staging buffer. Interleaving needs a buffer per op, which is a change
 * to wfs_block, not to this file.
 */
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"

#include "wfs_block.h"
#include "wfs_dir.h"
#include "wfs_fd.h"
#include "wfs_mount.h"
#include "wfs_ops.h"
#include "wfs_path.h"
#include "wfs_read.h"

#define WFS_SEND_RETRIES 64
#define WFS_READ_STAGE 4096u

static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;

static wasmos_wasm_runtime_t g_runtime;
static wasmos_sys_event_loop_t g_loop;
static wfs_block_t g_blk;
static wfs_volume_t g_vol;
static wfs_fd_table_t g_fds;

/* Mount identity reported to fs-manager on each backend-info pull. */
static int32_t g_mount_bid = -1;
static int32_t g_mount_len = 0;
static uint8_t g_mount_unit = 0;
static int32_t g_requested_unit = -1;

/* Op contexts, static because a task's context must outlive its awaits and the
 * driver runs one op at a time. */
/* Per-client working directory. Scoped to the source endpoint for the same
 * reason an fd is: one client's cd must not move another's. A client with no
 * entry stands at the root, so the table only ever holds clients that moved. */
#define WFS_MAX_CLIENTS 16u

typedef struct {
    int32_t owner;
    uint32_t cwd;
} wfs_client_cwd_t;

static wfs_client_cwd_t g_cwd[WFS_MAX_CLIENTS];

static uint32_t wfs_cwd_get(int32_t owner) {
    uint32_t i;

    for (i = 0; i < WFS_MAX_CLIENTS; ++i) {
        if (g_cwd[i].owner == owner) {
            return g_cwd[i].cwd;
        }
    }
    return WFS_OBJECT_ROOT;
}

/* Remember where `owner` stands. Setting the root releases the slot, so a client
 * that walks back out stops occupying one. Returns 0, or a packed code when the
 * table is full — which is a refusal to move, not a silent stay. */
static wasmos_error_code_t wfs_cwd_set(int32_t owner, uint32_t object_id) {
    uint32_t i;
    uint32_t free_slot = WFS_MAX_CLIENTS;

    for (i = 0; i < WFS_MAX_CLIENTS; ++i) {
        if (g_cwd[i].owner == owner) {
            if (object_id == WFS_OBJECT_ROOT) {
                g_cwd[i].owner = -1;
            } else {
                g_cwd[i].cwd = object_id;
            }
            return WASMOS_ERR_NONE;
        }
        if (g_cwd[i].owner < 0 && free_slot == WFS_MAX_CLIENTS) {
            free_slot = i;
        }
    }
    if (object_id == WFS_OBJECT_ROOT) {
        return WASMOS_ERR_NONE; /* already the default */
    }
    if (free_slot == WFS_MAX_CLIENTS) {
        return WASMOS_ERR_FS_NO_CLIENT_SLOT;
    }
    g_cwd[free_slot].owner = owner;
    g_cwd[free_slot].cwd = object_id;
    return WASMOS_ERR_NONE;
}

static wfs_path_ctx_t g_path;
static wfs_read_ctx_t g_read;
static wfs_dir_ctx_t g_dir;
static wfs_mount_ctx_t g_mount_ctx;
static uint8_t g_stage[WFS_READ_STAGE];

static void wfs_log(const char* msg) {
    int32_t n = 0;

    while (msg[n] != '\0') {
        n++;
    }
    if (n > 0) {
        (void)wasmos_console_write(addr_cast(int32_t, msg), n);
    }
}

/* Park permanently after a fatal init failure: wait on a private endpoint nobody
 * sends to, keeping the process alive rather than exiting, so the failure is
 * visible in the log instead of looking like a driver that was never spawned. */
static void wfs_stall(void) {
    int32_t ep = wasmos_ipc_create_endpoint();

    for (;;) {
        if (ep < 0 || wasmos_ipc_select_one(ep) < 0) {
            (void)wasmos_sched_yield();
        }
    }
}

static int32_t wfs_reply(int32_t dest, int32_t type, int32_t request_id, int32_t a0, int32_t a1) {
    return wasmos_sys_ipc_send_retry(
        dest, g_fs_endpoint, type, request_id, a0, a1, 0, 0, WFS_SEND_RETRIES);
}

/* Run one op to completion, pumping the runtime and the event loop the way the
 * ops' own host tests do: resume ready tasks, then deliver block replies, which
 * wakes whatever those tasks parked on.
 *
 * Returns the task's status: 0, or the negative packed code it failed with. */
static int32_t wfs_run(wasmos_wasm_task_resume_fn fn, void* ctx) {
    wasmos_wasm_coroutine_t task;

    wfs_ops_task_reset(&task);
    if (!wasmos_async_start(&g_runtime, &task, fn, ctx)) {
        return WASMOS_ERR_FS_BUSY;
    }
    for (;;) {
        int32_t status = 0;

        (void)wasmos_wasm_coroutine_run_budget(&g_runtime, 32u);
        if (task.state == WASMOS_WASM_COROUTINE_DEAD) {
            return wasmos_wasm_coroutine_join(&task, &status);
        }
        /* The loop owns a select set, so this parks rather than spinning: a task
         * that is not runnable is waiting on a block reply and nothing else can
         * make progress until it lands. */
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
    }
}

/* Copy a client's path out of the transfer buffer it borrowed to us. */
static wasmos_error_code_t wfs_take_path(int32_t buffer_id, uint32_t path_len, char* out,
                                         uint32_t out_len) {
    if (path_len == 0u || path_len >= out_len) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }
    if (path_len + 1u > (uint32_t)wasmos_xfer_buffer_size()) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }
    if (wasmos_sys_buffer_read(buffer_id, out, (int32_t)path_len, 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    out[path_len] = '\0';
    return WASMOS_ERR_NONE;
}

/* Resolve a client path to an object, reporting the outcome as a packed code so
 * a caller can hand it straight to the client. */
static wasmos_error_code_t wfs_resolve(int32_t owner, int32_t buffer_id, uint32_t path_len) {
    static char path[WFS_PATH_MAX];
    wasmos_error_code_t rc;
    int32_t status;

    rc = wfs_take_path(buffer_id, path_len, path, sizeof(path));
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    /* Relative to where the CLIENT stands, not to the root: `cat hello.txt`
     * inside a mount sends a bare name, and resolving it from the root would
     * look in the wrong directory — or find a different file of the same name. */
    rc = wfs_path_init_from(&g_path, &g_vol, wfs_cwd_get(owner), path, path_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    status = wfs_run(wfs_path_task, &g_path);
    if (status != 0) {
        return (wasmos_error_code_t)status;
    }
    return g_path.found ? WASMOS_ERR_NONE : WASMOS_ERR_FS_NOT_FOUND;
}

/* ---- the operations ----------------------------------------------------- */

static void wfs_do_open(int32_t src, int32_t request_id, int32_t path_len, int32_t flags,
                        int32_t buffer_id) {
    wasmos_error_code_t rc;
    int32_t fd;

    /* Read-only until the write path exists: accepting a write-open would let a
     * client believe its writes landed. */
    if (flags != 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_UNSUPPORTED, 0);
        return;
    }
    rc = wfs_resolve(src, buffer_id, (uint32_t)path_len);
    if (rc != WASMOS_ERR_NONE) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
        return;
    }
    if (g_path.object.out.type == WFS_TYPE_DIR) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_IS_DIR, 0);
        return;
    }
    fd = wfs_fd_open(&g_fds,
                     src,
                     g_path.object_id,
                     g_path.object.out.size,
                     g_path.object.out.type,
                     g_path.object.out.flags,
                     g_path.object.inline_data);
    if (fd < 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, fd, 0);
        return;
    }
    (void)wfs_reply(src, FS_IPC_RESP, request_id, fd, 0);
}

static void wfs_do_stat(int32_t src, int32_t request_id, int32_t path_len, int32_t buffer_id) {
    wasmos_error_code_t rc = wfs_resolve(src, buffer_id, (uint32_t)path_len);
    uint64_t size;
    int32_t reported;

    if (rc != WASMOS_ERR_NONE) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
        return;
    }
    /* The FS ABI carries size as int32 (tracked in docs/TASKS.md), so a larger
     * object is reported at the ceiling rather than wrapped into a small
     * plausible size. */
    size = g_path.object.out.size;
    reported = size > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)size;
    (void)wfs_reply(src, FS_IPC_RESP, request_id, reported, (int32_t)g_path.object.out.mode);
}

static void wfs_do_read(int32_t src, int32_t request_id, int32_t fd, int32_t count,
                        int32_t buffer_id) {
    wfs_open_file_t* f = wfs_fd_get(&g_fds, src, fd);
    uint32_t want;
    uint32_t done = 0u;
    struct wfs_object obj;

    if (!f) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_BAD_FD, 0);
        return;
    }
    if (count < 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_BAD_ARGS, 0);
        return;
    }
    want = (uint32_t)count;
    if (want > (uint32_t)wasmos_xfer_buffer_size()) {
        want = (uint32_t)wasmos_xfer_buffer_size();
    }

    /* The read op takes an object record; the fd carries what open latched, so a
     * read costs no second fetch of the record. */
    obj = (struct wfs_object){0};
    obj.object_id = f->object_id;
    obj.type = f->type;
    obj.flags = f->flags;
    obj.size = f->size;

    while (done < want) {
        uint32_t chunk = want - done;
        int32_t status;

        if (chunk > WFS_READ_STAGE) {
            chunk = WFS_READ_STAGE;
        }
        /* The extent map lives in the object record, which the fd does not
         * carry, so a non-inline read resolves the object again per chunk. That
         * is one extra block read per chunk and is the price of not holding a
         * record per open fd; a record cache belongs in wfs_ops, not here. */
        if ((f->flags & WFS_OBJ_INLINE_DATA) == 0u) {
            g_path.object.pc = WFS_OBJECT_PC_START;
            g_path.object.vol = &g_vol;
            g_path.object.object_id = f->object_id;
            g_path.object.err = WASMOS_ERR_NONE;
            status = wfs_run(wfs_object_task, &g_path.object);
            if (status != 0) {
                (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, 0);
                return;
            }
            obj = g_path.object.out;
        }

        wfs_read_init(&g_read, &g_vol, &obj, f->inline_data, f->offset + done, g_stage, chunk);
        status = wfs_run(wfs_read_task, &g_read);
        if (status != 0) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, 0);
            return;
        }
        if (g_read.done == 0u) {
            break; /* end of file */
        }
        if (wasmos_xfer_buffer_write(buffer_id, g_stage, (int32_t)g_read.done, (int32_t)done) !=
            0) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_BUFFER, 0);
            return;
        }
        done += g_read.done;
        if (g_read.done < chunk) {
            break; /* short read: the object ended */
        }
    }

    f->offset += done;
    (void)wfs_reply(src, FS_IPC_RESP, request_id, (int32_t)done, 0);
}

static void wfs_do_seek(int32_t src, int32_t request_id, int32_t fd, int32_t delta,
                        int32_t whence) {
    wfs_open_file_t* f = wfs_fd_get(&g_fds, src, fd);
    int64_t result;

    if (!f) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_BAD_FD, 0);
        return;
    }
    result = wfs_fd_seek(f, (int64_t)delta, whence);
    if (result < 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, (int32_t)result, 0);
        return;
    }
    if (result > 0x7FFFFFFF) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_RANGE, 0);
        return;
    }
    (void)wfs_reply(src, FS_IPC_RESP, request_id, (int32_t)result, 0);
}

/* Stream `len` bytes to a READDIR client, four per FS_IPC_STREAM frame — the
 * shape the client reassembles (libc_fs_request_stream) and fs-manager's own
 * root listing already emit.
 *
 * A full receiver queue is backpressure, not failure: the client is blocked on
 * exactly these frames and has no timeout, so a dropped frame strands it. Retry,
 * yielding between tries. Returns 0, or a packed code once the retries are
 * exhausted, which the caller reports instead of a truncated listing that would
 * read as a complete one. */
static wasmos_error_code_t wfs_stream(int32_t dest, int32_t request_id, const uint8_t* data,
                                      uint32_t len) {
    uint32_t pos = 0;

    while (pos < len) {
        int32_t a[4] = {0, 0, 0, 0};
        uint32_t i;
        uint32_t tries = 0;

        for (i = 0; i < 4u && pos < len; ++i) {
            a[i] = (int32_t)data[pos++];
        }
        for (;;) {
            int32_t rc = wasmos_ipc_send(
                dest, g_fs_endpoint, FS_IPC_STREAM, request_id, a[0], a[1], a[2], a[3]);

            if (rc == 0) {
                break;
            }
            if (rc != WASMOS_IPC_ERR_FULL || ++tries >= WFS_SEND_RETRIES) {
                return WASMOS_ERR_FS_REPLY_SEND;
            }
            (void)wasmos_sched_yield();
        }
    }
    return WASMOS_ERR_NONE;
}

/* READDIR: stream the entries of the client's current directory.
 *
 * Directories carry a trailing '/', matching fs-manager's root listing, which is
 * what the CLI renders. "." and ".." are NOT listed: the root listing shows no
 * dot entries either, and `cd ..` resolves through the records whether or not
 * they appear here. */
static void wfs_do_readdir(int32_t src, int32_t request_id) {
    uint8_t line[WFS_NAME_MAX + 2u];
    wasmos_error_code_t rc;
    int32_t status;
    uint32_t entries = 0;

    g_path.object.pc = WFS_OBJECT_PC_START;
    g_path.object.vol = &g_vol;
    g_path.object.object_id = wfs_cwd_get(src);
    g_path.object.err = WASMOS_ERR_NONE;
    status = wfs_run(wfs_object_task, &g_path.object);
    if (status != 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, 0);
        return;
    }
    if (g_path.object.out.type != WFS_TYPE_DIR) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_NOT_DIR, 0);
        return;
    }

    wfs_dir_lookup_init(&g_dir, &g_vol, &g_path.object.out, 0, 0u);
    for (;;) {
        uint32_t n = 0;
        uint32_t i;

        status = wfs_run(wfs_dir_task, &g_dir);
        if (status != 0) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, 0);
            return;
        }
        if (!g_dir.found) {
            break;
        }
        /* Skip the two records every directory carries. */
        if (!(g_dir.name_length == 1u && g_dir.name[0] == '.') &&
            !(g_dir.name_length == 2u && g_dir.name[0] == '.' && g_dir.name[1] == '.')) {
            for (i = 0; i < g_dir.name_length; ++i) {
                line[n++] = (uint8_t)g_dir.name[i];
            }
            if (g_dir.type == WFS_TYPE_DIR) {
                line[n++] = (uint8_t)'/';
            }
            line[n++] = (uint8_t)'\n';
            rc = wfs_stream(src, request_id, line, n);
            if (rc != WASMOS_ERR_NONE) {
                (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
                return;
            }
            entries++;
        }
        /* Resume in the block already staged: streaming sends no block requests,
         * so what the scan left staged is still there. */
        g_dir.pc = WFS_DIR_PC_SCAN;
        g_dir.found = 0u;
        if (entries > 4096u) {
            /* A directory that never ends is a corrupt one; refuse rather than
             * stream forever at a client that cannot stop us. */
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_CORRUPT, 0);
            return;
        }
    }
    (void)wfs_reply(src, FS_IPC_RESP, request_id, 0, 0);
}

/* CHDIR carries its target packed into arg0..arg3, up to 16 bytes (the FS ABI;
 * fs-manager packs "/" when a client enters this mount).
 *
 * FIXME: that packing caps a component at 15 bytes plus a NUL, while WFS names
 * run to 255 (WFS_NAME_MAX). `cd` into a directory with a longer name cannot be
 * expressed at all — the request arrives truncated, so the lookup misses and the
 * client is told the directory does not exist rather than that its name did not
 * fit. The limit is the opcode's, not this driver's, and every backend shares
 * it; fixing it means carrying the name in a transfer buffer the way OPEN and
 * STAT already do. Tracked in docs/TASKS.md.
 *
 * A single component is resolved through the records the directory carries, so
 * "." and ".." need no special case — ".." from the root names the root, which
 * is what stops a client walking out of the volume. */
static void wfs_do_chdir(int32_t src, int32_t request_id, int32_t a0, int32_t a1, int32_t a2,
                         int32_t a3) {
    char name[32];
    wasmos_error_code_t rc;
    uint32_t here;
    uint32_t name_len;
    int32_t status;

    wasmos_sys_ipc_unpack_name16(
        (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, name, sizeof(name));

    /* Entering the mount, or returning to its root. */
    if (name[0] == '\0' || (name[0] == '/' && name[1] == '\0')) {
        rc = wfs_cwd_set(src, WFS_OBJECT_ROOT);
        (void)wfs_reply(src,
                        rc == WASMOS_ERR_NONE ? FS_IPC_RESP : FS_IPC_ERROR,
                        request_id,
                        rc == WASMOS_ERR_NONE ? 0 : rc,
                        0);
        return;
    }

    here = wfs_cwd_get(src);

    /* Read where the client stands, then look the component up in it. */
    g_path.object.pc = WFS_OBJECT_PC_START;
    g_path.object.vol = &g_vol;
    g_path.object.object_id = here;
    g_path.object.err = WASMOS_ERR_NONE;
    status = wfs_run(wfs_object_task, &g_path.object);
    if (status != 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, 0);
        return;
    }
    if (g_path.object.out.type != WFS_TYPE_DIR) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_NOT_DIR, 0);
        return;
    }

    name_len = 0;
    while (name[name_len] != '\0') {
        name_len++;
    }
    wfs_dir_lookup_init(&g_dir, &g_vol, &g_path.object.out, name, name_len);
    status = wfs_run(wfs_dir_task, &g_dir);
    if (status != 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, 0);
        return;
    }
    if (!g_dir.found) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_NOT_FOUND, 0);
        return;
    }
    /* The record's type is enough: a cd into a file must fail as NOT_DIR rather
     * than succeed and leave the client standing on something unreadable. */
    if (g_dir.type != WFS_TYPE_DIR) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_NOT_DIR, 0);
        return;
    }
    rc = wfs_cwd_set(src, g_dir.object_id);
    (void)wfs_reply(src,
                    rc == WASMOS_ERR_NONE ? FS_IPC_RESP : FS_IPC_ERROR,
                    request_id,
                    rc == WASMOS_ERR_NONE ? 0 : rc,
                    0);
}

static void wfs_do_close(int32_t src, int32_t request_id, int32_t fd) {
    wasmos_error_code_t rc = wfs_fd_close(&g_fds, src, fd);

    if (rc != WASMOS_ERR_NONE) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
        return;
    }
    (void)wfs_reply(src, FS_IPC_RESP, request_id, 0, 0);
}

/* ---- fs-manager identity ------------------------------------------------ */

static void wfs_report_backend_info(int32_t dst, int32_t request_id) {
    int32_t mount_arg = 0;

    if (g_mount_bid >= 0 && g_mount_len > 0 &&
        wasmos_xfer_buffer_borrow(dst, g_mount_bid, WASMOS_BUFFER_GRANT_READ) >= 0) {
        mount_arg = (int32_t)(((uint32_t)g_mount_bid << 12) | ((uint32_t)g_mount_len & 0xFFFu));
    }
    (void)wasmos_sys_ipc_send_retry(dst,
                                    g_fs_endpoint,
                                    FSMGR_IPC_BACKEND_INFO_RESP,
                                    request_id,
                                    FSMGR_BACKEND_BOOT,
                                    0,
                                    mount_arg,
                                    (int32_t)g_mount_unit,
                                    WFS_SEND_RETRIES);
}

/* Which block unit the device-manager rule asked for. */
static int32_t wfs_parse_requested_unit(void) {
    char args[64];
    uint32_t i;

    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    for (i = 0; args[i] != '\0'; ++i) {
        if (args[i] == 'u' && args[i + 1] == 'n' && args[i + 2] == 'i' && args[i + 3] == 't' &&
            args[i + 4] == '=') {
            const char* p = &args[i + 5];
            int32_t v = 0;

            if (*p < '0' || *p > '9') {
                return -1;
            }
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                if (v > 255) {
                    return -1;
                }
                p++;
            }
            return v;
        }
    }
    return -1;
}

/* Resolve the mount alias and unit the same way fs_fat does: identify the block
 * unit, then ask the device manager what the rule bound it to. */
static int wfs_resolve_mount_alias(char* out_mount, uint32_t out_len, uint8_t* out_unit) {
    int32_t devmgr;
    int32_t req_id = 71;
    int32_t unit;
    uint32_t packed[4];
    uint32_t i;

    if (!out_mount || out_len < 2u || !out_unit) {
        return -1;
    }
    out_mount[0] = '\0';
    *out_unit = 0;

    if (wasmos_ipc_send(g_blk.block_endpoint,
                        g_reply_endpoint,
                        BLOCK_IPC_IDENTIFY_REQ,
                        req_id,
                        g_requested_unit,
                        0,
                        0,
                        0) != 0 ||
        wasmos_ipc_select_one(g_reply_endpoint) < 0) {
        return -1;
    }
    if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != BLOCK_IPC_IDENTIFY_RESP ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req_id) {
        return -1;
    }
    unit = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    *out_unit = (uint8_t)(unit & 0xFF);

    devmgr = wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "devmgr.query", 1);
    if (devmgr < 0) {
        return -1;
    }
    req_id++;
    if (wasmos_ipc_send(
            devmgr, g_reply_endpoint, DEVMGR_QUERY_BLOCK_MOUNT_REQ, req_id, unit, 0, 0, 0) != 0 ||
        wasmos_ipc_select_one(g_reply_endpoint) < 0) {
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
    for (i = 0; i < 16u && i + 1u < out_len; ++i) {
        char c = (char)((packed[i / 4u] >> ((i % 4u) * 8u)) & 0xFFu);

        out_mount[i] = c;
        if (c == '\0') {
            break;
        }
    }
    out_mount[out_len - 1u] = '\0';
    return out_mount[0] == '\0' ? -1 : 0;
}

/* ---- dispatch ----------------------------------------------------------- */

static void wfs_dispatch(int32_t type, int32_t src, int32_t request_id, int32_t a0, int32_t a1,
                         int32_t a2, int32_t a3) {
    switch (type) {
    case FSMGR_IPC_BACKEND_INFO_REQ:
        wfs_report_backend_info(src, request_id);
        return;
    case FS_IPC_READY_REQ:
        (void)wfs_reply(src,
                        g_vol.mounted ? FS_IPC_RESP : FS_IPC_ERROR,
                        request_id,
                        g_vol.mounted ? 0 : WASMOS_ERR_FS_NOT_READY,
                        0);
        return;
    case FS_IPC_OPEN_REQ:
        wfs_do_open(src, request_id, a0, a1, a2);
        return;
    case FS_IPC_STAT_REQ:
        wfs_do_stat(src, request_id, a0, a2);
        return;
    case FS_IPC_READ_REQ:
        wfs_do_read(src, request_id, a0, a1, a2);
        return;
    case FS_IPC_SEEK_REQ:
        wfs_do_seek(src, request_id, a0, a1, a2);
        return;
    case FS_IPC_CLOSE_REQ:
        wfs_do_close(src, request_id, a0);
        return;
    case FS_IPC_CHDIR_REQ:
        wfs_do_chdir(src, request_id, a0, a1, a2, a3);
        return;
    case FS_IPC_READDIR_REQ:
        wfs_do_readdir(src, request_id);
        return;
    default:
        /* The write ops (WRITE/UNLINK/MKDIR/RMDIR/RENAME) wait on the write
         * path. */
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_UNSUPPORTED, 0);
        return;
    }
}

WASMOS_WASM_EXPORT int32_t initialize(int32_t a, int32_t b, int32_t c, int32_t d) {
    char mount_alias[32];
    char service_name[32];
    int32_t block_endpoint = -1;
    int32_t mount_len;
    int32_t name_len;
    int32_t status;
    static const char* const k_service_prefix = "fs.wfs";

    (void)a;
    (void)b;
    (void)c;
    (void)d;

    g_proc_endpoint = wasmos_startup_proc_endpoint();
    g_fs_endpoint = wasmos_ipc_create_endpoint();
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_fs_endpoint < 0 || g_reply_endpoint < 0) {
        wfs_log("[fs-wfs] endpoint create failed\n");
        wfs_stall();
    }

    g_requested_unit = wfs_parse_requested_unit();
    for (;;) {
        block_endpoint = wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "block", 1);
        if (block_endpoint > 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }

    wasmos_wasm_runtime_init(&g_runtime);
    wasmos_sys_event_loop_init(&g_loop, g_reply_endpoint, 0x7000);
    wfs_block_configure(
        &g_blk, &g_loop, block_endpoint, g_reply_endpoint, wasmos_block_buffer_phys());
    if (g_blk.buf_id < 0) {
        wfs_log("[fs-wfs] block buffer missing\n");
        wfs_stall();
    }
    wfs_fd_table_init(&g_fds);
    for (name_len = 0; name_len < (int32_t)WFS_MAX_CLIENTS; ++name_len) {
        g_cwd[name_len].owner = -1;
        g_cwd[name_len].cwd = WFS_OBJECT_ROOT;
    }
    wfs_ops_bind(&g_runtime, &g_blk);

    /* Mount before advertising anything, so the driver only ever offers a
     * validated, parsed volume. */
    g_mount_ctx.pc = WFS_MOUNT_PC_START;
    g_mount_ctx.vol = &g_vol;
    g_mount_ctx.err = WASMOS_ERR_NONE;
    g_mount_ctx.next_group = 0u;
    g_mount_ctx.group_started = 0u;
    status = wfs_run(wfs_mount_task, &g_mount_ctx);
    if (status != 0 || !g_vol.mounted) {
        wfs_log("[fs-wfs] mount failed\n");
        wfs_stall();
    }
    wfs_log("[fs-wfs] mounted\n");

    if (wfs_resolve_mount_alias(mount_alias, sizeof(mount_alias), &g_mount_unit) != 0) {
        wfs_log("[fs-wfs] mount alias resolve failed\n");
        wfs_stall();
    }
    mount_len = 0;
    while (mount_alias[mount_len] != '\0') {
        mount_len++;
    }
    if (mount_len <= 0 || mount_len >= 0xFFF || mount_len >= wasmos_xfer_buffer_size()) {
        wfs_log("[fs-wfs] mount alias size invalid\n");
        wfs_stall();
    }
    g_mount_bid = wasmos_xfer_buffer_acquire(mount_len);
    if (g_mount_bid < 0 || wasmos_xfer_buffer_write(g_mount_bid, mount_alias, mount_len, 0) != 0) {
        wfs_log("[fs-wfs] mount alias buffer write failed\n");
        wfs_stall();
    }
    g_mount_len = mount_len;

    /* "fs.wfs<unit>" so two WFS volumes can be mounted at once. */
    name_len = 0;
    while (k_service_prefix[name_len] != '\0') {
        service_name[name_len] = k_service_prefix[name_len];
        name_len++;
    }
    if (g_mount_unit >= 10u) {
        service_name[name_len++] = (char)('0' + (g_mount_unit / 10u));
    }
    service_name[name_len++] = (char)('0' + (g_mount_unit % 10u));
    service_name[name_len] = '\0';
    if (wasmos_svc_register_class(g_proc_endpoint,
                                  g_fs_endpoint,
                                  service_name,
                                  FSMGR_BACKEND_CLASS,
                                  FSMGR_BACKEND_INSTANCE(FSMGR_BACKEND_BOOT, g_mount_unit),
                                  1) != 0) {
        wfs_log("[fs-wfs] fs.backend register failed\n");
        wfs_stall();
    }
    wfs_log("[fs-wfs] fs.backend registered\n");

    /* Answer fs-manager's first info pull before signalling ready, so the mount
     * is routable the moment anything is told the driver exists. */
    for (;;) {
        if (wasmos_ipc_select_one(g_fs_endpoint) < 0) {
            continue;
        }
        if (wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) == FSMGR_IPC_BACKEND_INFO_REQ) {
            wfs_report_backend_info(wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
                                    wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID));
            break;
        }
    }
    wasmos_sys_notify_ready(g_proc_endpoint, g_reply_endpoint);
    wfs_log("[fs-wfs] ready\n");

    /* Serve. Blocks on the endpoint at idle rather than polling it. */
    for (;;) {
        if (wasmos_ipc_select_one(g_fs_endpoint) < 0) {
            continue;
        }
        wfs_dispatch(wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE),
                     wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
                     wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID),
                     wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0),
                     wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1),
                     wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2),
                     wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3));
    }
}
