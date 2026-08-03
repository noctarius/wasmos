/* fat_types.h - shared types & constants for the FAT backend reactor.
 *
 * The driver is a single-threaded event reactor: every filesystem request
 * becomes a fat_op_ctx_t whose op-specific state machine advances one
 * block-I/O boundary at a time (see fat_block.h).  This header holds the
 * on-disk FAT structures (preserved verbatim from the original driver), the
 * per-op context, and the reactor's small vocabulary of enums.  No block I/O
 * or global mutable state lives here. */
#ifndef FS_FAT_FAT_TYPES_H
#define FS_FAT_FAT_TYPES_H

#include <stdint.h>

#define FAT_SECTOR_SIZE 512u
#define FAT_MAX_SECTOR_BYTES 4096u
#define FAT_LFN_MAX 255u
#define FAT_MAX_OPEN_FILES 16u
#define FAT_MAX_PATH 128u

/* open() flag bits (mirror of the libc O_* subset the FS ABI forwards). */
#define FAT_OPEN_APPEND 0x0008
#define FAT_OPEN_CREAT 0x0040
#define FAT_OPEN_TRUNC 0x0200

/* Reactor sizing.  Linear memory is no longer capped at 64 KB, so the op table
 * can be generous; the single shared block buffer still serializes physical
 * I/O, so in-flight ops beyond a handful only add latency-hiding, not
 * throughput.  FAT_IO_QUEUE_CAP bounds ops parked waiting for the block
 * buffer. */
#define FAT_MAX_INFLIGHT 8u
#define FAT_IO_QUEUE_CAP FAT_MAX_INFLIGHT

/* IPC send retry budget for streamed READDIR output when the client endpoint is
 * transiently full (carried over from the original driver). */
#define IPC_ERR_FULL (-3)
#define FAT_STREAM_SEND_RETRIES 8192

typedef enum { FAT_BOOT_INIT = 0, FAT_BOOT_WAIT, FAT_BOOT_READY, FAT_BOOT_FAILED } fat_boot_phase_t;

typedef enum { FAT_TYPE_UNKNOWN = 0, FAT_TYPE_12, FAT_TYPE_16, FAT_TYPE_32 } fat_type_t;

typedef enum { VFS_MOUNT_BOOT = 0 } vfs_mount_t;

/* Top-level operation an fat_op_ctx_t is executing. */
typedef enum {
    FAT_OP_NONE = 0,
    FAT_OP_OPEN,
    FAT_OP_READ,
    FAT_OP_WRITE,
    FAT_OP_STAT,
    FAT_OP_SEEK,
    FAT_OP_CLOSE,
    FAT_OP_UNLINK,
    FAT_OP_MKDIR,
    FAT_OP_RMDIR,
    FAT_OP_READDIR, /* directory enumeration (fat_dir.c) */
    FAT_OP_CHDIR    /* change directory      (fat_dir.c) */
} fat_op_t;

/* Result of advancing an op's state machine one step. */
typedef enum {
    FAT_R_DONE = 0, /* op complete: send FS_IPC_RESP using ctx->resp_* */
    FAT_R_WAIT = 1, /* op submitted block I/O: resume on completion */
    FAT_R_ERR = 2   /* op failed: send FS_IPC_ERROR using ctx->err (an WASMOS_ERR_FS_*) */
} fat_r_t;

#pragma pack(push, 1)
typedef struct {
    uint8_t jump[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t ext[54];
} fat_bpb_t;
#pragma pack(pop)

typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t sectors;
} fat_mbr_entry_t;

/* An open file descriptor (fd = index + 3). */
typedef struct {
    uint8_t in_use;
    int32_t owner;
    int32_t flags;
    uint16_t first_cluster;
    uint16_t current_cluster;
    uint32_t current_sector;
    uint32_t file_lba;
    uint32_t size;
    uint32_t capacity;
    uint32_t offset;
    uint32_t dir_lba;
    uint32_t dir_sector;
    uint32_t dir_index;
} fat_open_file_t;

/* A resolved directory entry (result of a directory scan). */
typedef struct {
    uint8_t valid;
    uint8_t attr;
    uint16_t cluster;
    uint32_t size;
    uint32_t dir_lba;
    uint32_t dir_sector;
    uint32_t dir_index;
} fat_dir_entry_info_t;

/* Long-file-name accumulation state, gathered across the LFN entries that
 * precede a short entry during a directory scan (was the g_lfn_* globals). */
typedef struct {
    char buf[FAT_LFN_MAX + 1u];
    uint8_t total;
    uint8_t seen;
    uint8_t valid;
} fat_lfn_t;

/* --- Coroutine sub-machine contexts (fat_alloc.c).  Each carries `int cont`. --- */

/* Read or write a single FAT-table entry (handles the FAT12 nibble packing and
 * the rare 2-sector boundary; writes fan out across all FAT copies). */
typedef struct {
    int cont;
    uint16_t cluster;     /* entry index */
    uint16_t value;       /* read result */
    uint16_t write_value; /* value to store (write) */
    uint8_t lo, hi;       /* the entry's two bytes */
    uint32_t copy_idx;    /* FAT copy being written */
    uint32_t fat_offset;  /* byte offset of the entry within a FAT copy */
    uint32_t fat_lba;     /* base lba of the entry's first byte (this copy) */
    uint32_t sector_offset;
} fat_fatent_ctx_t;

/* Resolve one cluster's successor.  next == 0 means end-of-chain. */
typedef struct {
    int cont;
    uint16_t cluster;
    uint16_t next;
    fat_fatent_ctx_t ent;
} fat_chain_ctx_t;

/* Walk a cluster chain to its end (used for capacity and last-cluster). */
typedef struct {
    int cont;
    uint16_t cluster; /* cursor */
    uint16_t last;    /* last cluster reached */
    uint32_t hops;    /* clusters visited */
    fat_chain_ctx_t step;
} fat_chainwalk_ctx_t;

/* Scan the FAT for the first free cluster. */
typedef struct {
    int cont;
    uint32_t cursor; /* current cluster index */
    uint32_t total;  /* total data clusters */
    uint16_t result; /* found free cluster */
    fat_fatent_ctx_t ent;
} fat_findfree_ctx_t;

/* --- Coroutine sub-machine contexts (fat_dir.c). --- */

/* Scan one directory for a target name (follows the cluster chain for a
 * non-root directory).  On a match fills `found` (found.valid = 1); on a miss
 * returns with found.valid = 0. */
typedef struct {
    int cont;
    /* Inputs (set before the first step). */
    const char* target;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t entry_limit;
    uint16_t cur_cluster;
    uint8_t cur_root;
    /* Scan cursors. */
    uint32_t entries_left;
    uint32_t cur_sector;
    uint32_t scan_index;
    uint32_t entries_total;
    fat_lfn_t lfn;
    fat_dir_entry_info_t found;
    fat_chain_ctx_t chain;
} fat_dir_scan_ctx_t;

/* Resolve a path to its directory entry (walks components, descends subdirs). */
typedef struct {
    int cont;
    const char* path;
    int32_t source;
    uint32_t pos;
    uint8_t cur_root;
    uint16_t cur_cluster;
    uint32_t cur_lba;
    uint32_t cur_sectors;
    int comp_rc;
    int has_more;
    char component[FAT_MAX_PATH];
    fat_dir_entry_info_t found;
    fat_dir_scan_ctx_t scan;
} fat_resolve_ctx_t;

/* Resolve a path to its PARENT directory + leaf name (for create/delete).  The
 * parent descriptor is packed into `found`: found.dir_lba = parent first LBA,
 * found.dir_sector = parent span in sectors, found.attr bit0 = parent-is-root,
 * found.cluster = parent cluster; `name` holds the leaf component. */
typedef struct {
    int cont;
    const char* path;
    int32_t source;
    uint32_t pos;
    uint8_t cur_root;
    uint16_t cur_cluster;
    uint32_t cur_lba;
    uint32_t cur_sectors;
    int comp_rc;
    int has_more;
    char component[FAT_MAX_PATH];
    char name[FAT_MAX_PATH];
    fat_dir_entry_info_t found;
    fat_dir_scan_ctx_t scan;
} fat_resolve_parent_ctx_t;

/* --- Coroutine sub-machine contexts (fat_dir.c, mutation side). --- */

/* Free every cluster of a chain (walk it, writing 0 into each FAT entry).  The
 * successor is resolved BEFORE the entry is cleared, mirroring the original. */
typedef struct {
    int cont;
    uint16_t cluster;      /* current cluster being freed */
    uint16_t next;         /* resolved successor (before clearing) */
    uint8_t has_next;      /* 1 if `next` was resolved as an in-chain successor */
    fat_chain_ctx_t chain; /* successor lookup */
    fat_fatent_ctx_t ent;  /* clear-entry write */
} fat_freechain_ctx_t;

/* Scan a directory's raw 8.3 entries for a given 11-byte short name.
 * result: 1 = present, 0 = absent (end-of-dir or exhausted). */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t entry_limit;
    uint8_t short_name[11];
    uint32_t entries_left;
    uint32_t cur_sector;
    uint32_t entries_total;
    uint32_t scan_index;
    int result; /* 1 present, 0 absent */
} fat_shortscan_ctx_t;

/* Find `needed` consecutive free directory slots; out_entry gets the first
 * slot's flat entry index.  result: 0 = found, -1 = none. */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t entry_limit;
    uint32_t needed;
    uint32_t run;
    uint32_t run_start;
    uint32_t entry;  /* flat entry cursor */
    uint32_t sector; /* derived (crosses the per-sector yield) */
    uint32_t out_entry;
    int result; /* 0 found, -1 none */
} fat_findslots_ctx_t;

/* Read-modify-write one 32-byte directory entry into its sector. */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t entry_index;
    uint32_t sector; /* derived; crosses the read->write yield */
    uint32_t index;  /* derived; crosses the read->write yield */
    uint8_t entry[32];
} fat_writeent_ctx_t;

/* Mark a short entry + its preceding LFN entries as deleted (0xE5). */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t entry_index; /* cursor: the short entry, then walks back over LFNs */
    uint32_t prev_index;
    uint32_t sector; /* derived; crosses the read yield */
    uint32_t index;  /* derived; crosses the read yield */
    uint8_t tombstone[32];
    fat_writeent_ctx_t wr;
} fat_delchain_ctx_t;

/* Check whether a directory (given by its cluster) contains any real child
 * beyond '.'/'..'.  result: 1 = empty, 0 = non-empty, -1 = error/not-a-dir. */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t entries_left;
    uint32_t cur_sector;
    uint32_t entries_total;
    uint32_t scan_index;
    int result; /* 1 empty, 0 non-empty */
    fat_lfn_t lfn;
} fat_dirempty_ctx_t;

/* Core create: resolve parent, validate/allocate the short name, build the LFN
 * chain, find free slots, write the entries.  Inputs attr/cluster/size/... are
 * set before the first step; `out` receives the resolved entry. */
typedef struct {
    int cont;
    /* Inputs. */
    const char* path;
    int32_t source;
    uint8_t attr;
    uint16_t cluster;
    uint32_t size;
    uint8_t fail_if_exists;
    /* Resolved-parent geometry. */
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t entry_limit;
    uint8_t root;
    /* Name handling. */
    char name[FAT_MAX_PATH];
    uint32_t name_len;
    uint8_t short_name[11];
    uint8_t exact_short;
    uint32_t ordinal; /* alias-search cursor */
    uint32_t lfn_count;
    uint32_t needed_entries;
    uint8_t checksum;
    uint32_t slot_entry; /* first free slot */
    uint32_t i;          /* LFN write cursor */
    uint8_t entry[32];
    fat_dir_entry_info_t found; /* existence pre-check result */
    /* Sub-machines. */
    fat_resolve_parent_ctx_t parent;
    fat_dir_scan_ctx_t scan;
    fat_shortscan_ctx_t shortscan;
    fat_findslots_ctx_t findslots;
    fat_writeent_ctx_t wr;
} fat_create_ctx_t;

/* Create a directory: allocate a cluster, write EOC, init '.'/'..' + zero the
 * cluster, then create the directory entry via an embedded fat_create_ctx_t. */
typedef struct {
    int cont;
    const char* path;
    int32_t source;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint8_t root;
    char name[FAT_MAX_PATH];
    uint16_t cluster;
    uint16_t parent_cluster;
    uint32_t cluster_lba;
    uint32_t sector; /* zero-fill cursor */
    uint8_t entry[32];
    fat_resolve_parent_ctx_t parent;
    fat_findfree_ctx_t findfree;
    fat_fatent_ctx_t fatent;
    fat_create_ctx_t create;
} fat_mkdir_ctx_t;

/* Unlink a file / remove a directory.  is_rmdir selects the rmdir guards. */
typedef struct {
    int cont;
    const char* path;
    int32_t source;
    uint8_t is_rmdir;
    uint32_t entry_index;
    fat_dir_entry_info_t entry;
    fat_resolve_ctx_t resolve;
    fat_dirempty_ctx_t empty;
    fat_freechain_ctx_t freechain;
    fat_delchain_ctx_t delchain;
} fat_remove_ctx_t;

/* --- Coroutine sub-machine contexts (fat_dir.c, navigation side). --- */

/* READDIR: stream the entries of the CURRENT directory (root region when
 * mnt->cwd_root, else the cwd subdir at mnt->dir_lba/dir_sectors) to the
 * requesting endpoint.  cur_root latches the region kind at the first step so a
 * concurrent CHDIR cannot shift the target mid-scan; the loop cursors and the
 * LFN accumulator survive the per-sector yields. */
typedef struct {
    int cont;
    uint8_t cur_root;       /* 1 = scanning the root region */
    uint32_t base_lba;      /* first LBA of the directory being listed */
    uint32_t dir_sectors;   /* span in sectors */
    uint32_t entries_left;  /* entries still to inspect */
    uint32_t cur_sector;    /* sector cursor within the run */
    uint32_t entries_total; /* entries in the sector currently staged */
    uint32_t scan_index;    /* entry cursor within the sector */
    fat_lfn_t lfn;          /* LFN accumulation across entries */
} fat_readdir_ctx_t;

/* CHDIR: navigate op->dir_name (relative to the cwd unless it starts with '/')
 * one component at a time, descending into each matched subdirectory.  The
 * path/name buffers and the running (root,cluster) target survive the per-sector
 * yields; the per-directory scan cursors track the current sector being read. */
typedef struct {
    int cont;
    char path[32];          /* working copy of the target (from dir_name) */
    uint32_t pos;           /* cursor into path[] */
    char name[16];          /* current component being resolved */
    uint8_t root;           /* running target: is-root flag */
    uint16_t cluster;       /* running target: directory cluster */
    uint32_t dir_lba;       /* first LBA of the directory being scanned */
    uint32_t dir_sectors;   /* span in sectors */
    uint32_t entries_left;  /* entries still to inspect in this directory */
    uint32_t cur_sector;    /* sector cursor within the run */
    uint32_t entries_total; /* entries in the sector currently staged */
    uint32_t scan_index;    /* entry cursor within the sector */
    int next;               /* fat_chdir_next_component result carried out */
    fat_lfn_t lfn;          /* LFN accumulation across entries */
} fat_chdir_ctx_t;

/* --- Coroutine sub-machine contexts (fat_file.c, open-file I/O side). --- */

/* Read-modify-write the 4-byte size field (bytes 28..31) of a file's directory
 * entry.  Inputs: file (target open file) + size are latched by the caller. */
typedef struct {
    int cont;
    fat_open_file_t* file;
    uint32_t size;
} fat_storesize_ctx_t;

/* Read-modify-write the 2-byte first-cluster field (bytes 26..27) of a file's
 * directory entry.  Inputs: file + cluster latched by the caller. */
typedef struct {
    int cont;
    fat_open_file_t* file;
    uint16_t cluster;
} fat_storecluster_ctx_t;

/* Reposition an open file to an absolute offset (walks the cluster chain when
 * the offset spans past the first cluster).  Inputs: file, offset, limit. */
typedef struct {
    int cont;
    fat_open_file_t* file;
    uint32_t offset;
    uint32_t limit;
    uint32_t cluster_skip; /* clusters left to hop */
    fat_chain_ctx_t step;  /* successor lookup during the walk */
} fat_reposition_ctx_t;

/* Append one freshly-allocated cluster to a file's chain (find free -> write
 * EOC -> link/first-cluster -> bump capacity).  Inputs: file. */
typedef struct {
    int cont;
    fat_open_file_t* file;
    uint16_t new_cluster;
    uint16_t last_cluster;
    uint16_t end_marker;
    fat_findfree_ctx_t findfree;  /* free-cluster search */
    fat_fatent_ctx_t fatent;      /* EOC / link writes */
    fat_storecluster_ctx_t store; /* first-cluster patch */
    fat_chainwalk_ctx_t walk;     /* last-cluster-in-chain */
} fat_append_ctx_t;

/* Grow a file's capacity to >= min_size (append clusters, then reposition to the
 * saved offset).  Inputs: file, min_size. */
typedef struct {
    int cont;
    fat_open_file_t* file;
    uint32_t min_size;
    uint32_t saved_offset;
    fat_append_ctx_t append;
    fat_reposition_ctx_t repos;
} fat_ensurecap_ctx_t;

/* ---------------------------------------------------------------------------
 * Per-operation context.  Holds everything the original driver kept in file-
 * scope globals, one instance per in-flight request, so ops interleave.  Field
 * groups are annotated with the op(s)/sub-machine that use them.  A step
 * function reads/writes only its own ctx (plus the shared block buffer, and
 * only during a completion step — never across a fair yield).
 * ------------------------------------------------------------------------- */
typedef struct fat_op_ctx {
    uint8_t in_use;
    fat_op_t op;
    int cont;    /* op-level coroutine resume point (fat_co.h) */
    int32_t err; /* WASMOS_ERR_FS_* to report on FAT_R_ERR */

    /* Request identity (was g_fs_req). */
    int32_t type;
    int32_t arg0, arg1, arg2, arg3;
    int32_t source;
    int32_t request_id;

    /* Response staging (was g_fs_resp_*). */
    uint8_t resp_override;
    int32_t resp_arg0;
    int32_t resp_arg1;

    /* (The single outstanding block request + staged sector live in fat_block_t,
     * not here — there is only one active op, so that state is per-buffer.) */

    /* Path/name working buffers. */
    char path[FAT_MAX_PATH];      /* raw client path */
    char fat_path[FAT_MAX_PATH];  /* vfs-translated path */
    char component[FAT_MAX_PATH]; /* current path component during traversal */
    char name[FAT_MAX_PATH];      /* leaf name (parent-dir resolve) */

    /* Directory scan / path-resolution sub-machines (fat_dir.c). */
    fat_resolve_ctx_t resolve;       /* path -> entry (open/stat/read) */
    fat_resolve_parent_ctx_t parent; /* path -> parent dir + leaf (create/delete) */
    fat_dir_scan_ctx_t scan;         /* direct directory scan (READDIR) */

    /* Directory mutation sub-machines (fat_dir.c, mutation side). */
    fat_create_ctx_t create; /* create file/dir entry (MKDIR reuses via mkdir) */
    fat_mkdir_ctx_t mkdir;   /* MKDIR */
    fat_remove_ctx_t remove; /* UNLINK / RMDIR */

    /* Cluster-chain / allocation sub-machines (fat_alloc.c). */
    fat_chain_ctx_t chain;         /* file-I/O chain follow */
    fat_chainwalk_ctx_t chainwalk; /* capacity / last-cluster walk */
    fat_findfree_ctx_t findfree;   /* free-cluster search */
    fat_fatent_ctx_t fatent;       /* direct FAT-entry writes */

    /* Open / read / write cursors. */
    int32_t fd;
    uint32_t done;                   /* bytes copied so far (read/write) */
    uint32_t requested;              /* bytes requested (read/write) */
    uint32_t target_end;             /* offset+requested (write capacity growth) */
    uint32_t old_size;               /* file size snapshot for zero-fill decisions */
    uint32_t io_sector_offset;       /* within-sector byte offset (read/write loop) */
    uint32_t io_chunk;               /* bytes copied this loop iteration (read/write) */
    fat_dir_entry_info_t open_entry; /* resolved entry for OPEN */

    /* Open-file I/O sub-machines (fat_file.c). */
    fat_storesize_ctx_t storesize;       /* dir-entry size RMW */
    fat_storecluster_ctx_t storecluster; /* dir-entry first-cluster RMW */
    fat_reposition_ctx_t repos;          /* absolute seek / chain walk */
    fat_append_ctx_t append;             /* append one cluster */
    fat_ensurecap_ctx_t ensurecap;       /* grow capacity for a write */
    fat_chainwalk_ctx_t capwalk;         /* OPEN capacity chain walk */

    /* Directory navigation dispatch (CHDIR / READDIR): unpacked leaf name. */
    char dir_name[16];

    /* Directory navigation sub-machines (fat_dir.c, navigation side). */
    fat_readdir_ctx_t readdir; /* READDIR streaming scan */
    fat_chdir_ctx_t chdir;     /* CHDIR component walk */

    /* CHDIR working state (fat_dir.c).  Was the g_chdir_ globals. */
    char chdir_path[32];
    uint32_t chdir_pos;
    char chdir_name[16];
    uint16_t chdir_cluster;
    uint8_t chdir_root;
    uint32_t chdir_dir_lba;
    uint32_t chdir_dir_sectors;
} fat_op_ctx_t;

#endif /* FS_FAT_FAT_TYPES_H */
