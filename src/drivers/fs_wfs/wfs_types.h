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
#include "wfs_status.h"
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
    /* The volume's on-disk state has been set to WFS_STATE_DIRTY for this mount,
     * so the marking is not repeated per write. Set by wfs_mark_dirty_task. */
    uint8_t dirty_marked;
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

/* Recording that a volume is mounted for writing (§4). */
typedef enum {
    WFS_DIRTY_PC_START = 0,
    WFS_DIRTY_PC_SUPER_READY,
    WFS_DIRTY_PC_SUPER_WRITTEN,
} wfs_dirty_pc_t;

typedef struct {
    wfs_dirty_pc_t pc;
    wfs_volume_t* vol;
    wasmos_error_code_t err;
} wfs_dirty_ctx_t;

/* Allocating blocks (§12). */
typedef enum {
    WFS_ALLOC_PC_START = 0,
    WFS_ALLOC_PC_DIRTY_JOINED,
    WFS_ALLOC_PC_DESC_JOINED,
    WFS_ALLOC_PC_BITMAP_READY,
    WFS_ALLOC_PC_BITMAP_WRITTEN,
    WFS_ALLOC_PC_DESC_READY,
    WFS_ALLOC_PC_DESC_WRITTEN,
} wfs_alloc_pc_t;

typedef struct {
    wfs_alloc_pc_t pc;
    wfs_volume_t* vol; /* mutable: the in-memory free counter follows the bitmap */
    uint32_t want;     /* blocks requested; a shorter run may be returned */
    uint32_t prefer_group;

    /* Results, valid once the task completes. `length` may be less than `want`:
     * §12 allocates contiguous extents where it can and falls back to fragments,
     * so a caller wanting more comes back for the remainder. */
    uint32_t first_block;
    uint32_t length;

    /* The search. `group` is the group being examined and `tried` bounds the
     * sweep at one pass over the volume, so a full filesystem terminates instead
     * of circling. `run_start` and `run_length` are the run found in `group`,
     * carried from the bitmap step to the steps that mark and account for it. */
    uint32_t group;
    uint32_t tried;
    uint32_t run_start;
    uint32_t run_length;
    uint32_t bitmap_block;
    uint32_t desc_block;

    uint8_t desc_started; /* a descriptor-read child is outstanding and owes a join */
    wasmos_wasm_coroutine_t desc_task;
    wfs_group_ctx_t desc;

    /* Marking the volume dirty, which must land BEFORE any metadata write. */
    uint8_t dirty_started;
    wasmos_wasm_coroutine_t dirty_task;
    wfs_dirty_ctx_t dirty;

    wasmos_error_code_t err;
} wfs_alloc_ctx_t;

/* Freeing blocks (§12). */
typedef enum {
    WFS_FREE_PC_START = 0,
    WFS_FREE_PC_DESC_JOINED,
    WFS_FREE_PC_BITMAP_READY,
    WFS_FREE_PC_BITMAP_WRITTEN,
    WFS_FREE_PC_DESC_READY,
    WFS_FREE_PC_DESC_WRITTEN,
} wfs_free_pc_t;

typedef struct {
    wfs_free_pc_t pc;
    wfs_volume_t* vol;
    uint32_t first_block;
    uint32_t length;

    /* The run may span groups, so it is freed one group at a time: `cursor` is
     * the next block to release and `run_in_group` how many of them fall in the
     * group being handled. Both survive the awaits. */
    uint32_t cursor;
    uint32_t group;
    uint32_t run_in_group;
    uint32_t bitmap_block;
    uint32_t desc_block;

    uint8_t desc_started;
    wasmos_wasm_coroutine_t desc_task;
    wfs_group_ctx_t desc;

    wasmos_error_code_t err;
} wfs_free_ctx_t;

/* Writing bytes into an object's data (§16). */
typedef enum {
    WFS_WRITE_PC_START = 0,
    WFS_WRITE_PC_DIRTY_JOINED,
    WFS_WRITE_PC_MAP,
    WFS_WRITE_PC_MAP_JOINED,
    WFS_WRITE_PC_ALLOC_JOINED,
    WFS_WRITE_PC_BLOCK_READ,
    WFS_WRITE_PC_BLOCK_PATCH,
    WFS_WRITE_PC_BLOCK_WRITTEN,
    WFS_WRITE_PC_RECORD_READ,
    WFS_WRITE_PC_RECORD_PATCH,
    WFS_WRITE_PC_RECORD_WRITTEN,
} wfs_write_pc_t;

typedef struct {
    wfs_write_pc_t pc;
    wfs_volume_t* vol;
    uint32_t object_id;

    /* The object record, owned here rather than borrowed: the write updates size,
     * mtime and the extent map as it goes, and seals all of it back once at the
     * end. `inline_data` mirrors wfs_object_ctx_t's -- an object carrying
     * WFS_OBJ_INLINE_DATA keeps its content in the bytes the extents array
     * occupies, which the decode into `obj.extents` destroys. */
    struct wfs_object obj;
    uint8_t inline_data[WFS_INLINE_DATA_MAX];

    uint64_t offset;
    const uint8_t* src;
    uint32_t len;
    /* Timestamp for mtime/ctime. Passed in rather than read from a clock here:
     * this driver has no time source of its own, and a write is not the place to
     * acquire one. Zero leaves the timestamps alone.
     * TODO: the RTC service is the eventual source; until a driver-side clock
     * exists every caller must supply this. */
    uint64_t now_ns;

    /* Bytes delivered, and the block the cursor is in. All survive the awaits. */
    uint32_t done;
    uint64_t logical;
    uint32_t physical;
    /* The current block was just allocated, so it holds whatever was there
     * before: a partial write must ZERO the rest rather than read it back. */
    uint8_t fresh;
    uint32_t record_block;

    uint8_t extent_started;
    wasmos_wasm_coroutine_t extent_task;
    wfs_extent_ctx_t extent;

    uint8_t alloc_started;
    wasmos_wasm_coroutine_t alloc_task;
    wfs_alloc_ctx_t alloc;

    uint8_t dirty_started;
    wasmos_wasm_coroutine_t dirty_task;
    wfs_dirty_ctx_t dirty;

    wasmos_error_code_t err;
} wfs_write_ctx_t;

/* Allocating and releasing object records (§12). */
typedef enum {
    WFS_OBJALLOC_PC_START = 0,
    WFS_OBJALLOC_PC_DIRTY_JOINED,
    WFS_OBJALLOC_PC_DESC_JOINED,
    WFS_OBJALLOC_PC_BITMAP_READY,
    WFS_OBJALLOC_PC_RECORD_READY,
    WFS_OBJALLOC_PC_RECORD_WRITTEN,
    WFS_OBJALLOC_PC_BITMAP_WRITTEN,
    WFS_OBJALLOC_PC_DESC_READY,
    WFS_OBJALLOC_PC_DESC_WRITTEN,
} wfs_objalloc_pc_t;

typedef struct {
    wfs_objalloc_pc_t pc;
    wfs_volume_t* vol;
    uint32_t prefer_group;

    /* What the new object is initialised as. A record has to be VALID the moment
     * its bit is set, so the caller states the type and mode up front rather than
     * filling them in afterwards. */
    uint16_t type;
    uint32_t mode;
    uint32_t link_count;
    uint64_t now_ns;
    /* For a directory: the object its ".." names, written into the record's first
     * extent slot by the caller's directory writer rather than here. */
    uint32_t parent_id;

    /* Result. */
    uint32_t object_id;

    uint32_t group;
    uint32_t tried;
    uint32_t slot; /* the free bit found in this group's object bitmap */
    uint32_t bitmap_block;
    uint32_t record_block;
    uint32_t desc_block;

    uint8_t desc_started;
    wasmos_wasm_coroutine_t desc_task;
    wfs_group_ctx_t desc;

    uint8_t dirty_started;
    wasmos_wasm_coroutine_t dirty_task;
    wfs_dirty_ctx_t dirty;

    wasmos_error_code_t err;
} wfs_objalloc_ctx_t;

typedef enum {
    WFS_OBJFREE_PC_START = 0,
    WFS_OBJFREE_PC_DESC_JOINED,
    WFS_OBJFREE_PC_BITMAP_READY,
    WFS_OBJFREE_PC_BITMAP_WRITTEN,
    WFS_OBJFREE_PC_DESC_READY,
    WFS_OBJFREE_PC_DESC_WRITTEN,
} wfs_objfree_pc_t;

typedef struct {
    wfs_objfree_pc_t pc;
    wfs_volume_t* vol;
    uint32_t object_id;

    uint32_t group;
    uint32_t slot;
    uint32_t bitmap_block;
    uint32_t desc_block;

    uint8_t desc_started;
    wasmos_wasm_coroutine_t desc_task;
    wfs_group_ctx_t desc;

    wasmos_error_code_t err;
} wfs_objfree_ctx_t;

/* Truncating an object (§16). */
typedef enum {
    WFS_TRUNC_PC_START = 0,
    WFS_TRUNC_PC_DIRTY_JOINED,
    WFS_TRUNC_PC_TAIL_READ,
    WFS_TRUNC_PC_TAIL_WRITTEN,
    WFS_TRUNC_PC_RECORD_READ,
    WFS_TRUNC_PC_RECORD_PATCH,
    WFS_TRUNC_PC_RECORD_WRITTEN,
    WFS_TRUNC_PC_FREE_JOINED,
} wfs_trunc_pc_t;

typedef struct {
    wfs_trunc_pc_t pc;
    wfs_volume_t* vol;
    uint32_t object_id;
    struct wfs_object obj;
    uint8_t inline_data[WFS_INLINE_DATA_MAX];
    uint64_t new_size;
    uint64_t now_ns;

    /* Runs the truncation stops referencing, collected while the extent array is
     * trimmed and released only AFTER the record no longer names them. At most
     * one run per inline extent, since an extent is either dropped whole or
     * shortened once. */
    uint32_t free_first[WFS_INLINE_EXTENTS];
    uint32_t free_len[WFS_INLINE_EXTENTS];
    uint32_t free_count;
    uint32_t free_index;

    /* The partial block the new end falls inside, if any: its bytes past the new
     * size are zeroed, or a later grow would read them back as content. */
    uint32_t tail_block;
    uint32_t tail_from;
    uint8_t tail_needed;

    uint32_t record_block;

    uint8_t free_started;
    wasmos_wasm_coroutine_t free_task;
    wfs_free_ctx_t free_ctx;

    uint8_t dirty_started;
    wasmos_wasm_coroutine_t dirty_task;
    wfs_dirty_ctx_t dirty;

    wasmos_error_code_t err;
} wfs_trunc_ctx_t;

/* Mounting a volume. */
typedef enum {
    WFS_MOUNT_PC_START = 0,
    WFS_MOUNT_PC_SUPER_READY,
    WFS_MOUNT_PC_BACKUP_READY,
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

    /* The backup-superblock scan (§5), which runs only when the primary does not
     * validate. Every one of these survives an await, so none can be a C local:
     * the candidate is recomputed from scan_index on each resume rather than
     * carried across it. primary_err is kept so a scan that finds nothing
     * reports why the PRIMARY failed, which is the useful diagnosis. */
    wasmos_error_code_t primary_err;
    uint32_t scan_index;
    uint8_t scan_started; /* a candidate read is outstanding and owes a take */
    uint8_t scan_have;    /* scan_best holds a copy that validated */
    wfs_super_t scan_best;
} wfs_mount_ctx_t;

#endif /* FS_WFS_WFS_TYPES_H */
