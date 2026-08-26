/* wfs_dirent.h - directory record surgery inside one block (§10).
 *
 * Pure: one staged block, no device and no task, so the record format is
 * testable without an image under it. Everything here leaves the block VALID —
 * the tail checksum is resealed before returning — because a half-updated
 * directory block is indistinguishable from a corrupt one to the reader.
 *
 * The format rules these functions must not break, all from §10:
 *   - `record_length` is the stride to the next record and is a multiple of 8,
 *     so every record's object_id lands on its natural alignment. Records are
 *     strided by it and never by sizeof: the header is 12 bytes and the name
 *     starts at 12, while sizeof rounds to 16.
 *   - `object_id == 0` is free space. A removed record and the block's tail are
 *     both encoded that way, so a scan that knows nothing about the tail skips
 *     it under the rule it already applies to removed records.
 *   - No record straddles a block boundary, and the records cover the usable
 *     area exactly: the last one's `record_length` reaches the tail, so a scan
 *     ends precisely where the tail begins.
 *
 * That last rule is why an insert must be able to split a record that is IN USE.
 * A directory mkfs_wfs just wrote has no free record at all — its final entry's
 * length spans to the tail — so the space to insert into is the SLACK inside a
 * used record, not a gap between records.
 */
#ifndef FS_WFS_WFS_DIRENT_H
#define FS_WFS_WFS_DIRENT_H

#include <stdint.h>

#include "wfs_format.h"
#include "wfs_status.h"

/* Walk the record chain and check every stride against §10.
 *
 * Called before any surgery: writing into a block whose chain is already broken
 * would turn a detectable corruption into a plausible-looking directory. */
wasmos_error_code_t wfs_dirent_validate(const uint8_t* block, uint32_t block_size);

/* Recompute the tail checksum, which covers the whole block with its own four
 * bytes zeroed and is seeded with the block's number (§13). */
void wfs_dirent_seal(uint8_t* block, uint32_t block_size, const uint8_t* uuid, uint64_t location);

/* Byte offset of the record naming `name`, or -1 when the block does not carry
 * it. Comparison is exact and case-SENSITIVE: WFS names are bytes. */
int32_t wfs_dirent_find(const uint8_t* block, uint32_t block_size, const char* name,
                        uint32_t name_len);

/* Lay out a freshly allocated, zeroed block as an empty directory block: one
 * free record spanning the usable area, plus the tail. A zeroed block is NOT a
 * valid directory block — its first record has a stride of 0, which a scan reads
 * as a chain that never advances. */
void wfs_dirent_init_block(uint8_t* block, uint32_t block_size, const uint8_t* uuid,
                           uint64_t location);

/* Insert a record naming `object_id`.
 *
 * Takes the space from a free record or from the slack inside a used one,
 * whichever comes first, and leaves a free record behind when the remainder is
 * large enough to hold one. Reseals the tail.
 *
 * Returns WASMOS_ERR_FS_EXISTS when the name is already present — checked first,
 * so a failed insert changes nothing — WASMOS_ERR_FS_NAME for a name that is
 * empty or longer than WFS_NAME_MAX, and WASMOS_ERR_FS_NO_SPACE when no record
 * has room. On any error the block is left untouched.
 */
wasmos_error_code_t wfs_dirent_insert(uint8_t* block, uint32_t block_size, const uint8_t* uuid,
                                      uint64_t location, const char* name, uint32_t name_len,
                                      uint32_t object_id, uint8_t type);

/* Remove the record naming `name`, then MERGE adjacent free records so the space
 * comes back as one usable gap rather than a row of holes too small to reuse.
 * Reseals the tail.
 *
 * Returns WASMOS_ERR_FS_NOT_FOUND when the block does not carry the name, having
 * changed nothing.
 */
wasmos_error_code_t wfs_dirent_remove(uint8_t* block, uint32_t block_size, const uint8_t* uuid,
                                      uint64_t location, const char* name, uint32_t name_len);

#endif /* FS_WFS_WFS_DIRENT_H */
