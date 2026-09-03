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
 *
 * ---- the files ------------------------------------------------------------
 *
 * Three layers, and the boundary between the first two is load-bearing rather
 * than decorative: the FORMAT layer touches no device and runs no task, which is
 * why its host suites link two files while every other suite links fifteen. A
 * change that gives a format file a block request has crossed a line.
 *
 * Each .c below has a paired header carrying its contract; only the header-only
 * files are named as such.
 *
 *   FORMAT — pure; on-disk structures and the arithmetic over them
 *     wfs_format.h    every on-disk struct, with its layout as _Static_assert
 *     wfs_status.h    the driver's view of the generated error codes
 *     wfs_types.h     the per-operation task contexts (declarations only)
 *     wfs_crc32c.c    CRC32C, and the uuid+location seeding (§13)
 *     wfs_super.c     superblock parse/validate, and where backups sit (§4, §5)
 *     wfs_bitmap.c    allocation bitmaps and the run search (§12)
 *     wfs_dirent.c    directory record insert/remove inside one block (§10)
 *     wfs_fd.c        the per-client open-file table
 *
 *   PLUMBING — the device and the runtime
 *     wfs_ops.c       the shared block client, runtime and event loop
 *     wfs_block.c     one block staged at a time, over BLOCK_IPC futures
 *
 *   OPERATIONS — tasks on the coroutine runtime, one file per operation
 *     wfs_mount.c     mount, group descriptors, object records
 *     wfs_extent.c    logical block -> physical block, inline and tree (§9)
 *     wfs_dir.c       directory scan and lookup (§10)
 *     wfs_path.c      path -> object, resolved component by component
 *     wfs_read.c      bytes out of an object (§16)
 *     wfs_write.c     bytes into an object; allocates where nothing is mapped
 *     wfs_truncate.c  set a size; frees or sparsens what changes
 *     wfs_alloc.c     take and release blocks and object records (§12)
 *     wfs_namespace.c create/mkdir/unlink/rmdir/rename over the two (§10)
 *     wfs_sync.c      record that the volume is mounted for writing (§4)
 */
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"
#include <fcntl.h>

#include "wfs_block.h"
#include "wfs_dir.h"
#include "wfs_fd.h"
#include "wfs_mount.h"
#include "wfs_ops.h"
#include "wfs_path.h"
#include "wfs_namespace.h"
#include "wfs_read.h"
#include "wfs_sync.h"
#include "wfs_truncate.h"
#include "wfs_write.h"

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
/* This driver's mount name, reported on an fs-manager pull. Held as plain bytes:
 * fs-manager is the CLIENT of that pull and supplies the buffer, so this driver
 * owns no transfer buffer for it. */
static char g_mount_name[32];
static int32_t g_mount_len = 0;
static uint8_t g_mount_unit = 0;
static int32_t g_requested_unit = -1;
/* The `block` class instance of this driver's disk, from `id=`. Both the lookup
 * key and the `target` every transfer carries. */
static uint32_t g_requested_instance = 0u;
/* Which backend serves the disk this instance was spawned for, as the device
 * manager named it. A unit alone does not identify a disk -- it is
 * backend-local -- so both halves are needed to find the right server. */
static uint8_t g_requested_backend = (uint8_t)BLOCK_BACKEND_UNKNOWN;
/* The `volume` class instance of the volume mounted here, DERIVED from the same
 * `id=` (see wfs_parse_volume_instance). 0 when no volume covers this device,
 * which is not an error: the claim is advisory. */
static uint32_t g_volume_instance = 0u;

/* Op contexts, static because a task's context must outlive its awaits and the
 * driver runs one op at a time. */
/* Per-client working directory. Scoped to the source endpoint for the same
 * reason an fd is: one client's cd must not move another's. A client with no
 * entry stands at the root, so the table only ever holds clients that moved. */

static wfs_path_ctx_t g_path;
static wfs_read_ctx_t g_read;
static wfs_write_ctx_t g_write;
static wfs_trunc_ctx_t g_trunc;
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

/* Run one op to completion. The pump lives in wfs_ops so the namespace ops can
 * use it too; this name is kept because every call site below reads better with
 * it. */
static int32_t wfs_run(wasmos_wasm_task_resume_fn fn, void* ctx) {
    return wfs_ops_run(fn, ctx);
}

/* Copy a client's path out of the transfer buffer it borrowed to us. */
static wasmos_error_code_t wfs_take_path(int32_t buffer_id, uint32_t path_len, char* out,
                                         uint32_t out_len) {
    /* An EMPTY name is not an over-long one: it is a malformed request, and a
     * caller told "too long" would shorten a name it never sent. */
    if (path_len == 0u) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (path_len >= out_len) {
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
    rc = wfs_path_init_from(&g_path, &g_vol, WFS_OBJECT_ROOT, path, path_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    status = wfs_run(wfs_path_task, &g_path);
    if (status != 0) {
        return (wasmos_error_code_t)status;
    }
    return g_path.found ? WASMOS_ERR_NONE : WASMOS_ERR_FS_NOT_FOUND;
}

/* Re-read an object record into g_path.object, after something rewrote it. */
static wasmos_error_code_t wfs_reload_object(uint32_t object_id) {
    int32_t status;

    g_path.object.pc = WFS_OBJECT_PC_START;
    g_path.object.vol = &g_vol;
    g_path.object.object_id = object_id;
    g_path.object.err = WASMOS_ERR_NONE;
    status = wfs_run(wfs_object_task, &g_path.object);
    return status != 0 ? (wasmos_error_code_t)status : WASMOS_ERR_NONE;
}

/* ---- the operations ----------------------------------------------------- */

static void wfs_do_open(int32_t src, int32_t request_id, int32_t path_len, int32_t flags,
                        int32_t buffer_id) {
    wasmos_error_code_t rc;
    int32_t fd;

    /* O_CREAT creates the file first, then falls through to the normal open. An
     * EXISTS from the create means the file is already there, which O_CREAT
     * without O_EXCL asks us to open rather than refuse. */
    if (((uint32_t)flags & (uint32_t)O_CREAT) != 0u) {
        static char create_path[WFS_PATH_MAX];

        rc = wfs_take_path(buffer_id, (uint32_t)path_len, create_path, sizeof(create_path));
        if (rc != WASMOS_ERR_NONE) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
            return;
        }
        rc = wfs_ns_create(&g_vol,
                           WFS_OBJECT_ROOT,
                           create_path,
                           (uint32_t)path_len,
                           (uint16_t)WFS_TYPE_FILE,
                           0644u,
                           0u,
                           0);
        if (rc != WASMOS_ERR_NONE && rc != WASMOS_ERR_FS_EXISTS) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
            return;
        }
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
    /* O_TRUNC applies BEFORE the fd is handed out, so the size the fd latches is
     * the truncated one; the other order would leave a client writing against a
     * size the file no longer has. */
    if (((uint32_t)flags & (uint32_t)O_TRUNC) != 0u && g_path.object.out.size != 0u) {
        int32_t status;

        wfs_truncate_init(&g_trunc,
                          &g_vol,
                          g_path.object_id,
                          &g_path.object.out,
                          g_path.object.inline_data,
                          0u,
                          0u);
        status = (int32_t)wfs_truncate_run(&g_trunc);
        if (status != 0) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, 0);
            return;
        }
        /* Re-read: truncation rewrote the record, and the fd must latch what is
         * on disk rather than what was there before. */
        rc = wfs_reload_object(g_path.object_id);
        if (rc != WASMOS_ERR_NONE) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
            return;
        }
    }
    fd = wfs_fd_open(&g_fds,
                     src,
                     g_path.object_id,
                     g_path.object.out.size,
                     g_path.object.out.type,
                     g_path.object.out.flags,
                     (uint16_t)flags,
                     g_path.object.inline_data);
    if (fd < 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, fd, 0);
        return;
    }
    /* O_APPEND starts the cursor at the end, which is the whole of what the flag
     * means here: every write then extends from wherever the file ends. */
    if (((uint32_t)flags & (uint32_t)O_APPEND) != 0u) {
        wfs_open_file_t* f = wfs_fd_get(&g_fds, src, fd);

        if (f) {
            f->offset = f->size;
        }
    }
    (void)wfs_reply(src, FS_IPC_RESP, request_id, fd, 0);
}

/* The POSIX-shaped mode for `obj`: its permission bits with the FILE TYPE bits
 * merged in.
 *
 * WFS records the type in its own field and keeps `mode` to permissions alone,
 * but the FS ABI reports one mode, and every consumer reads the type out of it --
 * `S_ISDIR` in libc, and fs-manager when it validates a chdir target. Reporting
 * the on-disk mode unchanged therefore described every directory on a WFS volume
 * as a regular file.
 *
 * A type with no POSIX equivalent contributes no type bits rather than a wrong
 * one: an unknown type is better read as "not a directory, not a regular file"
 * than as either. */
static uint32_t wfs_stat_mode(const struct wfs_object* obj) {
    uint32_t mode = obj->mode & 0x0FFFu;

    switch (obj->type) {
    case WFS_TYPE_DIR:
        return mode | 0x4000u; /* S_IFDIR */
    case WFS_TYPE_FILE:
        return mode | 0x8000u; /* S_IFREG */
    case WFS_TYPE_SYMLINK:
        return mode | 0xA000u; /* S_IFLNK */
    default:
        return mode;
    }
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
    (void)wfs_reply(
        src, FS_IPC_RESP, request_id, reported, (int32_t)(wfs_stat_mode(&g_path.object.out)));
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

/* WRITE: bytes from the client's buffer into the file the fd names.
 *
 * Mirrors wfs_do_read, including its per-chunk re-fetch of the object record:
 * the fd does not carry the extent map, and the write UPDATES that map, so each
 * chunk must start from what is on disk rather than from a stale copy. Skipping
 * the re-fetch would let a second chunk allocate against the first chunk's
 * pre-write map and lose its extent. */
static void wfs_do_write(int32_t src, int32_t request_id, int32_t fd, int32_t count,
                         int32_t buffer_id) {
    wfs_open_file_t* f = wfs_fd_get(&g_fds, src, fd);
    uint32_t want;
    uint32_t done = 0u;

    if (!f) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_BAD_FD, 0);
        return;
    }
    /* An fd-mode violation, distinct from a read-only VOLUME: this fd was opened
     * for reading, whatever the volume permits. */
    if (!wfs_fd_writable(f)) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_ACCESS, 0);
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

    while (done < want) {
        uint32_t chunk = want - done;
        wasmos_error_code_t rc;
        int32_t status;

        if (chunk > WFS_READ_STAGE) {
            chunk = WFS_READ_STAGE;
        }
        if (wasmos_sys_buffer_read(buffer_id, g_stage, (int32_t)chunk, (int32_t)done) != 0) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_BUFFER, 0);
            return;
        }
        rc = wfs_reload_object(f->object_id);
        if (rc != WASMOS_ERR_NONE) {
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
            return;
        }
        wfs_write_init(&g_write,
                       &g_vol,
                       f->object_id,
                       &g_path.object.out,
                       g_path.object.inline_data,
                       f->offset + done,
                       g_stage,
                       chunk,
                       0u);
        status = (int32_t)wfs_write_run(&g_write);
        if (status != 0) {
            /* Report what landed alongside the failure: a partial write is not a
             * failed write, and a client that resends from zero would duplicate
             * the bytes already on disk. */
            (void)wfs_reply(src, FS_IPC_ERROR, request_id, status, (int32_t)done);
            return;
        }
        done += g_write.done;
        if (g_write.done < chunk) {
            break;
        }
    }

    f->offset += done;
    /* The fd's cached size follows, so a later read through the same fd is not
     * clamped against the size the file had at open. */
    if (f->offset > f->size) {
        f->size = f->offset;
    }
    (void)wfs_reply(src, FS_IPC_RESP, request_id, (int32_t)done, 0);
}

/* MKDIR / UNLINK / RMDIR: one path in the client's buffer (arg0 = length,
 * arg2 = buffer id), resolved against where this client stands. */
static void wfs_do_namespace(int32_t src, int32_t request_id, int32_t type, int32_t path_len,
                             int32_t buffer_id) {
    static char path[WFS_PATH_MAX];
    wasmos_error_code_t rc = wfs_take_path(buffer_id, (uint32_t)path_len, path, sizeof(path));
    uint32_t id = 0u;

    if (rc != WASMOS_ERR_NONE) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
        return;
    }
    switch (type) {
    case FS_IPC_MKDIR_REQ:
        rc = wfs_ns_create(&g_vol,
                           WFS_OBJECT_ROOT,
                           path,
                           (uint32_t)path_len,
                           (uint16_t)WFS_TYPE_DIR,
                           0755u,
                           0u,
                           &id);
        break;
    case FS_IPC_UNLINK_REQ:
        rc = wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, path, (uint32_t)path_len, 0u);
        break;
    case FS_IPC_RMDIR_REQ:
        rc = wfs_ns_rmdir(&g_vol, WFS_OBJECT_ROOT, path, (uint32_t)path_len, 0u);
        break;
    default:
        rc = WASMOS_ERR_FS_UNSUPPORTED;
        break;
    }
    (void)wfs_reply(src,
                    rc == WASMOS_ERR_NONE ? FS_IPC_RESP : FS_IPC_ERROR,
                    request_id,
                    rc == WASMOS_ERR_NONE ? 0 : rc,
                    0);
}

/* RENAME carries BOTH paths in one buffer: the source at offset 0 and the
 * destination at arg0 + 1, which is the convention fs-manager's routing already
 * rewrites them under. */
static void wfs_do_rename(int32_t src, int32_t request_id, int32_t from_len, int32_t to_len,
                          int32_t buffer_id) {
    static char from[WFS_PATH_MAX];
    static char to[WFS_PATH_MAX];
    wasmos_error_code_t rc;

    if (from_len <= 0 || to_len <= 0 || (uint32_t)from_len >= sizeof(from) ||
        (uint32_t)to_len >= sizeof(to)) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_PATH_TOO_LONG, 0);
        return;
    }
    if (wasmos_sys_buffer_read(buffer_id, from, from_len, 0) != 0 ||
        wasmos_sys_buffer_read(buffer_id, to, to_len, from_len + 1) != 0) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_BUFFER, 0);
        return;
    }
    from[from_len] = '\0';
    to[to_len] = '\0';
    rc = wfs_ns_rename(&g_vol, WFS_OBJECT_ROOT, from, (uint32_t)from_len, to, (uint32_t)to_len, 0u);
    (void)wfs_reply(src,
                    rc == WASMOS_ERR_NONE ? FS_IPC_RESP : FS_IPC_ERROR,
                    request_id,
                    rc == WASMOS_ERR_NONE ? 0 : rc,
                    0);
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

/* READDIR: stream the entries of the directory the request names.
 *
 * Directories carry a trailing '/', matching fs-manager's root listing, which is
 * what the CLI renders. "." and ".." are NOT listed: the root listing shows no
 * dot entries either, and `cd ..` resolves through the records whether or not
 * they appear here. */
static void wfs_do_readdir(int32_t src, int32_t request_id, int32_t path_len, int32_t buffer_id) {
    uint8_t line[WFS_NAME_MAX + 2u];
    wasmos_error_code_t rc;
    int32_t status;
    uint32_t entries = 0;

    /* The directory is the one the request NAMES. This driver holds no working
     * directory: fs-manager owns the cwd and resolves it to a mount-relative
     * path before forwarding, so a listing depends on the request rather than on
     * what the previous one left behind. */
    rc = wfs_resolve(src, buffer_id, (uint32_t)path_len);
    if (rc != WASMOS_ERR_NONE) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
        return;
    }

    g_path.object.pc = WFS_OBJECT_PC_START;
    g_path.object.vol = &g_vol;
    g_path.object.object_id = g_path.object_id;
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

static void wfs_do_close(int32_t src, int32_t request_id, int32_t fd) {
    wasmos_error_code_t rc = wfs_fd_close(&g_fds, src, fd);

    if (rc != WASMOS_ERR_NONE) {
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, rc, 0);
        return;
    }
    (void)wfs_reply(src, FS_IPC_RESP, request_id, 0, 0);
}

/* ---- fs-manager identity ------------------------------------------------ */

static void wfs_report_backend_info(int32_t dst, int32_t request_id, int32_t buffer_id) {
    int32_t name_len = 0;

    if (buffer_id > 0 && g_mount_len > 0 &&
        wasmos_xfer_buffer_write(buffer_id, g_mount_name, g_mount_len, 0) == 0) {
        name_len = g_mount_len;
    }
    (void)wasmos_sys_ipc_send_retry(dst,
                                    g_fs_endpoint,
                                    FSMGR_IPC_BACKEND_INFO_RESP,
                                    request_id,
                                    FSMGR_BACKEND_BLOCK,
                                    FS_TYPE_WFS,
                                    name_len,
                                    (int32_t)g_mount_unit,
                                    WFS_SEND_RETRIES);
}

/* Map the device manager's DRIVER name to the backend it stands for. The names
 * are the drivers' manifest package names -- the same spelling a rule's DRIVER==
 * uses and the rule parser maps -- so this list tracks fs_fat's deliberately:
 * both filesystems resolve the same identity from the same argument. */
static uint8_t wfs_backend_from_name(const char* name) {
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

/* The value of `name` in the space-separated startup blob, or NULL when absent.
 * The returned pointer is INTO the blob and is not NUL-terminated: a caller
 * copies up to the next space.
 *
 * Anchored at a token boundary. An unanchored search finds `id=` inside
 * `partuuid=`, which ends in those same three characters -- and the value it
 * would return is the tail of a GUID. */
static const char* wfs_find_token_value(const char* args, const char* name) {
    uint32_t i;

    for (i = 0; args[i] != '\0'; ++i) {
        uint32_t k = 0;

        if (i != 0u && args[i - 1u] != ' ') {
            continue;
        }
        while (name[k] != '\0' && args[i + k] == name[k]) {
            ++k;
        }
        if (name[k] == '\0') {
            return &args[i + k];
        }
    }
    return 0;
}

static uint8_t wfs_parse_requested_backend(void) {
    char args[64];

    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return (uint8_t)BLOCK_BACKEND_UNKNOWN;
    }
    return wfs_backend_from_name(wfs_find_token_value(args, "driver="));
}

/* Resolve this instance's disk through the `block` service CLASS, matching the
 * instance that encodes its (backend, unit).
 *
 * By class rather than by the plain `block` NAME, because a name resolves to one
 * provider for the whole system: it could only ever reach whichever backend
 * registered the name, so a rule could not mount WFS on any other backend however
 * well that backend worked. Resolving by name is what made this driver silently
 * ATA-shaped -- it worked only while ATA was the sole block backend.
 *
 * Returns the provider's endpoint, or -1 while it has not registered yet; the
 * caller retries, since a filesystem may be spawned before its disk's driver has
 * finished coming up. */
static int32_t wfs_lookup_block_server(uint32_t instance, int32_t request_id) {
    svc_class_entry_t providers[8];
    int32_t n = wasmos_svc_lookup_class(g_proc_endpoint,
                                        g_reply_endpoint,
                                        "block",
                                        providers,
                                        (int32_t)(sizeof(providers) / sizeof(providers[0])),
                                        request_id);
    int32_t i;

    if (n < 0) {
        return -1;
    }
    if (n > (int32_t)(sizeof(providers) / sizeof(providers[0]))) {
        n = (int32_t)(sizeof(providers) / sizeof(providers[0]));
    }
    for (i = 0; i < n; ++i) {
        if (providers[i].instance == instance) {
            return (int32_t)providers[i].endpoint;
        }
    }
    return -1;
}

/* Startup-argument blob this driver reads. Sized for
 * `driver=<name> unit=<n> id=<canonical id> mount=<path>`, whose id alone may be
 * BLOCK_DESCRIPTOR_ID_MAX bytes. The process manager truncates at
 * WASMOS_STARTUP_ARGS_MAX, so nothing longer can arrive. */
#define WFS_STARTUP_ARGS_MAX 192

/* Copy one token's value out of the blob. Refuses a value that does not fit
 * rather than truncating it: a truncated path is a different, valid-looking
 * path, and a truncated id fingerprints to a device nobody registered. */
static int wfs_copy_token(const char* args, const char* name, char* out, uint32_t out_cap) {
    const char* token = wfs_find_token_value(args, name);
    uint32_t n = 0;

    if (!token || !out || out_cap == 0u) {
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

/* The `block` class instance of this driver's disk: the fingerprint of the
 * canonical id in `id=`.
 *
 * It is both how the provider is found in the class registry and how the
 * provider is told which of its disks a transfer means, since several instances
 * share one endpoint. NOT rebuilt from driver= and unit= here: a second place
 * that spells the id is a second place that can disagree with the publisher, and
 * a disagreement leaves this driver waiting forever on an instance nobody holds.
 * Returns 0, which no valid device has. */
static uint32_t wfs_parse_requested_instance(void) {
    char args[WFS_STARTUP_ARGS_MAX];
    char id[BLOCK_DESCRIPTOR_ID_MAX];

    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return 0u;
    }
    if (wfs_copy_token(args, "id=", id, sizeof(id)) != 0) {
        return 0u;
    }
    return wasmos_block_fingerprint(id);
}

/* The `volume` class instance of the volume this driver mounts.
 *
 * DERIVED here rather than asked for: a volume's canonical id is `volume:`
 * prefixed to its backing device's (architecture/37-volume-manager.md §3), and
 * its class instance is the fingerprint of that. Both sides build the same
 * string from the same `id=`, so nothing has to carry the value and there is no
 * second spelling to disagree with the publisher.
 *
 * Returns 0 -- which no volume has -- when `id=` is missing or the prefixed form
 * would not fit. A truncated id fingerprints to a value nobody registered, so
 * silently shortening it would claim some other volume, or none. */
static uint32_t wfs_parse_volume_instance(void) {
    char args[WFS_STARTUP_ARGS_MAX];
    char id[BLOCK_DESCRIPTOR_ID_MAX];
    char volume_id[VOLUME_DESCRIPTOR_ID_MAX];
    static const char prefix[] = "volume:";
    uint32_t n = 0;
    uint32_t i = 0;

    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return 0u;
    }
    if (wfs_copy_token(args, "id=", id, sizeof(id)) != 0) {
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
 * ADVISORY, and deliberately fire-and-forget. The volume manager is not in the
 * I/O path (architecture/37-volume-manager.md §5), so the claim changes nothing
 * about this mount -- it is what `fsck.wfs` consults before touching a volume a
 * filesystem may be writing. Blocking the mount on the round trip would make an
 * advisory record able to stall a boot; the reply is handled in wfs_dispatch
 * instead, where a refusal can be reported without anyone waiting for it.
 *
 * A volume that no volume manager published is simply not claimed. That is the
 * ordinary case for a driver spawned by a `SUBSYSTEM=="block"` rule, and it is
 * why nothing here treats a failed lookup as an error. */
static void wfs_volume_claim(int32_t claim) {
    svc_class_entry_t providers[8];
    int32_t n;
    int32_t i;

    if (g_volume_instance == 0u) {
        return;
    }
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
        if (providers[i].instance != g_volume_instance) {
            continue;
        }
        (void)wasmos_sys_ipc_send_retry((int32_t)providers[i].endpoint,
                                        g_fs_endpoint,
                                        VOLUME_IPC_CLAIM_REQ,
                                        1,
                                        (int32_t)g_volume_instance,
                                        claim,
                                        0,
                                        0,
                                        WFS_SEND_RETRIES);
        return;
    }
}

/* Which block unit the rule asked for. Still delivered, and still what names
 * this driver's service ("fs.wfs<unit>") and its fs.backend instance -- only the
 * BLOCK target moved to the canonical id. */
static int32_t wfs_parse_requested_unit(void) {
    char args[WFS_STARTUP_ARGS_MAX];
    char value[8];
    const char* p;
    int32_t v = 0;

    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    if (wfs_copy_token(args, "unit=", value, sizeof(value)) != 0) {
        return -1;
    }
    for (p = value; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        v = v * 10 + (*p - '0');
        if (v > 255) {
            return -1;
        }
    }
    return v;
}

/* The VFS path this volume mounts at, from `mount=`.
 *
 * The rule that spawned this driver chose it and the device manager delivers it
 * at spawn; nothing is asked back afterwards. The retired
 * DEVMGR_QUERY_BLOCK_MOUNT_REQ answered a query keyed on the disk unit, which
 * two volumes on one disk share, so it could not say which of them was meant. */
static int wfs_parse_requested_mount(char* out, uint32_t out_cap) {
    char args[WFS_STARTUP_ARGS_MAX];

    if (!out || out_cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (wasmos_startup_args(args, sizeof(args)) == 0u) {
        return -1;
    }
    return wfs_copy_token(args, "mount=", out, out_cap);
}

/* WASMOS_IPC_SHUTDOWN_REQ: the clean unmount.
 *
 * Reconciles the free counters the superblock carries and records
 * WFS_STATE_CLEAN, which is the only thing that tells the next mount its log
 * holds nothing to replay (§15). The volume is left mounted -- nothing revokes
 * the endpoint, and a request arriving after this is served -- because the
 * machine is going down either way and refusing would only turn a late read into
 * an error the caller cannot act on.
 *
 * DONE is sent whether or not the write succeeded. The sequence has no failure
 * reply and nothing to do with one: a volume whose clean mark did not land
 * mounts read-only next boot and replays, which is exactly what happens when a
 * volume misses the sequence entirely.
 *
 * A read-only volume has nothing to record, and wfs_sync_task refuses one, so it
 * answers without a write.
 */
static void wfs_do_shutdown(int32_t src, int32_t request_id) {
    static wfs_sync_ctx_t sync;

    /* Released before the clean mark is written, not after: once this returns
     * DONE the driver may be torn down at any moment, and a claim that outlives
     * its holder makes the volume permanently unrecheckable. Releasing early
     * costs nothing -- the machine is going down, so nothing is going to check
     * this volume in the window. */
    wfs_volume_claim(0);
    if (g_vol.mounted && !g_vol.super.read_only) {
        /* Static and zeroed rather than a stack local: it carries a coroutine
         * record, which must be zeroed before the task starts it, and the guest
         * stack this driver runs on has no room to spare for a context this
         * size. Static is safe because the sequence notifies a participant once. */
        memset(&sync, 0, sizeof(sync));
        sync.vol = &g_vol;
        sync.state = (uint32_t)WFS_STATE_CLEAN;
        if (wfs_run(wfs_sync_task, &sync) != 0) {
            wfs_log("[fs-wfs] clean unmount failed\n");
        } else {
            wfs_log("[fs-wfs] unmounted clean\n");
        }
    }
    (void)wfs_reply(src, WASMOS_IPC_SHUTDOWN_DONE, request_id, 0, 0);
}

/* ---- dispatch ----------------------------------------------------------- */

static void wfs_dispatch(int32_t type, int32_t src, int32_t request_id, int32_t a0, int32_t a1,
                         int32_t a2, int32_t a3) {
    switch (type) {
    case WASMOS_IPC_SHUTDOWN_REQ:
        wfs_do_shutdown(src, request_id);
        return;
    case FSMGR_IPC_BACKEND_INFO_REQ:
        wfs_report_backend_info(src, request_id, a0);
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
    case FS_IPC_WRITE_REQ:
        wfs_do_write(src, request_id, a0, a1, a2);
        return;
    case FS_IPC_SEEK_REQ:
        wfs_do_seek(src, request_id, a0, a1, a2);
        return;
    case FS_IPC_CLOSE_REQ:
        wfs_do_close(src, request_id, a0);
        return;
    case FS_IPC_READDIR_REQ:
        wfs_do_readdir(src, request_id, a0, a2);
        return;
    case FS_IPC_MKDIR_REQ:
    case FS_IPC_UNLINK_REQ:
    case FS_IPC_RMDIR_REQ:
        wfs_do_namespace(src, request_id, type, a0, a2);
        return;
    case FS_IPC_RENAME_REQ:
        wfs_do_rename(src, request_id, a0, a1, a2);
        return;
    case VOLUME_IPC_RESP:
        /* The volume manager acknowledging a claim this driver sent
         * fire-and-forget. Nothing waits on it; it is consumed here so the
         * default below does not answer a reply with an error. */
        return;
    case VOLUME_IPC_ERROR:
        /* A claim that did not take. The mount stands either way -- the record
         * is advisory -- but `fsck.wfs` will not know this volume is in use, so
         * the loss is worth a line rather than silence. */
        wfs_log("[fs-wfs] volume claim refused\n");
        return;
    default:
        /* WRITE-side opcodes this driver does not implement at all. */
        (void)wfs_reply(src, FS_IPC_ERROR, request_id, WASMOS_ERR_FS_UNSUPPORTED, 0);
        return;
    }
}

/* The entry the process manager calls, and it takes NO arguments.
 *
 * A path-spawned entry is called with argc 0 (process_manager_spawn.c,
 * wasmos_app.c), and wasm3 refuses argc 0 against a function that declares
 * parameters -- so a four-parameter entry here made this driver fail to start
 * at all under the default runtime, before its first line of output. WARP does
 * not enforce entry arity, which is the only reason it ever ran. fs_fat and
 * fs_init declare it the same way for the same reason.
 *
 * Startup values do not arrive as arguments: they come from the spawn-info
 * buffer, read below. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    char mount_alias[32];
    char service_name[32];
    int32_t block_endpoint = -1;
    int32_t mount_len;
    int32_t name_len;
    int32_t status;
    static const char* const k_service_prefix = "fs.wfs";

    g_proc_endpoint = wasmos_startup_proc_endpoint();
    g_fs_endpoint = wasmos_ipc_create_endpoint();
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_fs_endpoint < 0 || g_reply_endpoint < 0) {
        wfs_log("[fs-wfs] endpoint create failed\n");
        wfs_stall();
    }

    g_requested_unit = wfs_parse_requested_unit();
    g_requested_instance = wfs_parse_requested_instance();
    g_volume_instance = wfs_parse_volume_instance();
    if (g_requested_instance == 0u || g_requested_unit < 0) {
        /* The id names the disk and the unit names this driver's service. A
         * guessed disk mounts the wrong volume, so neither is inferred. */
        wfs_log("[fs-wfs] startup args missing id= or unit=; cannot resolve a block device\n");
        wfs_stall();
    }
    g_mount_unit = (uint8_t)g_requested_unit;
    for (int32_t attempt = 0;; ++attempt) {
        block_endpoint = wfs_lookup_block_server(g_requested_instance, 1 + attempt);
        if (block_endpoint > 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }

    wasmos_wasm_runtime_init(&g_runtime);
    wasmos_sys_event_loop_init(&g_loop, g_reply_endpoint, 0x7000);
    if (wfs_block_configure(&g_blk,
                            &g_loop,
                            block_endpoint,
                            g_reply_endpoint,
                            wasmos_block_buffer_phys(),
                            g_requested_instance) != WASMOS_ERR_NONE ||
        g_blk.buf_id < 0) {
        wfs_log("[fs-wfs] block buffer missing\n");
        wfs_stall();
    }
    wfs_fd_table_init(&g_fds);
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
    /* Claimed the moment the volume is mounted, not once the driver is ready:
     * between those two points the volume is already being written -- the mount
     * itself replays a journal -- and that is exactly the window in which a
     * check would report damage that is only a race. */
    wfs_volume_claim(1);
    /* Which of the three mount paths the volume took. The state byte never
     * leaves the driver, so this line is the only way to observe from outside
     * whether the previous boot's unmount actually reached the media -- which is
     * the whole of what an orderly shutdown buys, and is otherwise
     * indistinguishable from a replay that happened to find nothing. */
    if (g_mount_ctx.replayed != 0u) {
        wfs_log("[fs-wfs] mounted after replay\n");
    } else if (g_vol.super.state == (uint32_t)WFS_STATE_CLEAN) {
        wfs_log("[fs-wfs] mounted from a clean unmount\n");
    } else {
        wfs_log("[fs-wfs] mounted from a volume left dirty\n");
    }

    if (wfs_parse_requested_mount(mount_alias, sizeof(mount_alias)) != 0) {
        wfs_log("[fs-wfs] startup args missing mount=\n");
        wfs_stall();
    }
    mount_len = 0;
    while (mount_alias[mount_len] != '\0') {
        mount_len++;
    }
    if (mount_len <= 0 || mount_len >= (int32_t)sizeof(g_mount_name)) {
        wfs_log("[fs-wfs] mount alias size invalid\n");
        wfs_stall();
    }
    str_copy(g_mount_name, sizeof(g_mount_name), mount_alias);
    g_mount_len = mount_len;

    /* "fs.wfs<unit>" so two WFS volumes can be mounted at once. A unit is a full
     * byte and a virtio-blk unit is (slot << 3) | function, so it reaches 255;
     * all three digits are emitted or two disks whose units differ only in the
     * hundreds place would claim one name. */
    name_len = 0;
    while (k_service_prefix[name_len] != '\0') {
        service_name[name_len] = k_service_prefix[name_len];
        name_len++;
    }
    if (g_mount_unit >= 100u) {
        service_name[name_len++] = (char)('0' + (g_mount_unit / 100u));
    }
    if (g_mount_unit >= 10u) {
        service_name[name_len++] = (char)('0' + ((g_mount_unit / 10u) % 10u));
    }
    service_name[name_len++] = (char)('0' + (g_mount_unit % 10u));
    service_name[name_len] = '\0';
    /* TODO: the fs.backend instance encodes (kind, unit) but not the BLOCK
     * backend, so two WFS volumes whose units collide across backends -- ATA
     * unit 2 and a virtio-blk device at slot 0 function 2, say -- derive one
     * instance, and the second registration is refused and its mount never
     * appears. This is the same defect the retired `block` NAME had: a disk is
     * (backend, unit), so the instance has to carry the backend. Fixing it needs
     * a wider instance encoding, which fs-manager and fs_fat decode too. */
    /* WANTS_SHUTDOWN: this driver is the reason the sequence exists. Only a
     * clean unmount records WFS_STATE_CLEAN, and without it every volume ever
     * written stays DIRTY and replays an empty log on every later mount. */
    if (wasmos_svc_register_class_flags(g_proc_endpoint,
                                        g_fs_endpoint,
                                        service_name,
                                        FSMGR_BACKEND_CLASS,
                                        FSMGR_BACKEND_INSTANCE(FSMGR_BACKEND_BLOCK, g_mount_unit),
                                        WASMOS_SVC_FLAG_WANTS_SHUTDOWN,
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
            /* Read every field into a local before replying: the reply writes
             * into fs-manager's buffer, and a host call invalidates the
             * last-message fields. Evaluation order within one argument list is
             * unspecified, so the two must not be mixed there. */
            int32_t info_src = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
            int32_t info_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
            int32_t info_bid = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
            wfs_report_backend_info(info_src, info_req, info_bid);
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
