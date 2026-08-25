/* wfs_format.h - the WFS on-disk format.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md. Section numbers below cite it.
 *
 * Every structure here is written to and read from a block device, so its
 * layout IS the ABI. Three properties hold for all of them and are asserted at
 * the bottom of this file rather than left to review:
 *
 *   - Fixed-width types only. A `long`, a `size_t`, or a pointer is 4 bytes on
 *     wasm32 and 8 on x86_64, which would give the driver and any host-side
 *     tool different pictures of the same volume.
 *   - No implicit padding. Every hole is a named `reserved` field and is zero.
 *     The checksums (§13) are specified over whole structures, and a
 *     compiler-inserted hole that no writer sets makes the same image checksum
 *     differently depending on which tool wrote it.
 *   - Little-endian. wasm is little-endian by specification and x86_64 is
 *     little-endian, so no conversion happens on either. A big-endian host tool
 *     must swap on access.
 *
 * Block numbers are 64-bit on disk and carried as uint32_t inside the driver
 * (§22): host calls and IPC arguments are 32-bit, so a 64-bit block number
 * cannot cross either boundary as a scalar. A volume whose counts exceed
 * UINT32_MAX is refused at mount rather than silently truncated.
 */
#ifndef FS_WFS_WFS_FORMAT_H
#define FS_WFS_WFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* ---- identity ------------------------------------------------------------ */

/* Stored little-endian, so a hexdump of a volume shows "WFS1" and "WFSJ". */
#define WFS_MAGIC 0x31534657u         /* 'W' 'F' 'S' '1' */
#define WFS_JOURNAL_MAGIC 0x4A534657u /* 'W' 'F' 'S' 'J' */
#define WFS_EXTENT_NODE_MAGIC 0x5458u /* 'X' 'T' */

#define WFS_VERSION 1u

#define WFS_UUID_LEN 16u

/* ---- geometry (§2, §4) --------------------------------------------------- */

/* The primary superblock sits at a fixed BYTE offset. It cannot be addressed in
 * blocks: block_size is itself a superblock field, so no block unit exists
 * until this read completes (§4). Bytes 0..1023 are the reserved boot area. */
#define WFS_SUPER_OFFSET 1024u
#define WFS_SUPER_SIZE 1024u

#define WFS_BLOCK_SIZE_MIN 4096u
#define WFS_BLOCK_SIZE_MAX 16384u

/* One block of bitmap covers exactly one group's blocks, which is what fixes
 * this relation: block_size bytes hold 8 * block_size bits. Deriving the group
 * size rather than storing it freely is also what bounds a recovery scan for a
 * backup superblock to three candidate offsets per group (§5). */
#define WFS_BLOCKS_PER_GROUP(block_size) ((block_size) * 8u)

/* ---- superblock state (§4) ----------------------------------------------- */

/* Zero is not a valid state: an all-zero region must not read as a cleanly
 * unmounted volume, because that is the one value a partially written or
 * never-formatted device is most likely to hold. */
enum {
    WFS_STATE_CLEAN = 1u, /* unmounted cleanly; the journal holds nothing to replay */
    WFS_STATE_DIRTY = 2u, /* mounted for writing, or crashed while mounted */
    WFS_STATE_ERROR = 3u, /* an inconsistency was detected; mount read-only, run fsck */
};

/* ---- feature flags (§6) -------------------------------------------------- */

/* All three are INCOMPAT because each changes how an EXISTING structure is
 * read, not what is added beside it: an extent-mapped object, a journalled
 * volume, and an object holding data in its record are each misread rather than
 * merely unrecognized by a driver that ignores the flag. */
enum {
    WFS_FEATURE_INCOMPAT_EXTENTS = 1u << 0,
    WFS_FEATURE_INCOMPAT_JOURNAL = 1u << 1,
    WFS_FEATURE_INCOMPAT_INLINE_DATA = 1u << 2,
};

/* What this driver implements. A volume setting an INCOMPAT bit outside this
 * mask is refused; one setting an RO_COMPAT bit outside WFS_FEATURE_RO_COMPAT_SUPPORTED
 * mounts read-only (§6). */
#define WFS_FEATURE_INCOMPAT_SUPPORTED                                                             \
    (WFS_FEATURE_INCOMPAT_EXTENTS | WFS_FEATURE_INCOMPAT_JOURNAL | WFS_FEATURE_INCOMPAT_INLINE_DATA)
#define WFS_FEATURE_RO_COMPAT_SUPPORTED 0u
#define WFS_FEATURE_COMPAT_SUPPORTED 0u

/* ---- objects (§7, §25) --------------------------------------------------- */

enum {
    WFS_TYPE_FILE = 1u,
    WFS_TYPE_DIR = 2u,
    WFS_TYPE_SYMLINK = 3u,
    WFS_TYPE_DEVICE = 4u,
    WFS_TYPE_SPECIAL = 5u,
};

enum {
    /* The extents array holds the object's data directly (§7). */
    WFS_OBJ_INLINE_DATA = 1u << 0,
};

enum {
    WFS_OBJECT_INVALID = 0u, /* in a directory record, marks free space (§10) */
    WFS_OBJECT_ROOT = 1u,    /* the root directory */
    WFS_OBJECT_FIRST = 16u,  /* first allocatable id; 2..15 are reserved */
};

#define WFS_OBJECT_SIZE 256u
#define WFS_INLINE_EXTENTS 6u

/* Descent bound for an extent tree. An interior node's children are one level
 * shallower, so a descent that does not strictly decrease `depth` is a cycle.
 * Five is far past reach: at a 4096-byte block size an interior node holds 255
 * indices and a leaf 170 extents, so depth 3 already maps more blocks than a
 * uint32_t block number can address. */
#define WFS_EXTENT_MAX_DEPTH 5u

/* ---- group descriptors (§11) --------------------------------------------- */

#define WFS_GROUP_DESC_SIZE 64u

enum {
    WFS_GROUP_HAS_SUPER_BACKUP = 1u << 0,
};

/* ---- journal (§14) ------------------------------------------------------- */

enum {
    WFS_JOURNAL_DESCRIPTOR = 1u, /* names the destination of each image that follows */
    WFS_JOURNAL_COMMIT = 2u,     /* makes the transaction replayable */
    WFS_JOURNAL_REVOKE = 3u,     /* bars older images of the listed blocks from replay */
};

enum {
    WFS_JOURNAL_TARGET_LAST = 1u << 0,
};

/* ---- on-disk structures -------------------------------------------------- */

/* §4. Exactly 1024 bytes, spanning the whole region reserved to it, so the
 * checksum covers a fixed extent and later versions claim fields from
 * `reserved` without a format break. */
struct wfs_superblock {
    uint32_t magic;
    uint32_t version;

    uint32_t block_size;
    uint32_t blocks_per_group; /* 8 * block_size */

    uint64_t total_blocks;
    uint64_t total_objects;
    uint64_t free_blocks;
    uint64_t free_objects;

    uint64_t root_object_id;

    uint64_t group_table_start;
    uint64_t group_table_blocks;

    uint64_t object_table_start;
    uint64_t object_table_blocks;

    uint64_t bitmap_start;
    uint64_t bitmap_blocks;

    uint64_t journal_start;
    uint64_t journal_blocks;

    uint64_t generation; /* incremented on every superblock write; orders backups */

    uint32_t feature_compat;
    uint32_t feature_ro_compat;
    uint32_t feature_incompat;

    uint32_t state; /* WFS_STATE_* */

    uint8_t uuid[WFS_UUID_LEN]; /* volume identity; seeds every checksum */

    uint32_t checksum;
    uint32_t reserved[215]; /* zero; pads the structure to 1024 bytes */
};

/* §11. One per group, in group order. 64 bytes, so 64, 128, or 256 fit a block
 * exactly. The superblock's object_table_* and bitmap_* fields bound the whole
 * regions; each descriptor names its group's slice inside them. */
struct wfs_group_desc {
    uint64_t block_bitmap;  /* first block of this group's block bitmap */
    uint64_t object_bitmap; /* first block of this group's object bitmap */
    uint64_t object_table;  /* first block of this group's object table slice */

    uint32_t free_blocks;
    uint32_t free_objects;

    uint32_t flags; /* WFS_GROUP_HAS_SUPER_BACKUP */
    uint32_t checksum;

    uint32_t reserved[6]; /* zero; pads the descriptor to 64 bytes */
};

/* §9. `logical_block` and `physical_block` are block numbers, not byte offsets;
 * `length` is a count of blocks and is at least 1. `reserved` occupies what
 * would otherwise be implicit tail padding, so a checksum over an array of
 * extents covers fully defined records. */
struct wfs_extent {
    uint64_t logical_block;
    uint64_t physical_block;
    uint32_t length;
    uint32_t reserved; /* zero */
};

/* §7. Exactly 256 bytes, so 16, 32, or 64 records occupy a block with none
 * straddling a boundary and the object table is addressed by plain division.
 *
 * `mode` holds permission bits only; the object kind stays in `type` so a
 * reader validates it without masking a packed value. `btime` is the creation
 * time and is never updated, which is what distinguishes it from `ctime`.
 * Timestamps are unsigned nanoseconds since 1970-01-01T00:00:00Z; the platform
 * RTC reports whole seconds with no timezone, so a driver leaves the sub-second
 * part zero. */
struct wfs_object {
    uint64_t object_id;

    uint16_t type;  /* WFS_TYPE_* */
    uint16_t flags; /* WFS_OBJ_* */
    uint32_t mode;  /* permission bits only */

    uint32_t uid;
    uint32_t gid;

    uint64_t size;

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t btime;

    uint32_t link_count;
    uint32_t extent_count;

    struct wfs_extent extents[WFS_INLINE_EXTENTS];

    uint64_t extent_tree_block;

    uint32_t checksum;
    uint32_t reserved[7]; /* zero; growth room for xattrs, data-checksum root, snapshot id */
};

/* Bytes of an object record available to WFS_OBJ_INLINE_DATA: the extents array
 * holds the data directly, and `size` may not exceed this (§7, §20).
 *
 * A plain constant rather than a sizeof expression: sizeof yields size_t, and
 * every comparison against a 64-bit `size` then widens a size_t multiplication.
 * The assertion below ties it to the array it describes, so the two cannot
 * drift. */
#define WFS_INLINE_DATA_MAX 144u

/* §9. Header common to both extent-tree node kinds. The checksum is a header
 * field because C places a flexible array member last, so no field may follow a
 * node's record array. */
struct wfs_extent_header {
    uint16_t magic;    /* WFS_EXTENT_NODE_MAGIC */
    uint16_t depth;    /* 0 = leaf, > 0 = interior */
    uint16_t entries;  /* records in use */
    uint16_t capacity; /* records the block holds at this depth */
    uint32_t checksum;
    uint32_t reserved; /* zero */
};

/* §9. An interior record names the block of a child one level shallower.
 * Without it a tree could hold no more than its root and `depth` could never
 * exceed 0. */
struct wfs_extent_index {
    uint64_t logical_block; /* first logical block the child covers */
    uint64_t child_block;   /* physical block holding the child node */
};

struct wfs_extent_leaf {
    struct wfs_extent_header header; /* header.depth == 0 */
    struct wfs_extent records[];
};

struct wfs_extent_interior {
    struct wfs_extent_header header; /* header.depth > 0 */
    struct wfs_extent_index records[];
};

/* §10. `record_length` is the stride to the next entry and is a multiple of 8,
 * so every entry's `object_id` lands on its natural alignment. Records are
 * strided by `record_length` and never by `sizeof`: the fixed header is 12
 * bytes and `name` begins at offset 12, while sizeof rounds up to 16. No
 * on-disk record has a hole there. */
struct wfs_dir_entry {
    uint64_t object_id;
    uint16_t record_length;
    uint8_t name_length;
    uint8_t type;
    char name[];
};

#define WFS_DIR_ENTRY_HEADER 12u

/* §10. The last 16 bytes of every directory block. A directory block holds
 * entries rather than one structure, so there is no other field the checksum of
 * §13 could live in.
 *
 * Laid out as a directory record whose object_id is 0, so a scan that knows
 * nothing about it reads free space and skips it — the same rule that already
 * governs a removed entry. The four bytes a name would occupy hold the
 * checksum, which covers the whole block with those four zeroed, seeded with
 * the block's own number. */
struct wfs_dir_tail {
    uint64_t object_id;     /* 0 */
    uint16_t record_length; /* WFS_DIR_TAIL_SIZE */
    uint8_t name_length;    /* 0 */
    uint8_t type;           /* WFS_DIR_TAIL_TYPE */
    uint32_t checksum;
};

#define WFS_DIR_TAIL_SIZE 16u
#define WFS_DIR_TAIL_TYPE 0xFFu

/* Longest name a record can carry: `name_length` is one byte. */
#define WFS_NAME_MAX 255u

/* Shortest legal record stride. A record is 12 bytes of header plus its name,
 * rounded up to 8, so nothing smaller can hold even a one-character name — and a
 * stride of 0 would make a scan of a block never terminate. */
#define WFS_DIR_RECORD_MIN 16u

/* Round a record length up to the 8-byte stride every directory record keeps,
 * so the next record's object_id lands on its natural alignment (§10). */
static inline uint32_t wfs_dir_record_length(uint32_t name_length) {
    return (WFS_DIR_ENTRY_HEADER + name_length + 7u) & ~7u;
}

/* Bytes of a directory block available to records, i.e. everything before the
 * tail. */
static inline uint32_t wfs_dir_usable_bytes(uint32_t block_size) {
    return block_size - WFS_DIR_TAIL_SIZE;
}

/* §14. The log tail. Recovery starts at first_sequence/first_block and
 * checkpointing advances it. */
struct wfs_journal_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size; /* equals superblock.block_size */
    uint32_t blocks;     /* journal length in blocks, including this one */
    uint64_t first_sequence;
    uint32_t first_block;
    uint32_t checksum;
};

/* §14. Every journal block opens with this. `sequence` is a monotonically
 * increasing transaction id and is what binds a descriptor, the images
 * following it, and its commit into one transaction; without it a scan sees a
 * flat run of blocks with no way to group them or to tell a live block from
 * stale content left by a wrap. */
struct wfs_journal_header {
    uint32_t magic; /* WFS_JOURNAL_MAGIC */
    uint32_t type;  /* WFS_JOURNAL_* */
    uint64_t sequence;
    uint32_t checksum;
    uint32_t reserved; /* zero */
};

struct wfs_journal_target {
    uint64_t target_block; /* filesystem block this image replaces */
    uint32_t flags;        /* WFS_JOURNAL_TARGET_LAST on the final target */
    uint32_t checksum;     /* CRC32C of the block image */
};

/* A descriptor is followed immediately by one block image per target, in the
 * order the targets are listed. */
struct wfs_journal_descriptor {
    struct wfs_journal_header header;
    struct wfs_journal_target targets[];
};

/* `target_count` lets recovery confirm that every image the transaction
 * promised is present before any of them is applied. */
struct wfs_journal_commit {
    struct wfs_journal_header header;
    uint32_t target_count;
    uint32_t reserved; /* zero */
};

/* §14, §18. Mandatory whenever a block stops being metadata. The journal
 * records block numbers, not what a block currently holds, so an older
 * committed image of a freed block stays replayable after the block has been
 * handed to a file; replaying it would overwrite live data with stale metadata. */
struct wfs_journal_revoke {
    struct wfs_journal_header header;
    uint32_t count;
    uint32_t reserved; /* zero */
    uint64_t blocks[];
};

/* ---- derived capacities -------------------------------------------------- */

static inline uint32_t wfs_objects_per_block(uint32_t block_size) {
    return block_size / WFS_OBJECT_SIZE;
}

static inline uint32_t wfs_group_descs_per_block(uint32_t block_size) {
    return block_size / WFS_GROUP_DESC_SIZE;
}

/* Records a node of each kind holds at this block size. A node's `capacity`
 * field must equal these, and is validated on read (§9). */
static inline uint32_t wfs_extent_leaf_capacity(uint32_t block_size) {
    return (uint32_t)((block_size - sizeof(struct wfs_extent_header)) / sizeof(struct wfs_extent));
}

static inline uint32_t wfs_extent_interior_capacity(uint32_t block_size) {
    return (uint32_t)((block_size - sizeof(struct wfs_extent_header)) /
                      sizeof(struct wfs_extent_index));
}

/* Bits of block bitmap in one block, which is one group's worth (§2, §12). */
static inline uint32_t wfs_bitmap_bits_per_block(uint32_t block_size) {
    return block_size * 8u;
}

/* ---- layout assertions --------------------------------------------------- */

/* These are the format. A change that moves a field or resizes a structure
 * fails the build here rather than producing volumes a previously built tool
 * cannot read. Sizes and offsets are identical for wasm32 and x86_64. */

_Static_assert(sizeof(struct wfs_superblock) == WFS_SUPER_SIZE, "superblock spans its region");
_Static_assert(offsetof(struct wfs_superblock, total_blocks) == 16, "sb.total_blocks");
_Static_assert(offsetof(struct wfs_superblock, generation) == 120, "sb.generation");
_Static_assert(offsetof(struct wfs_superblock, uuid) == 144, "sb.uuid");
_Static_assert(offsetof(struct wfs_superblock, checksum) == 160, "sb.checksum");

_Static_assert(sizeof(struct wfs_group_desc) == WFS_GROUP_DESC_SIZE, "group desc packs a block");
_Static_assert(offsetof(struct wfs_group_desc, checksum) == 36, "gd.checksum");

_Static_assert(sizeof(struct wfs_extent) == 24, "extent has no tail hole");
_Static_assert(offsetof(struct wfs_extent, length) == 16, "extent.length");

_Static_assert(sizeof(struct wfs_object) == WFS_OBJECT_SIZE, "object divides every block size");
_Static_assert(offsetof(struct wfs_object, mode) == 12, "obj.mode");
_Static_assert(offsetof(struct wfs_object, size) == 24, "obj.size");
_Static_assert(offsetof(struct wfs_object, btime) == 56, "obj.btime");
_Static_assert(offsetof(struct wfs_object, extents) == 72, "obj.extents");
_Static_assert(offsetof(struct wfs_object, extent_tree_block) == 216, "obj.extent_tree_block");
_Static_assert(offsetof(struct wfs_object, checksum) == 224, "obj.checksum");
_Static_assert(WFS_INLINE_DATA_MAX == WFS_INLINE_EXTENTS * sizeof(struct wfs_extent),
               "inline data spans exactly the extents array");

_Static_assert(sizeof(struct wfs_extent_header) == 16, "extent header leaves records aligned");
_Static_assert(offsetof(struct wfs_extent_header, checksum) == 8, "eh.checksum");
_Static_assert(sizeof(struct wfs_extent_index) == 16, "extent index");
_Static_assert(offsetof(struct wfs_extent_leaf, records) == 16, "leaf records follow the header");
_Static_assert(offsetof(struct wfs_extent_interior, records) == 16, "interior records");

_Static_assert(offsetof(struct wfs_dir_entry, name) == WFS_DIR_ENTRY_HEADER, "dirent header is 12");
_Static_assert(sizeof(struct wfs_dir_tail) == WFS_DIR_TAIL_SIZE, "the tail is one 16-byte record");
_Static_assert(offsetof(struct wfs_dir_tail, checksum) == WFS_DIR_ENTRY_HEADER,
               "the tail's checksum occupies the bytes a name would");

_Static_assert(sizeof(struct wfs_journal_super) == 32, "journal super");
_Static_assert(offsetof(struct wfs_journal_super, first_sequence) == 16, "js.first_sequence");
_Static_assert(sizeof(struct wfs_journal_header) == 24, "journal header");
_Static_assert(offsetof(struct wfs_journal_header, sequence) == 8, "jh.sequence");
_Static_assert(sizeof(struct wfs_journal_target) == 16, "journal target");
_Static_assert(offsetof(struct wfs_journal_descriptor, targets) == 24, "descriptor targets");
_Static_assert(sizeof(struct wfs_journal_commit) == 32, "journal commit");
_Static_assert(offsetof(struct wfs_journal_revoke, blocks) == 32, "revoke block list");

/* The three permitted block sizes must each divide cleanly into the records
 * they carry, which is why the record sizes above are what they are. */
_Static_assert(WFS_BLOCK_SIZE_MIN % WFS_OBJECT_SIZE == 0, "objects pack the smallest block");
_Static_assert(WFS_BLOCK_SIZE_MAX % WFS_OBJECT_SIZE == 0, "objects pack the largest block");
_Static_assert(WFS_BLOCK_SIZE_MIN % WFS_GROUP_DESC_SIZE == 0,
               "descriptors pack the smallest block");

#endif /* FS_WFS_WFS_FORMAT_H */
