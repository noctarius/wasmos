/* fat_types.h - shared types & constants for the FAT backend reactor.
 *
 * The driver is a single-threaded event reactor: every filesystem request
 * becomes a fat_op_ctx_t whose op-specific state machine advances one
 * block-I/O boundary at a time (see fat_block.h).  This header holds the
 * on-disk FAT structures, the per-op context, and the reactor's small
 * vocabulary of enums.  No block I/O or global mutable state lives here. */
#ifndef FS_FAT_FAT_TYPES_H
#define FS_FAT_FAT_TYPES_H

#include <stdint.h>

/* FAT_SECTOR_SIZE is the assumed 512-byte sector; FAT_MAX_SECTOR_BYTES is what
 * the staging buffer is actually sized for, so a volume declaring a larger
 * bytes_per_sector in its BPB is still readable up to 4096. FAT_LFN_MAX is the
 * FAT specification's long-name limit in characters, excluding the NUL the
 * accumulator adds. FAT_MAX_OPEN_FILES bounds the whole driver's open-file table
 * across every client (fd = index + 3, so the highest fd is 18), and
 * FAT_MAX_PATH bounds a single path INCLUDING its NUL -- a longer path is
 * refused rather than truncated. */
#define FAT_SECTOR_SIZE 512u
#define FAT_MAX_SECTOR_BYTES 4096u
#define FAT_LFN_MAX 255u
#define FAT_MAX_OPEN_FILES 16u
#define FAT_MAX_PATH 128u

/* open() flag bits (mirror of the libc O_* subset the FS ABI forwards). */
#define FAT_OPEN_APPEND 0x0008
#define FAT_OPEN_CREAT 0x0040
#define FAT_OPEN_TRUNC 0x0200

/* Reactor sizing.  FAT_MAX_INFLIGHT bounds both the op-context pool and the FIFO
 * of ops parked waiting for the block buffer: fs_fat.c sizes both arrays from it,
 * so a queue push can never outrun the pool that gates it.  The single shared
 * block buffer serializes physical I/O, so ops beyond a handful only hide request
 * latency and add no throughput.
 * TODO: FAT_IO_QUEUE_CAP has no user; fs_fat.c sizes the FIFO from
 * FAT_MAX_INFLIGHT directly. */
#define FAT_MAX_INFLIGHT 8u
#define FAT_IO_QUEUE_CAP FAT_MAX_INFLIGHT

/* IPC send retry budget for streamed READDIR output when the client endpoint is
 * transiently full.
 *
 * IPC_ERR_FULL restates the kernel's transport code (see the IPC_ERR_* enum in
 * the kernel's ipc.h) because this driver cannot include kernel headers; the two
 * must move together. A full endpoint is ordinary backpressure from a client
 * that has not drained its stream yet, not a failure, which is why the sender
 * retries instead of aborting the listing. The budget is a bound, not a
 * timeout: exhausting it means the client is not consuming at all. */
#define IPC_ERR_FULL (-3)
#define FAT_STREAM_SEND_RETRIES 8192

/* Bring-up state of the driver as a whole. WAIT means the mount coroutine has
 * block I/O outstanding; FAILED is terminal for the volume -- it is not retried,
 * and every subsequent request fails rather than re-probing. */
typedef enum { FAT_BOOT_INIT = 0, FAT_BOOT_WAIT, FAT_BOOT_READY, FAT_BOOT_FAILED } fat_boot_phase_t;

/* FAT width, decided from the cluster count during mount. FAT_TYPE_32 is
 * recognised as a value but the allocation paths only support 12 and 16: the
 * end-of-chain marker helper returns 0 for anything else, which callers treat as
 * unsupported. */
typedef enum { FAT_TYPE_UNKNOWN = 0, FAT_TYPE_12, FAT_TYPE_16, FAT_TYPE_32 } fat_type_t;

/* Mount namespace tag. This backend serves exactly one volume, so BOOT is the
 * only member; it exists so the cwd records which mount it belongs to. */
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
    FAT_OP_CHDIR,   /* change directory      (fat_dir.c) */
    FAT_OP_RENAME   /* rename/move an entry  (fat_dir.c) */
} fat_op_t;

/* Result of advancing an op's state machine one step. */
typedef enum {
    FAT_R_DONE = 0, /* op complete: send FS_IPC_RESP using ctx->resp_* */
    FAT_R_WAIT = 1, /* op submitted block I/O: resume on completion */
    FAT_R_ERR = 2   /* op failed: send FS_IPC_ERROR using ctx->err (an WASMOS_ERR_FS_*) */
} fat_r_t;

/* The FAT BIOS Parameter Block, exactly as it sits at offset 0 of a volume boot
 * sector -- field order and widths mirror the specification and must not be
 * reordered. All multi-byte fields are little-endian on disk, which matches x86,
 * so the struct is read by overlaying it on the sector rather than by parsing.
 *
 * The pairs exist because FAT12/16 and FAT32 disagree: total_sectors_16 and
 * fat_size_16 are zero on a volume large enough to need total_sectors_32, and
 * `ext` holds whichever extended block the volume type defines. root_entry_count
 * is zero on FAT32, where the root directory is an ordinary cluster chain. */
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

/* One 16-byte MBR partition-table entry, as found at offset 0x1BE of LBA 0.
 * Probed only when LBA 0 does not parse as a BPB. The CHS fields are legacy and
 * ignored; `lba_start` is what the mount uses, and it becomes the volume's
 * boot_lba so every later LBA is relative to it. `type` 0 means an unused slot. */
typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t sectors;
} fat_mbr_entry_t;

/* An open file descriptor (fd = index + 3).
 *
 * `owner` is the endpoint that opened it: a slot belongs to that client and
 * lookups by another endpoint miss, so an fd is not transferable. `flags` is the
 * FAT_OPEN_* subset the client passed.
 *
 * The cursor is split across four fields that must stay consistent: `offset` is
 * the authoritative byte position, and current_cluster / current_sector /
 * file_lba are its derived location on disk. Only the reposition coroutine may
 * move `offset` across a cluster boundary, because doing so requires walking the
 * chain; fat_set_open_file_offset covers the cheap in-first-cluster case and
 * refuses the rest.
 *
 * `size` is the file's length as recorded in its directory entry, while
 * `capacity` is the space its cluster chain already covers, which is always >=
 * size and is what a write compares against before growing the chain. The three
 * dir_* fields locate the directory entry itself, so a size or first-cluster
 * change can be written back without re-resolving the path -- and so the
 * unlink path can tell whether an entry it is about to remove is open. */
typedef struct {
    uint8_t in_use;
    int32_t owner;
    int32_t flags;
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t current_sector;
    uint32_t file_lba;
    uint32_t size;
    uint32_t capacity;
    uint32_t offset;
    uint32_t dir_lba;
    uint32_t dir_sector;
    uint32_t dir_index;
} fat_open_file_t;

/* A resolved directory entry (result of a directory scan).
 *
 * `valid` is the hit/miss flag: a scan that runs off the end of a directory
 * leaves it 0 and still reports success, so callers must test it rather than
 * treating FAT_R_DONE as a match. `attr` is the raw FAT attribute byte (0x10 =
 * directory). `cluster` is the entry's first cluster, 0 for an empty file. The
 * dir_* triple locates the 32-byte entry PHYSICALLY -- LBA of the run it was
 * found in, sector within that run, and entry index within the sector -- which
 * is what lets a later write patch the entry in place, and what the open-file
 * table records.
 *
 * `dir_entry_index` locates it LOGICALLY instead: the entry's index counted
 * across the directory's whole cluster chain, which is the form
 * fat_write_dir_entry and fat_delete_dir_entry_chain address. The two are the
 * same number only in a single-cluster directory, which is why reconstructing
 * one from the other (`dir_sector * eps + dir_index`) was wrong as soon as
 * directories spanned clusters. `dir_first_cluster`/`dir_root` say which chain
 * that index belongs to. */
typedef struct {
    uint8_t valid;
    uint8_t attr;
    uint32_t cluster;
    uint32_t size;
    uint32_t dir_lba;
    uint32_t dir_sector;
    uint32_t dir_index;
    uint32_t dir_entry_index;   /* chain-relative */
    uint32_t dir_first_cluster; /* the directory's first cluster */
    uint8_t dir_root;           /* the entry lives in the fixed root region */
} fat_dir_entry_info_t;

/* Long-file-name accumulation state, gathered across the LFN entries that
 * precede a short entry during a directory scan.  One per scan context, so
 * scans cannot corrupt each other's names. */
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
    uint32_t cluster;     /* entry index */
    uint32_t value;       /* read result (28-bit on FAT32) */
    uint32_t write_value; /* value to store (write) */
    uint8_t lo, hi;       /* the entry's two bytes (FAT12/16) */
    uint8_t b2, b3;       /* the upper two bytes (FAT32) */
    uint32_t copy_idx;    /* FAT copy being written */
    uint32_t fat_offset;  /* byte offset of the entry within a FAT copy */
    uint32_t fsinfo_lba;  /* carried across the FSInfo read->write yield */
    uint32_t old_value;   /* what the entry held, for FSInfo free accounting */
    uint32_t fat_lba;     /* base lba of the entry's first byte (this copy) */
    uint32_t sector_offset;
} fat_fatent_ctx_t;

/* Resolve one cluster's successor.  next == 0 means end-of-chain. */
typedef struct {
    int cont;
    uint32_t cluster;
    uint32_t next;
    fat_fatent_ctx_t ent;
} fat_chain_ctx_t;

/* Walk a cluster chain to its end (used for capacity and last-cluster). */
typedef struct {
    int cont;
    uint32_t cluster; /* cursor */
    uint32_t last;    /* last cluster reached */
    uint32_t hops;    /* clusters visited */
    fat_chain_ctx_t step;
} fat_chainwalk_ctx_t;

/* Scan the FAT for the first free cluster. */
typedef struct {
    int cont;
    uint32_t cursor; /* current cluster index */
    uint32_t total;  /* total data clusters */
    uint32_t result; /* found free cluster */
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
    uint32_t cur_cluster;   /* chain cursor: moves as the scan hops */
    uint32_t first_cluster; /* latched at entry; what a hit reports */
    uint8_t cur_root;
    /* Scan cursors.  `entry_limit` budgets ONE cluster run and `entries_left` is
     * refilled from it at each hop, so `hops` -- not the entry budget -- is what
     * bounds the walk. */
    uint32_t entries_left;
    uint32_t cur_sector;
    uint32_t scan_index;
    uint32_t entries_total;
    uint32_t hops; /* clusters visited, against a cyclic chain */
    fat_lfn_t lfn;
    fat_dir_entry_info_t found;
    fat_chain_ctx_t chain;
} fat_dir_scan_ctx_t;

/* Read a directory's parent from its on-disk '..' entry.  `out_parent` is 0 when
 * the parent IS the root, which is what the specification stores there. */
typedef struct {
    int cont;
    uint32_t cluster; /* the directory whose parent is wanted */
    uint32_t out_parent;
} fat_dotdot_ctx_t;

/* Resolve a path to its directory entry (walks components, descends subdirs). */
typedef struct {
    int cont;
    const char* path;
    int32_t source;
    uint32_t pos;
    uint8_t cur_root;
    uint32_t cur_cluster;
    uint32_t cur_lba;
    uint32_t cur_sectors;
    int comp_rc;
    int has_more;
    char component[FAT_MAX_PATH];
    fat_dir_entry_info_t found;
    fat_dir_scan_ctx_t scan;
    fat_dotdot_ctx_t dotdot; /* '..' resolved through the on-disk entry */
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
    uint32_t cur_cluster;
    uint32_t cur_lba;
    uint32_t cur_sectors;
    int comp_rc;
    int has_more;
    char component[FAT_MAX_PATH];
    char name[FAT_MAX_PATH];
    fat_dir_entry_info_t found;
    fat_dir_scan_ctx_t scan;
    fat_dotdot_ctx_t dotdot; /* '..' resolved through the on-disk entry */
} fat_resolve_parent_ctx_t;

/* --- Coroutine sub-machine contexts (fat_dir.c, mutation side). --- */

/* Free every cluster of a chain (walk it, writing 0 into each FAT entry).  The
 * successor is resolved BEFORE the entry is cleared; clearing first would lose
 * the rest of the chain. */
typedef struct {
    int cont;
    uint32_t cluster;      /* current cluster being freed */
    uint32_t next;         /* resolved successor (before clearing) */
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
    uint32_t cur_cluster; /* chain cursor; ignored when cur_root */
    uint8_t cur_root;
    uint32_t entries_left;
    uint32_t cur_sector;
    uint32_t entries_total;
    uint32_t scan_index;
    uint32_t hops; /* clusters visited, against a cyclic chain */
    fat_chain_ctx_t chain;
    int result; /* 1 present, 0 absent */
} fat_shortscan_ctx_t;

/* Find `needed` consecutive free directory slots; out_entry gets the first
 * slot's flat entry index.  result: 0 = found, -1 = none. */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t entry_limit; /* entries per cluster run (per the root region when root) */
    uint32_t first_cluster;
    uint8_t root;
    uint32_t needed;
    uint32_t run;
    uint32_t run_start;
    uint32_t entry;  /* chain-relative entry cursor */
    uint32_t sector; /* derived (crosses the per-sector yield) */
    uint32_t base;   /* chain-relative index of the current cluster's first entry */
    uint32_t cur_cluster;
    uint32_t cur_lba;
    uint32_t hops; /* clusters visited, against a cyclic chain */
    uint32_t grow_cluster;
    uint32_t zero_sector;
    uint8_t grew; /* a cluster was appended to satisfy the request */
    uint32_t out_entry;
    int result; /* 0 found, -1 none */
    fat_chain_ctx_t chain;
    fat_findfree_ctx_t findfree;
    fat_fatent_ctx_t fatent;
} fat_findslots_ctx_t;

/* Resolve a directory's CHAIN-RELATIVE entry index to a physical location.
 *
 * An entry index counts across the whole directory, not within one cluster, so
 * resolving it means walking the chain -- `dir_lba + index / entries_per_sector`
 * is only valid inside one contiguous run, and the next cluster of a chain is
 * not the next LBA.  The fixed FAT12/16 root IS such a run, which is why `root`
 * short-circuits the walk. */
typedef struct {
    int cont;
    uint32_t first_cluster; /* directory's first cluster; ignored when root */
    uint8_t root;
    uint32_t entry_index; /* input: chain-relative */
    uint32_t out_lba;     /* output: the sector holding it */
    uint32_t out_index;   /* output: entry index within that sector */
    uint32_t cluster_skip;
    uint32_t cur_cluster;
    fat_chain_ctx_t chain;
} fat_dirloc_ctx_t;

/* Read-modify-write one 32-byte directory entry into its sector.  `entry_index`
 * is chain-relative and resolved through fat_dirloc_ctx_t, so the caller must
 * also supply the directory's first cluster (or set root). */
typedef struct {
    int cont;
    uint32_t dir_lba; /* the fixed root region's first LBA when root */
    uint32_t first_cluster;
    uint8_t root;
    uint32_t entry_index;
    uint32_t sector; /* derived; crosses the read->write yield */
    uint32_t index;  /* derived; crosses the read->write yield */
    uint8_t entry[32];
    fat_dirloc_ctx_t loc;
} fat_writeent_ctx_t;

/* Mark a short entry + its preceding LFN entries as deleted (0xE5). */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t first_cluster; /* the directory's first cluster; ignored when root */
    uint8_t root;
    uint32_t entry_index; /* cursor: the short entry, then walks back over LFNs */
    uint32_t prev_index;
    uint32_t sector; /* derived; crosses the read yield */
    uint32_t index;  /* derived; crosses the read yield */
    uint8_t tombstone[32];
    fat_writeent_ctx_t wr;
    fat_dirloc_ctx_t loc;
} fat_delchain_ctx_t;

/* Check whether a directory (given by its first LBA + span) contains any real
 * child beyond '.'/'..'.  result: 1 = empty, 0 = non-empty; an I/O fault leaves
 * result untouched and propagates as FAT_R_ERR instead. */
typedef struct {
    int cont;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t cur_cluster; /* chain cursor: the directory's first cluster on entry */
    uint32_t entries_left;
    uint32_t cur_sector;
    uint32_t entries_total;
    uint32_t scan_index;
    uint32_t hops; /* clusters visited, against a cyclic chain */
    fat_chain_ctx_t chain;
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
    uint32_t cluster;
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
    uint32_t cluster;
    uint32_t parent_cluster;
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

/* Rename/move an entry: re-point a name at an existing cluster chain without
 * touching the data.  The new entry is written BEFORE the old one is removed,
 * so an interruption leaves the chain reachable under two names rather than
 * under none -- recoverable, where the other order loses the file.
 *
 * `entry` is the source's resolved directory entry, captured before anything is
 * written because the create step restages the block buffer. */
typedef struct {
    int cont;
    const char* old_path;
    const char* new_path;
    int32_t source;
    fat_dir_entry_info_t entry; /* the source entry, latched */
    uint32_t entry_index;       /* its flat index within its directory run */
    uint32_t dotdot_cluster;    /* what '..' must hold after a directory moves */
    uint8_t is_dir;
    uint8_t entry_buf[32];
    fat_resolve_ctx_t resolve;
    fat_resolve_ctx_t dest_probe;
    fat_resolve_parent_ctx_t old_parent;
    fat_create_ctx_t create;
    fat_delchain_ctx_t delchain;
    fat_writeent_ctx_t wr;
} fat_rename_ctx_t;

/* --- Coroutine sub-machine contexts (fat_dir.c, navigation side). --- */

/* READDIR: stream the entries of the CURRENT directory (root region when
 * mnt->cwd_root, else the cwd subdir at mnt->dir_lba/dir_sectors) to the
 * requesting endpoint.  cur_root/base_lba/dir_sectors latch the target region at
 * the first step, so the scan keeps listing the directory it started on instead
 * of re-reading mnt after every yield; the loop cursors and the LFN accumulator
 * survive the per-sector yields. */
typedef struct {
    int cont;
    uint8_t cur_root;       /* 1 = scanning the root region */
    uint32_t base_lba;      /* first LBA of the directory being listed */
    uint32_t dir_sectors;   /* span in sectors */
    uint32_t entries_left;  /* entries still to inspect in the current run */
    uint32_t entry_budget;  /* entries per run, refilled at each cluster hop */
    uint32_t cur_cluster;   /* chain cursor; ignored when cur_root */
    uint32_t cur_sector;    /* sector cursor within the run */
    uint32_t entries_total; /* entries in the sector currently staged */
    uint32_t scan_index;    /* entry cursor within the sector */
    uint32_t hops;          /* clusters visited, against a cyclic chain */
    fat_chain_ctx_t chain;
    fat_lfn_t lfn; /* LFN accumulation across entries */
} fat_readdir_ctx_t;

/* CHDIR: navigate op->dir_name (relative to the cwd unless it starts with '/')
 * one component at a time, descending into each matched subdirectory.  The
 * path/name buffers and the running (root,cluster) target survive the yields;
 * the per-level lookup is delegated to fat_find_in_dir, so this carries no scan
 * cursors of its own. */
typedef struct {
    int cont;
    char path[32];      /* working copy of the target (from dir_name) */
    uint32_t pos;       /* cursor into path[] */
    char name[16];      /* current component being resolved */
    uint8_t root;       /* running target: is-root flag */
    uint32_t cluster;   /* running target: directory cluster */
    uint8_t root_probe; /* fat_root_origin output, latched at entry */
    uint32_t root_cluster_probe;
    uint32_t root_lba_probe;
    uint32_t root_sectors_probe;
    int next;                /* fat_chdir_next_component result carried out */
    fat_dir_scan_ctx_t scan; /* the shared directory scan, one level at a time */
    fat_dotdot_ctx_t dotdot; /* '..' resolved through the on-disk entry */
} fat_chdir_ctx_t;

/* --- Coroutine sub-machine contexts (fat_file.c, open-file I/O side). --- */

/* Read-modify-write the 4-byte size field (bytes 28..31) of a file's directory
 * entry.  Inputs: file (target open file) + size are latched by the caller. */
typedef struct {
    int cont;
    fat_open_file_t* file;
    uint32_t size;
} fat_storesize_ctx_t;

/* Read-modify-write the first-cluster field of a file's directory entry: the
 * low half at bytes 26..27 and, on FAT32, the high half at bytes 20..21.
 * Inputs: file + cluster latched by the caller. */
typedef struct {
    int cont;
    fat_open_file_t* file;
    uint32_t cluster;
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
    uint32_t new_cluster;
    uint32_t last_cluster;
    uint32_t end_marker;
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
    uint32_t prev_capacity; /* progress check: an append must raise capacity */
    fat_append_ctx_t append;
    fat_reposition_ctx_t repos;
} fat_ensurecap_ctx_t;

/* ---------------------------------------------------------------------------
 * Per-operation context.  Holds all per-request state, one instance per
 * accepted request; the reactor keeps a pool of them so requests can queue up
 * behind the one it is driving.  No file-scope globals.  Field groups are
 * annotated with the op(s)/sub-machine that use them.  A step function
 * reads/writes only its own ctx plus the shared block buffer.  The staged sector
 * survives that op's yields because the reactor drives one op to completion
 * before activating the next; across ops the buffer holds whatever the previous
 * one left, which is safe only because loaded_lba tags it (fat_block.h).
 * ------------------------------------------------------------------------- */
typedef struct fat_op_ctx {
    uint8_t in_use;
    fat_op_t op;
    int cont;    /* op-level coroutine resume point (fat_co.h) */
    int32_t err; /* WASMOS_ERR_FS_* to report on FAT_R_ERR */

    /* Request identity (the forwarded FS_IPC_* message). */
    int32_t type;
    int32_t arg0, arg1, arg2, arg3;
    int32_t source;
    int32_t request_id;

    /* Response staging: the reactor sends resp_arg0/1 only when resp_override is
     * set, otherwise FS_IPC_RESP carries zeros. */
    uint8_t resp_override;
    int32_t resp_arg0;
    int32_t resp_arg1;

    /* (The single outstanding block request + staged sector live in fat_block_t,
     * not here — there is only one active op, so that state is per-buffer.) */

    /* Path/name working buffers.  Traversal keeps its own component and leaf
     * copies inside the resolve / resolve-parent contexts.
     * TODO: component[] and name[] have no reader; they only cost op-ctx bytes. */
    char path[FAT_MAX_PATH];     /* raw client path */
    char fat_path[FAT_MAX_PATH]; /* vfs-translated path */
    /* RENAME's destination.  A second pair rather than a reuse of the above,
     * because fat_rename_path holds pointers to BOTH for the length of the
     * operation. */
    char rename_path[FAT_MAX_PATH];
    char fat_rename_path[FAT_MAX_PATH];
    char component[FAT_MAX_PATH];
    char name[FAT_MAX_PATH];

    /* Directory scan / path-resolution sub-machines (fat_dir.c).  Only `resolve`
     * is driven from the op level (OPEN and STAT).
     * TODO: `parent` and `scan` have no reader — create/mkdir/remove and READDIR
     * drive their own embedded contexts instead. */
    fat_resolve_ctx_t resolve; /* path -> entry */
    fat_resolve_parent_ctx_t parent;
    fat_dir_scan_ctx_t scan;

    /* Directory mutation sub-machines (fat_dir.c, mutation side). */
    fat_create_ctx_t create; /* create file/dir entry (MKDIR reuses via mkdir) */
    fat_mkdir_ctx_t mkdir;   /* MKDIR */
    fat_remove_ctx_t remove; /* UNLINK / RMDIR */
    fat_rename_ctx_t rename; /* RENAME */

    /* Cluster-chain / allocation sub-machines (fat_alloc.c).  Only `chain` is
     * driven from the op level (the read/write loop's chain follow).
     * TODO: chainwalk/findfree/fatent have no reader — append, mkdir and the
     * free-chain walk embed their own. */
    fat_chain_ctx_t chain;
    fat_chainwalk_ctx_t chainwalk;
    fat_findfree_ctx_t findfree;
    fat_fatent_ctx_t fatent;

    /* Open / read / write cursors. */
    int32_t fd;
    uint32_t done;             /* bytes copied so far (read/write) */
    uint32_t requested;        /* bytes requested (read/write) */
    uint32_t target_end;       /* offset+requested (write capacity growth) */
    uint32_t old_size;         /* file size snapshot for zero-fill decisions */
    uint32_t io_sector_offset; /* within-sector byte offset (read/write loop) */
    uint32_t io_chunk;         /* bytes copied this loop iteration (read/write) */
    /* Zero-copy read passthrough: whole sectors are landed by the block server
     * straight into the client's transfer buffer instead of being staged. The
     * reborrow is what gives that server the right to write there; it is taken
     * lazily (only if a whole sector actually comes up) and dropped in
     * fat_op_free, so no request can leak a grant. */
    /* Tri-state, and the values are not interchangeable: 0 = no borrow (the
     * initial value and what fat_op_free restores), > 0 = a live reborrow that
     * MUST be unborrowed, -1 = a reborrow was attempted and refused, so do not
     * retry for the remaining sectors of this request.  Teardown keys on > 0;
     * treating -1 as "none" would be harmless, but treating it as live would
     * unborrow a handle that was never taken. */
    int32_t zc_borrow;
    uint32_t io_run_sectors;         /* whole sectors in the current direct run */
    fat_dir_entry_info_t open_entry; /* resolved entry for OPEN */

    /* Open-file I/O sub-machines (fat_file.c).
     * TODO: `storecluster` and `append` have no reader at this level — the
     * append and ensure-capacity machines embed their own. */
    fat_storesize_ctx_t storesize; /* dir-entry size RMW */
    fat_storecluster_ctx_t storecluster;
    fat_reposition_ctx_t repos; /* absolute seek / chain walk */
    fat_append_ctx_t append;
    fat_ensurecap_ctx_t ensurecap; /* grow capacity for a write */
    fat_chainwalk_ctx_t capwalk;   /* OPEN capacity chain walk */

    /* Directory navigation dispatch (CHDIR / READDIR): unpacked leaf name. */
    char dir_name[16];

    /* Directory navigation sub-machines (fat_dir.c, navigation side). */
    fat_readdir_ctx_t readdir; /* READDIR streaming scan */
    fat_chdir_ctx_t chdir;     /* CHDIR component walk */

    /* TODO: CHDIR working state with no reader — fat_op_chdir keeps all of this
     * in the `chdir` sub-context above. */
    char chdir_path[32];
    uint32_t chdir_pos;
    char chdir_name[16];
    uint32_t chdir_cluster;
    uint8_t chdir_root;
    uint32_t chdir_dir_lba;
    uint32_t chdir_dir_sectors;
} fat_op_ctx_t;

#endif /* FS_FAT_FAT_TYPES_H */
