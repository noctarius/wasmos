# Custom Filesystem (CFS) — Feature Design Document

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

---

# 3. Disk Layout Overview

```
| Boot Area (optional) |
| Primary Superblock |
| Block Group Descriptor Table |
| Journal Area |
| Object Table |
| Allocation Bitmaps |
| Data Blocks |
| Backup Superblocks |
```

Visual:

```
+-------------------+
| Boot / Reserved   |
+-------------------+
| Superblock        |
+-------------------+
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
````

---

# 4. Superblock

Located at **block 1**.

Backup copies exist in multiple block groups.

## Superblock Structure

```c
struct cfs_superblock {
    uint32_t magic;
    uint32_t version;

    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t total_objects;

    uint64_t root_object_id;

    uint64_t object_table_start;
    uint64_t object_table_blocks;

    uint64_t bitmap_start;
    uint64_t bitmap_blocks;

    uint64_t journal_start;
    uint64_t journal_blocks;

    uint32_t feature_compat;
    uint32_t feature_ro_compat;
    uint32_t feature_incompat;

    uint32_t checksum;
};
````

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
* choose newest valid version

---

# 6. Feature Flags

Feature flags allow forward compatibility.

## Feature Types

| Type      | Meaning              |
| --------- | -------------------- |
| COMPAT    | Safe if unknown      |
| RO_COMPAT | Must mount read-only |
| INCOMPAT  | Must refuse mount    |

Example:

```
FEATURE_EXTENTS
FEATURE_METADATA_CHECKSUM
FEATURE_JOURNAL
FEATURE_INLINE_DATA
```

Mount rules:

```
if unknown INCOMPAT flag → fail mount
if unknown RO_COMPAT flag → mount read-only
```

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
struct cfs_object {
    uint64_t object_id;

    uint16_t type;
    uint16_t flags;

    uint32_t uid;
    uint32_t gid;

    uint64_t size;

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;

    uint32_t link_count;

    uint32_t extent_count;

    struct cfs_extent extents[6];

    uint64_t extent_tree_block;

    uint32_t checksum;
};
```

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
struct cfs_extent {
    uint64_t logical_block;
    uint64_t physical_block;
    uint32_t length;
};
```

---

## Inline Extents

Objects store **6 extents inline**.

Small files require no additional metadata blocks.

---

## Extent Tree

If extents exceed inline capacity:

```
object → extent_tree_block → extent nodes
```

Extent node:

```c
struct cfs_extent_node {
    uint16_t depth;
    uint16_t entries;
    struct cfs_extent records[];
    uint32_t checksum;
};
```

---

# 10. Directory Structure

Directories contain entries mapping names to object IDs.

Entry format:

```c
struct cfs_dir_entry {
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

---

# 11. Allocation

Filesystem uses **bitmaps**.

Separate bitmaps exist for:

```
block allocation
object allocation
```

Allocation policy:

1. prefer locality near parent directory
2. allocate contiguous extents
3. fall back to fragmented blocks

---

# 12. Metadata Checksums

All metadata blocks include checksums.

Structures with checksums:

```
superblock
object records
extent nodes
directory blocks
journal blocks
```

Checksum algorithm:

```
CRC32C
```

Checksum covers entire structure except checksum field.

---

# 13. Metadata Journal

Filesystem uses **write-ahead metadata journaling**.

No copy-on-write.

---

## Journal Layout

Journal is circular.

```
| journal super |
| transaction |
| transaction |
| transaction |
```

---

## Journal Entry

```c
struct cfs_journal_entry {
    uint32_t type;
    uint32_t length;
    uint64_t target_block;
    uint32_t checksum;
};
```

Entry types:

```
WRITE_BLOCK
COMMIT
ABORT
```

---

## Transaction Model

Write sequence:

```
1 write journal entries
2 write COMMIT record
3 apply changes to main fs
4 mark transaction complete
```

On crash:

```
scan journal
replay committed transactions
discard incomplete ones
```

---

# 14. Mount Procedure

Agent must follow:

```
read superblock
verify checksum
validate feature flags
locate journal
replay journal
mount root directory
```

Root object:

```
superblock.root_object_id
```

---

# 15. File Read Flow

```
path lookup
→ directory traversal
→ object record
→ locate extents
→ translate logical offset
→ read blocks
```

---

# 16. File Write Flow

```
start journal transaction
allocate blocks
update extents
update object size
update directory entries if needed
commit transaction
apply metadata updates
```

---

# 17. Deletion

Deletion steps:

```
remove directory entry
decrement link_count
if link_count == 0
    free extents
    free object
```

All metadata updates occur inside a journal transaction.

---

# 18. Hard Links

Hard links implemented via **multiple directory entries pointing to same object_id**.

```
dir A/file → object 42
dir B/file → object 42
```

Deletion rules use link count.

---

# 19. Symlinks

Symlinks stored as:

```
small path → inline data in object
large path → extents
```

---

# 20. Crash Recovery

Recovery algorithm:

```
scan journal
for each transaction
    if COMMIT present
        replay metadata blocks
    else
        discard
```

---

# 21. Minimal Implementation Order

Recommended implementation phases.

### Phase 1

* superblock
* object table
* directories
* extents
* read-only mount

### Phase 2

* allocation bitmaps
* file writes
* truncation

### Phase 3

* metadata journal
* crash recovery

### Phase 4

* checksums
* feature flags
* fsck tool

---

# 22. fsck Strategy

Consistency checks:

```
verify superblock
scan object table
validate extents
validate directory references
rebuild allocation bitmap
validate link counts
```

---

# 23. Reserved Object IDs

```
0 = invalid
1 = root directory
2 = journal file
3 = free object list
```

---

# 24. Future Extensions

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

# 25. Summary

Key architecture:

```
superblock
backup superblocks
object table
extent-based data mapping
metadata checksums
metadata journal
bitmap allocation
feature flags
identity separated from location
```

The design balances:

* simplicity
* robustness
* extensibility
