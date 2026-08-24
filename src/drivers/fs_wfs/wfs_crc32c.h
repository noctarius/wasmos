/* wfs_crc32c.h - CRC32C (Castagnoli) and the seeded metadata checksum.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §13.
 *
 * The polynomial is CRC-32C, reflected 0x82F63B78, initialised to 0xFFFFFFFF
 * and finalised by inverting — the same parameters as iSCSI (RFC 3720) and as
 * ext4's metadata checksums, so an image is verifiable by tools outside this
 * tree.
 *
 * wasm has no CRC instruction, so this is a software table and the cost is paid
 * per byte; under the wasm3 interpreter that cost is interpreted. Journaling
 * doubles it, because every metadata block is checksummed once as a journal
 * image and once in place. That is why the scope is metadata only.
 *
 * The running value passed between _update calls is NOT finalised. Call
 * wfs_crc32c_finish exactly once, at the end.
 */
#ifndef FS_WFS_WFS_CRC32C_H
#define FS_WFS_WFS_CRC32C_H

#include <stdint.h>

#include "wfs_format.h"

#define WFS_CRC32C_INIT 0xFFFFFFFFu

/* Fold `len` bytes into a running CRC. */
uint32_t wfs_crc32c_update(uint32_t crc, const void* data, uint32_t len);

/* Finalise a running CRC into the value that is stored on disk. */
static inline uint32_t wfs_crc32c_finish(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

/* One-shot CRC32C of a buffer. */
uint32_t wfs_crc32c(const void* data, uint32_t len);

/* The seed every metadata checksum starts from: the volume uuid followed by the
 * location that addresses the structure, as a little-endian uint64.
 *
 * Seeding is what turns a checksum into a detector of misdirected and misplaced
 * writes. Unseeded, a block written to the wrong address still validates
 * perfectly at its new home and a block copied in from another volume validates
 * as native; seeded, both fail. `location` is the block number for a
 * block-addressed structure and the object_id for an object record. The primary
 * superblock uses 0 and a backup uses its own block number.
 */
uint32_t wfs_crc32c_seed(const uint8_t uuid[WFS_UUID_LEN], uint64_t location);

/* Checksum of a structure with its own `checksum` field treated as zero,
 * covering every other byte including the reserved fields.
 *
 * `checksum_offset` is the byte offset of that uint32 field within the
 * structure. The field is not skipped but replaced by four zero bytes, so the
 * value does not depend on what the field currently holds — which is what lets
 * a verifier recompute it in place.
 */
uint32_t wfs_checksum_struct(const uint8_t uuid[WFS_UUID_LEN], uint64_t location, const void* base,
                             uint32_t size, uint32_t checksum_offset);

#endif /* FS_WFS_WFS_CRC32C_H */
