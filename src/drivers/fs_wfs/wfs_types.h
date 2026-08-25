/* wfs_types.h - the driver's operation contexts.
 *
 * Operations are tasks on the SYSTEM coroutine runtime
 * (src/libsys/wasm/coroutine_wasm.h, docs/architecture/32): each is a
 * wasmos_wasm_task_resume_fn that awaits a future and returns
 * WASMOS_WASM_TASK_YIELDED, and the runtime parks and resumes it. The runtime
 * owns the ready list, the waiter lists, and the settlement of every future;
 * this driver owns only the per-operation state below.
 *
 * The runtime preserves no stack across a resume, so a task records its own
 * resume point. That is what `pc` is, and it is why any value that must survive
 * an await lives in the context struct rather than in a C local — a local is
 * read uninitialised on the resume path, and a block number read that way sends
 * a request for whatever the stack happened to hold. Values computed strictly
 * after an await may be locals.
 */
#ifndef FS_WFS_WFS_TYPES_H
#define FS_WFS_WFS_TYPES_H

#include <stdint.h>

#include "wasmos/coroutine_wasm.h"
#include "wasmos_status.h"
#include "wfs_format.h"
#include "wfs_super.h"

/* A mounted volume: the superblock as the reader parsed it.
 *
 * Group descriptors are not held here. A volume of many groups has a descriptor
 * table larger than a driver's linear memory should carry, so a descriptor is
 * read from its block when its group is touched; the table's location is in the
 * superblock already. */
typedef struct {
    wfs_super_t super;
    uint8_t mounted;
} wfs_volume_t;

/* Reading one group descriptor. */
typedef enum {
    WFS_GROUP_PC_START = 0,
    WFS_GROUP_PC_BLOCK_READY,
} wfs_group_pc_t;

typedef struct {
    wfs_group_pc_t pc;
    const wfs_volume_t* vol;
    uint32_t group; /* which descriptor is wanted */
    uint32_t block; /* the block holding it; must survive the await */
    wasmos_error_code_t err;
    struct wfs_group_desc out;
} wfs_group_ctx_t;

/* Reading one object record. */
typedef enum {
    WFS_OBJECT_PC_START = 0,
    WFS_OBJECT_PC_BLOCK_READY,
} wfs_object_pc_t;

typedef struct {
    wfs_object_pc_t pc;
    const wfs_volume_t* vol;
    uint32_t object_id;
    uint32_t block; /* must survive the await, for the same reason */
    wasmos_error_code_t err;
    struct wfs_object out;

    /* An object carrying WFS_OBJ_INLINE_DATA stores its content in the bytes the
     * extents array occupies (§7), so those bytes are kept VERBATIM here as well
     * as decoded into `out.extents`. Decoding alone would destroy them: the
     * decode reads them as little-endian block numbers and lengths, and there is
     * no way back from that to the file's bytes.
     *
     * Valid for `out.size` bytes when the flag is set, and zero otherwise. */
    uint8_t inline_data[WFS_INLINE_DATA_MAX];
} wfs_object_ctx_t;

/* Walking an object's extent map: logical block -> physical block. */
typedef enum {
    WFS_EXTENT_PC_START = 0,
    WFS_EXTENT_PC_NODE_READY,
} wfs_extent_pc_t;

typedef struct {
    wfs_extent_pc_t pc;
    const wfs_volume_t* vol;
    /* The object whose map is walked. Borrowed: it must outlive the task, which
     * it does when it lives in the caller's own context. */
    const struct wfs_object* obj;
    uint64_t logical; /* the logical block wanted */

    uint32_t block;       /* node block to read; must survive the await */
    uint32_t depth_guard; /* remaining permitted descents */
    wasmos_error_code_t err;

    /* Result. `found` is 0 for a HOLE, which is not an error: a sparsely
     * written file has logical ranges no extent maps, and they read as zeroes.
     * `run` is how many contiguous blocks follow `physical`, so a caller can
     * read a whole extent in one request instead of a block at a time. */
    uint8_t found;
    uint32_t physical;
    uint32_t run;
} wfs_extent_ctx_t;

/* Scanning a directory: resolve a name, or walk entries one at a time.
 *
 * A directory is stored as regular file data (§10), so its blocks are reached
 * through the extent map — which is why this runs the extent walk as a child
 * task rather than addressing blocks itself. */
typedef enum {
    WFS_DIR_PC_START = 0,
    WFS_DIR_PC_MAP,  /* start the extent walk for the cursor's logical block */
    WFS_DIR_PC_JOIN, /* collect it */
    WFS_DIR_PC_READ, /* read the block it named */
    WFS_DIR_PC_SCAN, /* walk the records in that block */
} wfs_dir_pc_t;

typedef struct {
    wfs_dir_pc_t pc;
    const wfs_volume_t* vol;
    /* The directory being scanned. Borrowed, so it must outlive the task. */
    const struct wfs_object* dir;

    /* The name to find. A zero `want_len` means "the next entry from the
     * cursor" instead, which is what a readdir walks with. */
    const char* want;
    uint32_t want_len;

    /* The cursor: where the next scan resumes. Left just past the entry that
     * was reported, so repeated runs walk the directory. */
    uint64_t logical;
    uint32_t offset;

    uint32_t physical; /* the cursor's block, once mapped; survives the await */
    wasmos_error_code_t err;

    /* The extent walk, run as a child. Both records live here because the
     * runtime requires them to outlive the child, and a task cannot keep them
     * on a stack that does not survive its own await. */
    uint8_t extent_started;
    wasmos_wasm_coroutine_t extent_task;
    wfs_extent_ctx_t extent;

    /* The entry found, if any.
     *
     * The name is COPIED rather than pointed at. A pointer into the staged block
     * is valid only until the next await, and this task awaits again on its very
     * next run — so a borrowed name would be read out of whatever block was
     * staged by then. */
    uint8_t found;
    uint32_t object_id;
    uint8_t type;
    uint8_t name_length;
    char name[WFS_NAME_MAX + 1u];
} wfs_dir_ctx_t;

/* Resolving a whole path to an object, one component at a time.
 *
 * Absolute paths only: the driver is handed a path already rooted by the caller,
 * so there is no working directory here to resolve against. */
typedef enum {
    WFS_PATH_PC_START = 0,
    WFS_PATH_PC_OBJECT, /* read the object the walk currently stands on */
    WFS_PATH_PC_OBJECT_JOIN,
    WFS_PATH_PC_LOOKUP, /* find the next component in it */
    WFS_PATH_PC_LOOKUP_JOIN,
} wfs_path_pc_t;

#define WFS_PATH_MAX 512u

typedef struct {
    wfs_path_pc_t pc;
    const wfs_volume_t* vol;

    /* The path, copied in: the caller's buffer is a transfer buffer whose
     * contents outlive nothing in particular, and this walk awaits repeatedly. */
    char path[WFS_PATH_MAX];
    uint32_t path_len;
    uint32_t cursor; /* offset of the component being resolved */

    uint32_t object_id; /* the object the walk stands on */
    wasmos_error_code_t err;

    uint8_t child_started;
    wasmos_wasm_coroutine_t child;
    wfs_object_ctx_t object;
    wfs_dir_ctx_t dir;

    /* The object the path named, once the walk completes. */
    uint8_t found;
} wfs_path_ctx_t;

/* Reading bytes out of an object.
 *
 * The destination is plain driver memory. Landing bytes in a client's transfer
 * buffer is the dispatch layer's job, and keeping it out of here leaves room for
 * the zero-copy path — where the block server writes the client's buffer
 * directly — without this op changing shape. */
typedef enum {
    WFS_READ_PC_START = 0,
    WFS_READ_PC_MAP,   /* start the extent walk for the byte the cursor is on */
    WFS_READ_PC_JOIN,  /* collect it */
    WFS_READ_PC_BLOCK, /* read the block it named */
    WFS_READ_PC_COPY,  /* take the slice this iteration wants */
} wfs_read_pc_t;

typedef struct {
    wfs_read_pc_t pc;
    const wfs_volume_t* vol;
    /* The object to read. Borrowed, so it must outlive the task. */
    const struct wfs_object* obj;
    /* The object's inline bytes, as wfs_object_ctx_t kept them. Required when
     * the object carries WFS_OBJ_INLINE_DATA and unused otherwise: the decoded
     * `obj` cannot supply them, because the decode read those same bytes as
     * block numbers. */
    const uint8_t* inline_data;

    uint64_t offset; /* byte offset in the object */
    uint8_t* dst;
    uint32_t len; /* bytes requested */

    /* Bytes delivered so far, and the block the cursor is in. Both survive the
     * awaits, so neither can be a C local. */
    uint32_t done;
    uint64_t logical;
    uint32_t physical;
    wasmos_error_code_t err;

    uint8_t extent_started;
    wasmos_wasm_coroutine_t extent_task;
    wfs_extent_ctx_t extent;
} wfs_read_ctx_t;

/* Mounting a volume. */
typedef enum {
    WFS_MOUNT_PC_START = 0,
    WFS_MOUNT_PC_SUPER_READY,
    WFS_MOUNT_PC_GROUP_JOINED,
} wfs_mount_pc_t;

typedef struct {
    wfs_mount_pc_t pc;
    wfs_volume_t* vol;
    wasmos_error_code_t err;

    /* The group-descriptor sweep. The child task's record and context are held
     * here because the runtime requires both to outlive the task, and a task
     * cannot keep them on a stack that does not survive its own await. */
    uint32_t next_group;
    uint8_t group_started; /* a child task is outstanding and owes a join */
    wasmos_wasm_coroutine_t group_task;
    wfs_group_ctx_t group;
} wfs_mount_ctx_t;

#endif /* FS_WFS_WFS_TYPES_H */
