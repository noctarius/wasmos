# WASMOS Filesystem (WFS) — Feature Design Document

> **Documentation status: format specification.** This document defines the
> on-disk format and the procedures over it. It is not an implementation
> snapshot: what the driver in `src/drivers/fs_wfs` implements today, and which
> of the phases in [§23](#23-minimal-implementation-order) are complete, is
> recorded in [Current Status](STATUS.md). WFS is a third backend beside
> `fs_fat` and `fs_init` under the `fs_manager` router
> ([Filesystem Stack](architecture/18-filesystem-stack.md)).

# 1. Design Goals

The filesystem is designed to be:

- **Simple enough to implement incrementally**
- **Crash-resistant through metadata journaling**
- **Efficient for modern storage**
- **Extensible through feature flags**
- **Clear in on-disk layout for debugging**

Core principles:

1. **Identity separated from location**
2. **Extent-based block mapping**
3. **Object-based metadata (inode-like)**
4. **Metadata journal (no copy-on-write)**
5. **Checksummed metadata**
6. **Bitmap-based allocation**
7. **Feature-flag compatibility**

---

# 2. Block Model

Filesystem operates on **fixed-size blocks**.

Supported sizes:

```
4096 bytes (default)
8192 bytes
16384 bytes
```

Block size stored in superblock.

All structures are block-aligned.

Blocks are grouped. `blocks_per_group` equals `8 * block_size`, so one block of
block bitmap covers exactly one group: 32768 blocks at a 4096-byte block size,
65536 at 8192, 131072 at 16384. Group *N* begins at block *N* \*
`blocks_per_group`. The relation is fixed rather than free so that a recovery
scan can compute group offsets from `block_size` alone.

---

## Byte Order

All multi-byte on-disk fields are **little-endian**.

wasm is little-endian by specification and x86_64 is little-endian, so a wasm
driver and host-side tooling read the same bytes with no conversion. A
big-endian host must swap on access.

---

## Padding

Every structure below is laid out so that it carries no implicit padding: each
field sits at its natural alignment and every hole is a named `reserved` field.
Reserved fields are **zero**.

The rule is what makes the checksums well defined. A checksum specified over a
whole structure otherwise covers compiler-inserted holes whose contents no
writer sets, and the same image then checksums differently depending on which
tool wrote it.

---

# 3. Disk Layout Overview

```
byte 0     | Boot / Reserved              |  1024 bytes
byte 1024  | Primary Superblock           |  1024 bytes
           | Block Group Descriptor Table |
           | Journal Area                 |
           | Object Table                 |
           | Allocation Bitmaps           |
           | Data Blocks                  |
           | Backup Superblocks           |  first block of groups 1, 3, 5, 7...
```

Visual:

```
+-------------------+  byte 0
| Boot / Reserved   |
+-------------------+  byte 1024
| Superblock        |
+-------------------+  byte 2048
| BlockGroup Table  |
+-------------------+
| Journal Area      |
+-------------------+
| Object Table      |
+-------------------+
| Bitmaps           |
+-------------------+
| Data Blocks       |
+-------------------+
| Backup Superblock |
+-------------------+
```

The two leading regions are addressed in **bytes**, not blocks: `block_size` is
a superblock field, so no block unit exists until the superblock has been read.
Everything after them is block-addressed.

The object table and bitmap regions are bounded by the superblock and sliced
per group by the descriptor table, so a group's bitmap and object records sit
near the data blocks they describe.

---

# 4. Superblock

The primary superblock is located at a fixed **byte offset 1024**, independent
of `block_size`.

The offset cannot be expressed in blocks. `block_size` is itself a superblock
field, so a mounter holds no block unit until the superblock has been read;
"block 1" would name byte 4096, 8192, or 16384 depending on a value that is
only knowable afterwards. Mount therefore reads 1024 bytes at byte offset 1024
unconditionally, validates `magic` and `checksum`, and adopts `block_size` for
every subsequent access.

Bytes 0..1023 are the reserved boot area. Bytes 1024..2047 are reserved to the
superblock, so block 0 contains both at each supported block size. Block 0 is
never allocated.

Backup copies exist in multiple block groups.

## Superblock Structure

```c
struct wfs_superblock {
    uint32_t magic;
    uint32_t version;

    uint32_t block_size;
    uint32_t blocks_per_group;   /* 8 * block_size */

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

    uint64_t generation;         /* incremented on every superblock write */

    uint32_t feature_compat;
    uint32_t feature_ro_compat;
    uint32_t feature_incompat;

    uint32_t state;              /* WFS_STATE_CLEAN | _DIRTY | _ERROR */

    uint8_t  uuid[16];           /* volume identity; seeds every checksum */

    uint32_t checksum;
    uint32_t reserved[215];      /* zero; pads the structure to 1024 bytes */
};
```

The structure is exactly 1024 bytes and spans the whole region reserved to it,
so the checksum covers a fixed extent and later versions can claim fields from
`reserved` without a format break.

`uuid` is the volume's identity. It seeds every checksum in the filesystem, so
a block transplanted from another volume fails validation instead of being
accepted as local.

`generation` increments on every superblock write and is what orders the
primary against its backups.

`state` records whether the volume was unmounted cleanly:

```
WFS_STATE_CLEAN  unmounted cleanly; the journal holds nothing to replay
WFS_STATE_DIRTY  mounted for writing, or crashed while mounted
WFS_STATE_ERROR  an inconsistency was detected; mount read-only and run fsck
```

A driver sets `state` to `WFS_STATE_DIRTY` before its first write and back to
`WFS_STATE_CLEAN` on unmount. Mount replays the journal only when `state` is
not `WFS_STATE_CLEAN`.

`free_blocks` and `free_objects` are the sums of the per-group counters and let
a `statfs` answer without scanning every bitmap. They are advisory: fsck
recomputes them from the bitmaps, which are authoritative.

`group_table_start` / `group_table_blocks` locate the block group descriptor
table. The `object_table_*` and `bitmap_*` fields bound whole regions; each
group descriptor names its group's slice inside them.

---

# 5. Backup Superblocks

Backup superblocks are placed in **selected block groups**.

Suggested pattern:

```
Group 0
Group 1
Group 3
Group 5
Group 7
...
```

Agent must:

* read primary superblock
* if checksum fails, scan for backups
* choose the valid copy with the highest `generation`

`generation` is what makes the last step decidable: it increments on every
superblock write, so the highest valid one is the most recent.

A backup sits at the first block of its group, so its byte offset is
*group* \* `blocks_per_group` \* `block_size`. A scan that runs because the
primary is unreadable is bounded: `blocks_per_group` follows from `block_size`,
which has three permitted values, so the scan tries three candidate offsets per
group and accepts the first copy whose `magic` and `checksum` validate.

A volume too small to hold group 1 carries no backup.

---

# 6. Feature Flags

Feature flags allow forward compatibility.

## Feature Types

| Type      | Meaning              |
|-----------|----------------------|
| COMPAT    | Safe if unknown      |
| RO_COMPAT | Must mount read-only |
| INCOMPAT  | Must refuse mount    |

Flags:

| Flag                     | Type      | Meaning                              |
|--------------------------|-----------|--------------------------------------|
| `FEATURE_EXTENTS`        | INCOMPAT  | data is extent-mapped                |
| `FEATURE_JOURNAL`        | INCOMPAT  | the journal region is present        |
| `FEATURE_INLINE_DATA`    | INCOMPAT  | objects may carry data in the record |

Mount rules:

```
if unknown INCOMPAT flag → fail mount
if unknown RO_COMPAT flag → mount read-only
```

Each is INCOMPAT because each changes how existing structures are read, not
what is added beside them: an extent-mapped object, a journalled volume, and an
object holding inline data are all misread rather than merely unrecognized by a
driver that ignores the flag.

There is no checksum feature flag. Every structure carries a `checksum` field
in version 1 of the format, so checksums are not a capability a volume can lack
— a reader that skips them is out of specification, not reading an older
volume.

---

# 7. Object Table

Objects replace traditional **inodes**.

Each object represents:

* file
* directory
* symlink
* device
* special

Objects store metadata and block mapping roots.

---

## Object Layout

```c
struct wfs_object {
    uint64_t object_id;          /*   0 */

    uint16_t type;               /*   8 */
    uint16_t flags;              /*  10 */
    uint32_t mode;               /*  12 */

    uint32_t uid;                /*  16 */
    uint32_t gid;                /*  20 */

    uint64_t size;               /*  24 */

    uint64_t atime;              /*  32 */
    uint64_t mtime;              /*  40 */
    uint64_t ctime;              /*  48 */
    uint64_t btime;              /*  56 */

    uint32_t link_count;         /*  64 */
    uint32_t extent_count;       /*  68 */

    struct wfs_extent extents[6];   /*  72 */

    uint64_t extent_tree_block;  /* 216 */

    uint32_t checksum;           /* 224 */
    uint32_t reserved[7];        /* 228; zero */
};
```

The record is exactly **256 bytes**, so 16, 32, or 64 records occupy a block
with none straddling a boundary and the object table is addressed by plain
division. The 28 reserved bytes hold the growth room the feature-flag model
needs: extended attributes, a data-checksum root, and a snapshot id all want a
field here, and each would otherwise force an incompatible format break.

`mode` holds permission bits only. The object kind stays in `type`, so a reader
validates the kind without masking a packed value.

`btime` is the creation time. It is never updated, which is what distinguishes
it from `ctime`.

## Timestamps

The four timestamps are **unsigned nanoseconds since 1970-01-01T00:00:00Z**,
representable to the year 2554.

The platform clock is coarser than the field. The RTC service reports
broken-down local wall-clock time at one-second resolution with no timezone
(`abi/opcodes.yaml`, `RTC_IPC_READ_RESP`), so a driver converts that to an epoch
count and leaves the sub-second part zero. The field is nanoseconds regardless,
because widening a timestamp later is a format break and the 64 bits are spent
either way.

## Inline Data

When `flags` carries `WFS_OBJ_INLINE_DATA`, the 144 bytes of the `extents`
array hold the object's data directly. `extent_count` and `extent_tree_block`
are then 0 and `size` may not exceed 144.

This is the form a symlink with a path of 144 bytes or fewer takes, and the
form small files take when `FEATURE_INLINE_DATA` is enabled. Neither costs a
data block or an extent.

A reader that does not understand `WFS_OBJ_INLINE_DATA` would read the data as
extents, so `FEATURE_INLINE_DATA` is an `INCOMPAT` flag.

---

# 8. Separation of Identity and Location

Directories reference objects by **object_id**, not block addresses.

```
Directory Entry → object_id → object record → extents → data blocks
```

Benefits:

* renames are cheap
* hard links supported
* open file handles stable

---

# 9. Extents

Extents map logical file offsets to disk blocks.

```
[logical offset] -> [physical block] length
```

## Extent Structure

```c
struct wfs_extent {
    uint64_t logical_block;
    uint64_t physical_block;
    uint32_t length;
    uint32_t reserved; /* zero */
};
```

`logical_block` and `physical_block` are block numbers, not byte offsets.
`length` is a **count of blocks** and is at least 1; a single extent therefore
maps up to 16 TiB at a 4096-byte block size.

`reserved` occupies what would otherwise be implicit tail padding, so that a
checksum taken over an array of extents covers fully defined records.

---

## Inline Extents

Objects store **6 extents inline**, mapping up to 6 discontiguous runs with no
metadata block beyond the object record itself.

A file needing more takes an extent tree; a file small enough to fit in the
record takes inline data instead.

---

## Extent Tree

If extents exceed inline capacity:

```
object → extent_tree_block → root node → ... → leaf node → extents
```

`extent_tree_block` names the root node. Every node occupies exactly one block
and opens with a header common to both node kinds.

```c
struct wfs_extent_header {
    uint16_t magic;    /* WFS_EXTENT_NODE_MAGIC */
    uint16_t depth;    /* 0 = leaf, > 0 = interior */
    uint16_t entries;  /* records in use */
    uint16_t capacity; /* records the block holds at this depth */
    uint32_t checksum;
    uint32_t reserved; /* zero */
};
```

The checksum is a header field. C places a flexible array member last, so no
field may follow a node's record array.

The header is 16 bytes and both record types are 8-byte aligned, so a node
carries no implicit padding.

`depth` selects the record type. A leaf holds extents. An interior node holds
indices, each naming the block of a child one level shallower — without them a
tree could hold no more than its root, and `depth` could never exceed 0.

```c
struct wfs_extent_index {
    uint64_t logical_block; /* first logical block the child covers */
    uint64_t child_block;   /* physical block holding the child node */
};

struct wfs_extent_leaf {
    struct wfs_extent_header header;  /* header.depth == 0 */
    struct wfs_extent records[];
};

struct wfs_extent_interior {
    struct wfs_extent_header header;  /* header.depth > 0 */
    struct wfs_extent_index records[];
};
```

Records are sorted by `logical_block` and cover disjoint ranges. Lookup
descends from the root, at each interior node taking the last index whose
`logical_block` does not exceed the target, until `depth` reaches 0.

`extent_tree_block` selects which map an object has, and the two are exclusive:

| `extent_tree_block` | The map is | `extent_count` |
|---|---|---|
| 0 | the first `extent_count` inline extents | at most 6 |
| non-zero | the tree | total extents across its leaves |

When a tree exists the inline array is **zero**. Two sources of truth for one
logical range is a corruption nothing could detect: a reader would take whichever
it consulted first and two readers could disagree about where a block lives.

A logical block covered by no extent is a **hole**, and reads as zeroes. A hole
is not an error: a file written sparsely has ranges no extent maps.

`depth` is bounded. An interior node's children are one level shallower, so a
descent that does not strictly decrease `depth` is a cycle, and a tree deeper
than `WFS_EXTENT_MAX_DEPTH` cannot be reached by any legal write.

`capacity` follows from the block size and is validated on read:

| Block size | Leaf extents | Interior indices |
|-----------:|-------------:|-----------------:|
|       4096 |          170 |              255 |
|       8192 |          340 |              511 |
|      16384 |          682 |             1023 |

A node failing `magic`, `entries <= capacity`, or `checksum` renders its object
unreadable and is reported by fsck.

---

# 10. Directory Structure

Directories contain entries mapping names to object IDs.

Entry format:

```c
struct wfs_dir_entry {
    uint64_t object_id;
    uint16_t record_length;
    uint8_t name_length;
    uint8_t type;
    char name[];
};
```

Properties:

* variable-length records
* packed entries
* directory is stored as regular file data

`record_length` is the stride to the next entry and is a **multiple of 8**, so
every entry's `object_id` lands on its natural alignment. Records are strided
by `record_length` and never by `sizeof`: the fixed header is 12 bytes and
`name` begins at offset 12, while `sizeof` rounds up to 16 for alignment. No
on-disk record has a 4-byte hole there.

The requirement is not cosmetic. A misaligned record cast to
`struct wfs_dir_entry *` is undefined behavior that clang is free to compile
against the alignment it assumes, which matters most for host-side tooling
built at higher optimization levels than the driver.

`record_length` is at least `12 + name_length` rounded up to a multiple of 8.
Bytes between the end of `name` and the end of the record are zero.

No record straddles a block boundary. The last record in a block has its
`record_length` extended to the start of the block's tail, so a scan of a
directory block ends exactly where the tail begins.

## The Tail

The last 16 bytes of every directory block are a tail record carrying the
block's checksum. A directory block holds entries, not one structure, so there
is no other field the checksum §13 requires could live in.

```c
struct wfs_dir_tail {
    uint64_t object_id;     /* 0 */
    uint16_t record_length; /* 16 */
    uint8_t name_length;    /* 0 */
    uint8_t type;           /* WFS_DIR_TAIL_TYPE */
    uint32_t checksum;
};
```

The tail is laid out as a directory record whose `object_id` is 0, so a scan
that knows nothing about it reads free space and skips it — the same rule that
already governs a removed entry. `name_length` is 0 and the four bytes a name
would occupy hold the checksum.

The checksum covers the whole block with these four bytes zeroed, seeded with
the block's own number (§13).

A record with `object_id == 0` is free space: this is how an entry is removed
without rewriting the block. A scan skips it and an insertion may claim it.

`.` and `..` are the first two records of every directory, in that order. The
root directory's `..` names the root.

---

# 11. Block Groups

The volume is divided into groups of `blocks_per_group` blocks. The descriptor
table at `group_table_start` holds one descriptor per group, in group order.

```c
struct wfs_group_desc {
    uint64_t block_bitmap;   /* first block of this group's block bitmap */
    uint64_t object_bitmap;  /* first block of this group's object bitmap */
    uint64_t object_table;   /* first block of this group's object table slice */

    uint32_t free_blocks;
    uint32_t free_objects;

    uint32_t flags;          /* WFS_GROUP_HAS_SUPER_BACKUP */
    uint32_t checksum;

    uint32_t reserved[6];    /* zero; pads the descriptor to 64 bytes */
};
```

The descriptor is 64 bytes, so 64, 128, or 256 fit a block exactly.

A group is the unit of locality. Confining an object and its data to one group
keeps a bitmap, an object record, and the blocks it maps within one region of
the volume.

---

# 12. Allocation

Allocation is **bitmap-based**. There is no free list, and no object holds one.

A block bitmap and an object bitmap exist per group. One bit maps one block or
one object record, least-significant bit first, and a set bit means allocated.
`blocks_per_group` is `8 * block_size` precisely so that one block of bitmap
covers one group's blocks.

The bitmaps are authoritative; the counters in the superblock and the group
descriptors are derived from them and are rebuilt by fsck.

Allocation policy:

1. prefer the group holding the parent directory
2. allocate contiguous extents
3. fall back to fragmented blocks
4. fall back to another group

---

# 13. Metadata Checksums

All metadata blocks include checksums.

Structures with checksums:

```
superblock
group descriptors
object records
extent nodes
directory blocks
journal blocks
```

Checksum algorithm:

```
CRC32C
```

A checksum is taken over the entire structure with its own `checksum` field set
to zero, including every `reserved` field. Reserved and padding bytes are zero,
so the image is fully defined.

## Seeding

A checksum is seeded with the volume `uuid` and with the location that
addresses the structure:

```
seed  = crc32c(0xFFFFFFFF, uuid, 16)
seed  = crc32c(seed, &location, 8)      /* little-endian uint64 */
value = crc32c(seed, structure_image)
```

`location` is the identifier that addresses the structure:

| Structure | `location` |
|---|---|
| Primary superblock | 0 |
| Backup superblock | its own block number |
| Group descriptor | its group index |
| Object record | its `object_id` |
| Directory block | its block number |
| Extent tree node | its block number |
| Journal block | its block number |

A group descriptor and an object record are records inside a shared block, so
neither has a block number of its own to be bound to; the index that addresses
the record serves instead, and a descriptor or record moved to the wrong slot
fails to verify.

Seeding is what turns a checksum into a detector of misdirected and misplaced
writes. Unseeded, a block written to the wrong address still validates
perfectly at its new home, and a block copied in from another volume validates
as native. Seeded, both fail.

## Cost

wasm has no CRC instruction, so CRC32C is a software table — a slicing-by-8
table is 8 KiB — and there is no CRC32C in this repository today. Under the
wasm3 interpreter that cost is paid per byte. Journaling doubles it, because
every metadata block is checksummed once as a journal image and once in place.

The scope is deliberately metadata only. Data checksums are a feature flag
(future extension), not a default.

---

# 14. Metadata Journal

Filesystem uses **write-ahead metadata journaling**.

No copy-on-write.

---

## Journal Layout

The journal occupies `journal_blocks` blocks from `journal_start`. Its first
block is the journal superblock; the remainder is a circular log.

```
| journal super | transaction | transaction | ... |
                ^ first_block          wraps to the first log block
```

```c
struct wfs_journal_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;     /* equals superblock.block_size */
    uint32_t blocks;         /* journal length in blocks, including this one */
    uint64_t first_sequence; /* oldest transaction that may need replay */
    uint32_t first_block;    /* journal-relative block holding its descriptor */
    uint32_t checksum;
};
```

`first_sequence` and `first_block` are the log tail: recovery starts there, and
checkpointing advances it.

---

## Journal Blocks

Every journal block opens with a common header. `sequence` is a monotonically
increasing transaction id, and is what binds a descriptor, the block images
following it, and its commit into one transaction. Without it a scan sees a
flat run of blocks with no way to group them or to tell a live block from
stale content left behind by a wrap.

```c
struct wfs_journal_header {
    uint32_t magic;    /* WFS_JOURNAL_MAGIC, on every journal block */
    uint32_t type;     /* WFS_JOURNAL_DESCRIPTOR | _COMMIT | _REVOKE */
    uint64_t sequence;
    uint32_t checksum;
    uint32_t reserved; /* zero */
};
```

`checksum` covers the **whole block** with its own four bytes zeroed, seeded
with the block's own number (§13). It is not a checksum of the header alone: a
descriptor's targets and a revoke's block list follow the header and are the
part recovery acts on, so a header-only checksum would leave them unprotected.
A block laid out for one offset therefore fails to verify at another.

The journal superblock is the exception. It is sealed over its 32-byte record
alone, seeded with its own block number, because nothing else in that block is
defined.


Block types:

```
DESCRIPTOR  names the destination of each block image that follows it
COMMIT      makes the transaction replayable
REVOKE      bars older images of the listed blocks from replay
```

There is no ABORT type. A transaction is replayable if and only if a valid
COMMIT block carries its sequence; a crash cannot be relied on to record its
own failure.

---

## Descriptor

A descriptor is followed immediately by one block image per target, in the
order the targets are listed.

```c
struct wfs_journal_target {
    uint64_t target_block; /* filesystem block this image replaces */
    uint32_t flags;        /* WFS_JOURNAL_TARGET_LAST on the final target */
    uint32_t checksum;     /* CRC32C of the block image */
};

struct wfs_journal_descriptor {
    struct wfs_journal_header header;
    struct wfs_journal_target targets[];
};
```

A transaction with more targets than one descriptor block holds continues with
a further descriptor carrying the same `sequence`.

`checksum` is a plain CRC32C of the image, **unseeded**. An image is not
addressed by the block it is stored in — it is identified by the descriptor
record that names both, and that record is sealed with the descriptor block.

The target list ends at the record carrying `WFS_JOURNAL_TARGET_LAST`, or at a
record whose `target_block` is 0. Block 0 holds the boot area and the primary
superblock and is never allocated (§4), so no transaction can target it, and an
all-zero record past the last one therefore terminates the list. That is what
lets a transaction with **no** targets be represented: a revoke-only
transaction has no record on which to set the flag.

---

## Commit

```c
struct wfs_journal_commit {
    struct wfs_journal_header header;
    uint32_t target_count; /* targets the transaction journaled in total */
    uint32_t reserved;     /* zero */
};
```

`target_count` lets recovery confirm that every image the transaction promised
is present before any of them is applied.

---

## Revoke

```c
struct wfs_journal_revoke {
    struct wfs_journal_header header;
    uint32_t count;
    uint32_t reserved; /* zero */
    uint64_t blocks[];
};
```

A revoke record is mandatory whenever a block stops being metadata, whether it
is freed or freed and reallocated as file data. The transaction that frees the
block revokes it in the same transaction.

The journal records block numbers, not what a block currently holds. An older
committed image of a freed block therefore stays replayable after the block has
been handed to a file, and replaying it overwrites live file data with stale
metadata. The revoke record is what makes that image unreplayable.

---

## Transaction Model

A barrier is a flush that completes before the following step is issued.

```
1 write the descriptor blocks and the block images
2 barrier
3 write the COMMIT block
4 barrier
5 checkpoint: write each image to its target_block
6 barrier
7 advance journal_super.first_sequence / first_block past the transaction
```

Steps 1-4 make the transaction durable; steps 5-7 retire it. A crash before
step 3 discards the transaction. A crash between steps 3 and 7 replays it.

Log space is consumed in order and wraps at the end of the journal. A
transaction that does not fit in the free span is preceded by a checkpoint of
enough older transactions to make room.

---

# 15. Mount Procedure

Agent must follow:

```
read 1024 bytes at byte offset 1024
verify magic and checksum
on failure, scan for backups and take the highest valid generation
validate feature flags
validate block_size and blocks_per_group == 8 * block_size
read the block group descriptor table
if state != WFS_STATE_CLEAN, replay the journal
mount root directory
before the first write, set state = WFS_STATE_DIRTY and flush
```

The read is expressed in bytes, not blocks: `block_size` is a superblock field
and is unknown until this read completes.

The `state` check is what keeps a clean mount from replaying: a volume
unmounted cleanly has nothing in its journal to apply, and scanning it on every
mount costs a full journal read for no result.

Root object:

```
superblock.root_object_id
```

---

# 16. File Read Flow

```
path lookup
→ directory traversal
→ object record
→ locate extents
→ translate logical offset
→ read blocks
```

---

# 17. File Write Flow

```
start journal transaction
allocate blocks from the parent's group where possible
write file data to the allocated blocks
update extents
update object size and mtime
update directory entries if needed
revoke any metadata block the write freed
journal the changed metadata blocks
commit transaction
checkpoint: apply metadata updates in place
```

File data is not journalled. Only metadata is, which is what
"metadata journaling" means here: after a crash the metadata is consistent, but
a block newly allocated to a file may hold whatever it held before.

Data is written before the transaction commits. Committing first would let a
crash leave an extent pointing at a block whose contents were never written.

---

# 18. Deletion

Deletion steps:

```
set the directory record's object_id to 0
decrement link_count
if link_count == 0
    free extents
    free object
    revoke every freed metadata block
```

All metadata updates occur inside a journal transaction.

The revoke is not optional. A freed block may be handed to a file before the
journal wraps past the transactions that hold images of it, and replay of such
an image would overwrite that file's data with stale metadata.

---

# 19. Hard Links

Hard links implemented via **multiple directory entries pointing to same object_id**.

```
dir A/file → object 42
dir B/file → object 42
```

Deletion rules use link count.

---

# 20. Symlinks

Symlinks stored as:

```
path <= 144 bytes → inline data in the object record
path >  144 bytes → extents
```

The inline form is the `WFS_OBJ_INLINE_DATA` layout: the path occupies the 144
bytes of the `extents` array, and `size` is its length. No terminator is
stored.

144 bytes covers ordinary paths, so a symlink normally costs no data block.

---

# 21. Crash Recovery

Recovery reads the journal superblock and makes three passes over the log,
starting at `first_block` with `expected = first_sequence`.

## Pass 1 — find the head

Walk the log forward. A block belongs to the log when its `magic` validates and
its `sequence` equals `expected`; the first block failing either test is the
head, and everything beyond it is stale content left by a wrap.

A transaction enters the replay set only when a COMMIT block carries its
sequence, `target_count` matches the targets its descriptors named, and every
header checksum validates. `expected` advances by one per committed
transaction.

## Pass 2 — build the revoke table

Over the replay set, map each block named by a REVOKE record to the highest
`sequence` at which it was revoked.

## Pass 3 — replay

In ascending `sequence` order, for each target of each transaction in the
replay set:

```
revoke_table[target_block] >= transaction.sequence  → skip the image
target.checksum does not match the image            → abort recovery
otherwise                                           → write image to target_block
```

The revoke comparison is `>=`, not `>`: an image journaled by the same
transaction that revoked its block must not be replayed either.

A failed image checksum aborts recovery rather than applying a partial
transaction. The volume then mounts read-only and fsck runs.

Recovery ends by barriering the replayed writes, then setting `first_sequence`
to one past the last replayed transaction and `first_block` to the head. A
crash during recovery repeats it from the same tail; replay is idempotent
because every image is a whole-block overwrite.

---

# 22. Host Integration and 64-bit Boundaries

The on-disk format is 64-bit. The platform a wasm driver reaches it through is
32-bit at every boundary. This section records where the two meet.

## Why u64 on disk is safe

`i64` is a first-class wasm value type, not an emulated one, and every
structure here lays out identically for `wasm32` and `x86_64` — same sizes,
same offsets. A wasm driver and host-side tooling therefore agree on every
byte without conversion or packing attributes, provided only fixed-width types
appear on disk. A `long`, a `size_t`, or a pointer in an on-disk structure
breaks that immediately: those are 4 bytes on wasm32 and 8 on x86_64.

Both engines implement the full `i64` opcode set. Under the wasm3 interpreter
there is a cost: it is built with 32-bit operand-stack slots
(`d_m3Use32BitSlots`), so each `i64` that spills to the stack consumes two
slots against `d_m3MaxFunctionSlots`. A `u64`-dense function — the extent-tree
walker most of all — exhausts that budget twice as fast as a `u32` one.

## Where u64 cannot go

A 64-bit value cannot cross into or out of a wasm module as a scalar:

* **Host calls are i32-only.** The generator in `scripts/gen_abi_hostcalls.py`
  emits nothing but `i32` params and an `i32` or `void` return. The single
  64-bit value in the whole host-call surface, `wasmos_region_alloc`'s
  `out_phys`, crosses as a *pointer to* a `uint64_t`.
* **IPC carries four 32-bit arguments.** A 64-bit value must be split, as the
  RTC opcodes already split one across `time_packed_lo` / `time_packed_hi`.
  The block opcodes have no room left to do so: `BLOCK_IPC_READ_ZC_REQ` already
  spends all four arguments.
* **The block layer is 32-bit LBA end to end**, which at 512-byte sectors caps
  addressing at 2 TiB.
* **The filesystem ABI caps offsets at `int32`.** `lseek` refuses anything
  beyond `INT32_MAX` and `stat` carries size as `int32_t`, so file offsets above
  2 GiB are unobservable from an application no matter what the object record
  holds.

## The rule

**Block numbers are carried as `uint32_t` inside the driver. Byte counts that
can exceed 4 GiB are carried as `uint64_t` and never crossed a boundary.**

Block numbers are blocks, not bytes: a `uint32_t` block number reaches 16 TiB
at a 4096-byte block size, past the 2 TiB the block layer can address anyway.
So the driver truncates on read and mount refuses a volume it cannot address:

```
total_blocks  > UINT32_MAX → refuse the mount
total_objects > UINT32_MAX → refuse the mount
```

Refusal returns a packed code from `abi/errors.yaml`, never a bare `-1`.

This keeps the i32 host-call and IPC surface untouched and avoids splitting
every block number lo/hi across call sites that have no argument space for it,
while leaving the on-disk fields wide enough that raising the ceiling later is
a driver change and not a format break.

`wfs_object.size` is the exception: it counts bytes, 4 GiB is a plausible file,
and it stays `uint64_t` inside the driver. It is truncated to `INT32_MAX` only
where it crosses the filesystem ABI, and widening `lseek` and `stat` is
tracked as platform work rather than as part of this format.

Journal sequence numbers are `uint64_t` and never cross a boundary at all.

---

# 23. Minimal Implementation Order

Recommended implementation phases.

### Phase 1

* superblock and byte-offset mount
* block group descriptor table
* object table
* directories
* extents and the extent tree
* checksums, seeded
* feature-flag validation
* read-only mount

### Phase 2

* allocation bitmaps
* file writes
* truncation

### Phase 3

* metadata journal, including revoke records
* crash recovery
* the `state` flag

### Phase 4

* inline data
* fsck tool

Checksums and feature-flag validation belong to phase 1, not to a later pass.
Every structure carries a `checksum` field from the first version of the
format, so a phase that writes those structures without computing it produces
images that a later phase must either reject or migrate. The same holds for
`INCOMPAT` flag checks: a reader that skips them mounts volumes it cannot
correctly interpret.

Phase 2 writes without a journal and is therefore not crash-safe. That is
acceptable only as a development stage.

Phase 3 is not complete until every writer runs inside a transaction. A journal
that exists but that the metadata writers bypass changes nothing a crash can
observe: replay finds an empty log, and the damage an interrupted write left is
in the filesystem rather than the journal.

---

# 24. fsck Strategy

Consistency checks:

```
verify superblock and pick the highest valid generation
verify the group descriptor table
scan object table
validate extents and extent tree nodes
validate directory references, record_length strides, and . / .. records
rebuild allocation bitmaps
recompute free_blocks / free_objects in the superblock and every descriptor
validate link counts
clear state to WFS_STATE_CLEAN
```

The bitmaps are authoritative and the counters are derived, so a counter
mismatch is repaired from the bitmap rather than the reverse.

---

# 25. Reserved Object IDs

```
0      = invalid; in a directory record it marks free space
1      = root directory
2..15  = reserved
16     = first allocatable object id
```

No id names a journal or a free list. The journal is a fixed region addressed by
`journal_start` / `journal_blocks`, not an object, and allocation is bitmap-based,
so there is no free-object list for an id to point at.

---

# 26. Future Extensions

Possible later features:

```
compression
data checksums
directory indexing
snapshot support
online resize
extended attributes
```

Each must introduce a new feature flag.

---

# 27. Summary

Key architecture:

```
superblock at a fixed byte offset, generation-ordered against its backups
block groups as the unit of locality
object table of fixed 256-byte records
extent-based data mapping with an indexed extent tree
metadata checksums, seeded with the volume uuid and the structure's location
metadata journal with sequence numbers and revoke records
bitmap allocation
feature flags
identity separated from location
little-endian, no implicit padding, 64-bit on disk
```

The design balances:

* simplicity
* robustness
* extensibility
