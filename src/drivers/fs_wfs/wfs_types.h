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

/* ---- the metadata journal (§14) ------------------------------------------
 *
 * Blocks a transaction may carry. The bound is the driver's, not the format's:
 * one descriptor block holds 254 targets at a 4096-byte block size, and the log
 * is 64 blocks at its smallest, so what constrains this is that a transaction is
 * refused WHOLE when it does not fit. Every metadata operation in this driver
 * touches a handful of blocks -- a bitmap, a group descriptor, an object record,
 * a directory block, an extent leaf -- so the ceiling is reached only by an
 * operation that should have been split into transactions of its own.
 */
#define WFS_TXN_MAX_TARGETS 24u
#define WFS_TXN_MAX_REVOKES 24u

/* One journaled block: where it belongs, where its image currently is, and the
 * checksum recovery verifies the image against before applying it. */
typedef struct {
    uint32_t target;        /* filesystem block the image replaces */
    uint32_t journal_block; /* absolute block of the log holding the image */
    uint32_t checksum;      /* CRC32C of the image, as the descriptor records it */
} wfs_txn_target_t;

/* The volume's log, and the one transaction that may be open in it.
 *
 * ONE transaction is live at a time: this driver runs the whole of §14's
 * sequence -- journal, commit, checkpoint, advance the tail -- before the next
 * transaction begins, so the log is empty between transactions and the tail is
 * always the first log block. The format permits several live transactions and a
 * checkpoint policy that retires them lazily; that costs a wrap-aware allocator
 * and a free-span calculation, and buys throughput this driver does not yet
 * need.
 *
 * TODO: batch several operations into one transaction. Every metadata write
 * currently pays a full descriptor, commit and checkpoint round trip.
 */
typedef struct {
    /* Geometry, from the journal superblock. `start` is the absolute block of
     * that superblock; log block N is start + N, for 1 <= N < blocks. */
    uint8_t loaded;
    uint32_t start;
    uint32_t blocks;
    /* The sequence the next transaction takes. Monotonic and never reused, which
     * is what lets recovery tell a live block from stale content a previous
     * transaction left at the same offset. */
    uint64_t next_sequence;

    /* The open transaction. */
    uint8_t open;
    uint64_t sequence;
    uint32_t target_count;
    wfs_txn_target_t targets[WFS_TXN_MAX_TARGETS];
    /* Blocks that stop being metadata in this transaction (§18). A revoke is
     * mandatory for each, because the log records block numbers rather than what
     * a block holds: an older committed image of a freed block stays replayable
     * after the block is handed to a file. */
    uint32_t revoke_count;
    uint32_t revokes[WFS_TXN_MAX_REVOKES];

    /* The volume's free counters have moved since the superblock last recorded
     * them, so a sync has something to write. Set by the allocator through
     * wfs_txn_note_counters and cleared by wfs_sync_task.
     *
     * They are NOT written per transaction, and cannot be journaled inside one
     * either: a target_block of 0 terminates a descriptor's target list, and
     * recovery refuses block 0 as a replay destination so a damaged descriptor
     * cannot overwrite the boot area and the primary superblock. Neither price is
     * worth paying for numbers §4 calls derived and advisory -- the bitmaps are
     * authoritative and land inside the transaction, and fsck recomputes these
     * from them. A sync writes them; a crash leaves them trailing. */
    uint8_t counters_dirty;

    /* A stage that failed before it reached the device, held until the take
     * reports it. wfs_txn_stage_begin returns NULL for both "nothing to await"
     * cases -- a refusal and a staging failure -- and without this the refusal
     * would pass for a write that succeeded, which is the mistake wfs_block_t's
     * own stage_failed exists to prevent one layer down. */
    wasmos_error_code_t stage_err;
} wfs_journal_t;

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
    wfs_journal_t journal;
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

/* Writing the volume superblock (§4). The one writer every other caller goes
 * through, so `generation` cannot be advanced by some paths and not others. */
typedef enum {
    WFS_SB_PC_START = 0,
    WFS_SB_PC_PRIMARY_READY,
    WFS_SB_PC_PRIMARY_WRITTEN,
    WFS_SB_PC_BACKUP_READY,
    WFS_SB_PC_BACKUP_WRITTEN,
} wfs_sb_pc_t;

typedef struct {
    wfs_sb_pc_t pc;
    wfs_volume_t* vol;

    /* What to record. `state` is always written; the free counters only when
     * `set_counters` is set, because most superblock writes are state
     * transitions that have no reason to touch them. */
    uint32_t state;
    uint8_t set_counters;
    /* Also refresh the backup copies (§5). Set for a STATE TRANSITION and clear
     * otherwise: `state` is the field a stale backup gets dangerously wrong, and
     * a backup is allowed to trail on everything else because §5 orders copies by
     * generation and a trailing one correctly loses. */
    uint8_t refresh_backups;

    /* The generation this write took, which is one past what the image held. */
    uint64_t generation;

    /* The backup sweep. Both survive the awaits. */
    uint32_t backup_index;
    uint32_t backup_block;

    wasmos_error_code_t err;
} wfs_sb_ctx_t;

/* Reconciling the superblock with the volume, and recording how it was left
 * (§4). A sync is what makes the free counters true on disk, and the CLEAN state
 * at unmount is what tells the next mount its log holds nothing to replay. */
typedef enum {
    WFS_SYNC_PC_START = 0,
    WFS_SYNC_PC_WRITE_JOINED,
} wfs_sync_pc_t;

typedef struct {
    wfs_sync_pc_t pc;
    wfs_volume_t* vol;
    /* The state to leave the volume in: WFS_STATE_DIRTY for a sync that keeps it
     * mounted, WFS_STATE_CLEAN for an unmount. */
    uint32_t state;

    uint8_t write_started;
    wasmos_wasm_coroutine_t write_task;
    wfs_sb_ctx_t write;

    wasmos_error_code_t err;
} wfs_sync_ctx_t;

/* Recording that a volume is mounted for writing (§4). */
typedef enum {
    WFS_DIRTY_PC_START = 0,
    WFS_DIRTY_PC_WRITE_JOINED,
} wfs_dirty_pc_t;

typedef struct {
    wfs_dirty_pc_t pc;
    wfs_volume_t* vol;

    uint8_t write_started;
    wasmos_wasm_coroutine_t write_task;
    wfs_sb_ctx_t write;

    wasmos_error_code_t err;
} wfs_dirty_ctx_t;

/* Allocating blocks (§12). */
typedef enum {
    WFS_ALLOC_PC_START = 0,
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
    /* The run held METADATA -- an extent-tree node, or a directory's records --
     * so every block in it is revoked as it is freed (§18). The log records block
     * NUMBERS rather than what a block holds, so an older committed image of one
     * of these stays replayable once the block has been handed to a file, and
     * replaying it would overwrite that file's data with stale metadata.
     *
     * Clear for file data, which is never journaled and so has no image to bar. */
    uint8_t metadata;

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

/* Adding one extent to an object whose inline map is full (§9). */
typedef enum {
    WFS_XTADD_PC_START = 0,
    WFS_XTADD_PC_NODE_READY, /* the root, which may be a leaf or interior */
    WFS_XTADD_PC_LEAF_READY, /* the leaf the insert belongs in */
    WFS_XTADD_PC_LEAF_WRITTEN,
    WFS_XTADD_PC_SPLIT_LOWER, /* the full leaf, rewritten with the records it keeps */
    WFS_XTADD_PC_SPLIT_UPPER, /* the new leaf holding the records it gave up */
    WFS_XTADD_PC_ROOT_READY,  /* the interior root, read so the new leaf can be indexed */
    WFS_XTADD_PC_ROOT_WRITTEN,
} wfs_xtadd_pc_t;

typedef struct {
    wfs_xtadd_pc_t pc;
    wfs_volume_t* vol;
    /* Updated IN PLACE. extent_tree_block, extent_count and the inline array are
     * this task's outputs; the caller seals them into the object record. */
    struct wfs_object* obj;

    /* The extent to add. `physical` is a block number, `length` a block count. */
    uint64_t logical;
    uint32_t physical;
    uint32_t length;

    /* Blocks the CALLER allocated for this task to consume. Allocating them in
     * the caller keeps the allocator a sibling sub-task rather than nesting one
     * level deeper under this one.
     *
     * `leaf_block` becomes the leaf a promotion creates, or the one a SPLIT moves
     * the upper half of a full leaf into. `root_spare` becomes the interior root
     * the FIRST split adds above them, and is needed only then -- a tree that
     * already has an interior root indexes the new leaf into it.
     *
     * Neither is always used, so the caller must release what `leaf_used` and
     * `root_used` say went unconsumed. Reporting it is what keeps this task from
     * having to free a block itself, which would need the allocator nested here
     * after all. */
    uint32_t leaf_block;
    uint32_t root_spare;
    uint8_t leaf_used;
    uint8_t root_used;

    /* The node being edited, and the interior root above it when there is one.
     * `root_block` is 0 while the tree is a single leaf. */
    uint32_t node_block;
    uint32_t root_block;
    /* Which of the root's children `node_block` is, so a split knows where the
     * new leaf's index belongs. */
    uint32_t child;
    /* First logical block of the half a split moved out, which is the key the
     * new leaf is indexed under. */
    uint64_t split_logical;
    /* Records the split moved, held in the file's own buffer across the two
     * writes: one staged block cannot hold both halves at once. */
    uint32_t split_count;
    /* Records this added to the map: 1 for an insert, 0 when it coalesced into
     * an existing record, and the leaf's whole entry count for a promotion. */
    uint32_t added;

    wasmos_error_code_t err;
} wfs_xtadd_ctx_t;

/* Removing one run from an extent tree's leaf (§9). One step of a trim: the
 * caller loops until `freed_length` comes back zero, releasing the reported run
 * between steps.
 *
 * Split this way so the block free stays the CALLER's sub-task rather than
 * nesting under this one, and so the leaf is left consistent at every step: it
 * is rewritten without the run before the run is released, never the reverse. */
typedef enum {
    WFS_XTTRIM_PC_START = 0,
    WFS_XTTRIM_PC_ROOT_READY, /* the root, which may be a leaf or interior */
    WFS_XTTRIM_PC_LEAF_READY,
    WFS_XTTRIM_PC_LEAF_WRITTEN,
    WFS_XTTRIM_PC_ROOT_WRITTEN, /* the root, with an emptied child dropped */
} wfs_xttrim_pc_t;

typedef struct {
    wfs_xttrim_pc_t pc;
    wfs_volume_t* vol;
    uint32_t node_block;
    /* Logical blocks the object retains. Zero drops every record, which is what
     * releasing a whole object needs. */
    uint64_t keep;

    /* The run this step detached, for the caller to release. Zero length means
     * nothing was left to trim and the loop is done. */
    uint32_t freed_first;
    uint32_t freed_length;
    /* A METADATA block this step unreferenced -- a leaf an interior tree emptied
     * out -- for the caller to release with wfs_free_ctx_t's `metadata` set, so
     * its number is revoked (§18). Zero when the step freed no node. */
    uint32_t freed_node;
    /* Records this step removed from the map: 1 when a whole extent went, 0 when
     * one was merely shortened or a node was dropped. The caller keeps the
     * object's extent_count by it, which no single step could recompute for a
     * tree without reading every leaf. */
    uint32_t removed;
    /* Records still in the map after this step. Zero means the map is empty and
     * the caller releases the root too. */
    uint32_t remaining;
    /* The interior root, when the map has one, and the child this step is
     * working in. Both survive the awaits. */
    uint32_t root_block;
    uint32_t child;

    wasmos_error_code_t err;
} wfs_xttrim_ctx_t;

/* Writing bytes into an object's data (§16). */
typedef enum {
    WFS_WRITE_PC_START = 0,
    WFS_WRITE_PC_PREPARED,
    WFS_WRITE_PC_MAP,
    WFS_WRITE_PC_MAP_JOINED,
    WFS_WRITE_PC_ALLOC_JOINED,
    WFS_WRITE_PC_INLINE_ALLOC_JOINED,
    WFS_WRITE_PC_INLINE_WRITTEN,
    WFS_WRITE_PC_LEAF_ALLOC_JOINED,
    WFS_WRITE_PC_SPLIT_ALLOC_JOINED,
    WFS_WRITE_PC_XTADD_JOINED,
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

    /* One extent waiting to go into the extent TREE, held until its data block
     * is on disk: a leaf is reachable from the object record the moment it is
     * written, so publishing an extent first would name a block still holding
     * what it held before. The inline map needs no such delay, because it is
     * sealed into the record at the very end. */
    /* An INLINE object that must outgrow the record. Its bytes live in the
     * record where extents would be, so they are copied into a first data block
     * and the flag cleared before the write proper begins. */
    uint8_t promote_inline;
    uint32_t promote_block;

    uint8_t tree_pending;
    uint64_t pending_logical;
    uint32_t pending_physical;
    uint32_t pending_length;

    uint8_t xtadd_started;
    wasmos_wasm_coroutine_t xtadd_task;
    wfs_xtadd_ctx_t xtadd;

    /* Blocks a leaf SPLIT asked for, held across the retries that supply them.
     * The extent writer modifies nothing when it asks (WASMOS_ERR_FS_NEED_BLOCK),
     * so the retry descends to the same leaf; carrying them here is what stops
     * each retry from allocating another. `split_want` says which one the
     * allocation in flight is for. */
    uint32_t split_leaf;
    uint32_t split_root;
    uint32_t split_want;

    wasmos_error_code_t err;
} wfs_write_ctx_t;

/* Allocating and releasing object records (§12). */
typedef enum {
    WFS_OBJALLOC_PC_START = 0,
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
    WFS_TRUNC_PC_PREPARED,
    WFS_TRUNC_PC_TAIL_READ,
    WFS_TRUNC_PC_TAIL_WRITTEN,
    WFS_TRUNC_PC_RECORD_READ,
    WFS_TRUNC_PC_RECORD_PATCH,
    WFS_TRUNC_PC_RECORD_WRITTEN,
    WFS_TRUNC_PC_FREE_JOINED,
    WFS_TRUNC_PC_TREE_PREPARED,
    WFS_TRUNC_PC_TRIM_JOINED,
    WFS_TRUNC_PC_TRIM_FREE_JOINED,
    WFS_TRUNC_PC_TAIL_LOOKUP_JOINED,
    WFS_TRUNC_PC_INLINE_ALLOC_JOINED,
    WFS_TRUNC_PC_INLINE_WRITTEN,
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

    /* Trimming an extent TREE, which is unbounded where the inline free list is
     * not: one run is detached and released per step, so no array has to hold
     * every run a large object drops. */
    uint8_t trim_started;
    wasmos_wasm_coroutine_t trim_task;
    wfs_xttrim_ctx_t trim;
    /* Logical blocks retained, and the leaf's root, held across the trim steps. */
    uint64_t trim_keep;
    uint32_t trim_root;

    /* Resolving the physical block behind the logical one the new end falls in.
     * An inline map is scanned in memory; a TREE needs a descent, which is this
     * sub-task. */
    uint8_t extent_started;
    wasmos_wasm_coroutine_t extent_task;
    wfs_extent_ctx_t extent;

    /* Promoting an INLINE object that a grow takes past the record, the way a
     * write does: its bytes move into a first data block and the flag clears. */
    uint8_t promote_inline;
    uint32_t promote_block;
    uint8_t alloc_started;
    wasmos_wasm_coroutine_t alloc_task;
    wfs_alloc_ctx_t alloc;

    wasmos_error_code_t err;
} wfs_trunc_ctx_t;

/* Reading the journal superblock (§14). */
typedef enum {
    WFS_JLOAD_PC_START = 0,
    WFS_JLOAD_PC_SUPER_READY,
} wfs_jload_pc_t;

typedef struct {
    wfs_jload_pc_t pc;
    wfs_volume_t* vol;
    wasmos_error_code_t err;
} wfs_jload_ctx_t;

/* Committing and retiring a transaction (§14 steps 1-7). */
typedef enum {
    WFS_TXCOMMIT_PC_START = 0,
    WFS_TXCOMMIT_PC_DESC_WRITTEN,
    WFS_TXCOMMIT_PC_REVOKE_WRITTEN,
    WFS_TXCOMMIT_PC_COMMIT_WRITTEN,
    WFS_TXCOMMIT_PC_IMAGE_READ,
    WFS_TXCOMMIT_PC_TARGET_WRITTEN,
    WFS_TXCOMMIT_PC_TAIL_WRITTEN,
} wfs_txcommit_pc_t;

typedef struct {
    wfs_txcommit_pc_t pc;
    wfs_volume_t* vol;
    /* The checkpoint cursor: which target is being written back. Survives both
     * awaits of each iteration. */
    uint32_t index;
    wasmos_error_code_t err;
} wfs_txcommit_ctx_t;

/* Replaying the log after an unclean unmount (§21).
 *
 * The walk holds ONE transaction, which is the shape wfs_journal.c writes: a
 * transaction is checkpointed and retired before the next begins, so the log
 * never carries a chain. A log that does carry one was written by something
 * else, and is refused rather than half-applied.
 *
 * TODO: a chain needs §21's three separate passes -- a revoke table built over
 * the whole replay set before any image is applied, because a later
 * transaction's revoke bars an earlier transaction's image. Land it with the
 * batching that would produce a chain in the first place.
 */
typedef enum {
    WFS_REPLAY_PC_START = 0,
    WFS_REPLAY_PC_LOAD_JOINED,
    WFS_REPLAY_PC_DESC_READY, /* the record the tail names */
    WFS_REPLAY_PC_SCAN_READY, /* the revoke and commit blocks behind it */
    WFS_REPLAY_PC_IMAGE_READY,
    WFS_REPLAY_PC_TARGET_WRITTEN,
    WFS_REPLAY_PC_TAIL_WRITTEN,
} wfs_replay_pc_t;

typedef struct {
    wfs_replay_pc_t pc;
    wfs_volume_t* vol;

    /* The forward walk (§21 pass 1). `cursor` is journal-relative and `sequence`
     * is the transaction the tail says should be there. */
    uint32_t cursor;
    uint64_t sequence;
    uint8_t committed; /* a valid COMMIT carrying `sequence` was found */

    /* The transaction's targets, as its descriptor named them, and the blocks it
     * revoked (§21 pass 2). */
    uint32_t target_count;
    uint32_t targets[WFS_TXN_MAX_TARGETS];
    uint32_t checksums[WFS_TXN_MAX_TARGETS];
    uint32_t revoke_count;
    uint32_t revokes[WFS_TXN_MAX_REVOKES];

    /* The apply cursor (§21 pass 3), and how many images it wrote. `applied` is
     * the task's result: zero means the log held nothing to replay, which is the
     * ordinary case for a volume that merely crashed between transactions. */
    uint32_t index;
    uint32_t applied;

    uint8_t load_started;
    wasmos_wasm_coroutine_t load_task;
    wfs_jload_ctx_t load;

    wasmos_error_code_t err;
} wfs_replay_ctx_t;

/* Mounting a volume. */
typedef enum {
    WFS_MOUNT_PC_START = 0,
    WFS_MOUNT_PC_SUPER_READY,
    WFS_MOUNT_PC_BACKUP_READY,
    WFS_MOUNT_PC_GROUP_JOINED,
    WFS_MOUNT_PC_JLOAD_JOINED,
    WFS_MOUNT_PC_REPLAY_JOINED,
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

    /* The log: read on every mount so transactions are available, replayed only
     * when the volume was not unmounted cleanly (§15). Both children's records
     * live here for the same reason the group sweep's do. */
    uint8_t jload_started;
    wasmos_wasm_coroutine_t jload_task;
    wfs_jload_ctx_t jload;
    uint8_t replay_started;
    wasmos_wasm_coroutine_t replay_task;
    wfs_replay_ctx_t replay;
    /* Why the volume is read-only, when the log is the reason: reported so a
     * caller can tell an unusable log from a replay that failed part way. Zero
     * when the log was fine. */
    wasmos_error_code_t journal_err;
    /* Block images the replay applied, for a caller that logs the recovery. */
    uint32_t replayed;
} wfs_mount_ctx_t;

#endif /* FS_WFS_WFS_TYPES_H */
