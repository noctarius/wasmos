/* wfs_super.c - superblock validation.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §4, §5, §13, §15, §22.
 */
#include "wfs_super.h"

#include "wfs_crc32c.h"
#include "wfs_endian.h"

/* A 64-bit on-disk count narrowed to the uint32_t the driver carries (§22).
 * Returns 0 when the value does not fit, which every caller treats as a refusal
 * rather than as a truncation. */
static int fits_u32(uint64_t v, uint32_t* out) {
    if (v > 0xFFFFFFFFu) {
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

int wfs_super_group_has_backup(uint32_t group) {
    /* Group 0 holds the primary. Backups then occupy the odd groups, which
     * spreads them without consuming a block in every group. */
    return group != 0u && (group & 1u) != 0u;
}

uint64_t wfs_super_backup_offset(uint32_t block_size, uint32_t group) {
    if (!wfs_super_group_has_backup(group)) {
        return 0;
    }
    return (uint64_t)group * (uint64_t)WFS_BLOCKS_PER_GROUP(block_size) * (uint64_t)block_size;
}

int wfs_super_backup_prefer(const wfs_super_t* current, int have_current,
                            const wfs_super_t* candidate) {
    if (!candidate) {
        return 0;
    }
    if (!have_current || !current) {
        return 1;
    }
    return candidate->generation > current->generation;
}

int wfs_super_backup_candidate(uint32_t index, uint32_t* out_block_size, uint32_t* out_group) {
    /* The three permitted block sizes (§4). block_size is itself a superblock
     * field, so a scan that runs because the primary is unreadable does not know
     * it and must try each -- which is bounded precisely because
     * blocks_per_group follows from block_size rather than being stored freely
     * (§5). A wrong guess is self-rejecting: the candidate's checksum is seeded
     * with the block number that guess implies, so it fails to verify. */
    static const uint32_t sizes[WFS_SUPER_SCAN_SIZES] = {4096u, 8192u, 16384u};
    uint32_t slot;

    if (!out_block_size || !out_group || index >= WFS_SUPER_SCAN_CANDIDATES) {
        return 0;
    }
    slot = index / WFS_SUPER_SCAN_SIZES;
    *out_block_size = sizes[index % WFS_SUPER_SCAN_SIZES];
    /* Backups occupy the odd groups: 1, 3, 5, ... (§5). */
    *out_group = slot * 2u + 1u;
    return 1;
}

wasmos_error_code_t wfs_super_parse(const void* image, uint32_t len, uint64_t location,
                                    wfs_super_t* out) {
    const uint8_t* p = (const uint8_t*)image;
    wfs_super_t sb;
    uint32_t stored_checksum;
    uint32_t computed;
    uint64_t total_blocks;
    uint64_t total_objects;
    uint32_t i;

    if (!p || !out || len < WFS_SUPER_SIZE) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }

    /* Identity first: a device that was never formatted must not be reported as
     * a corrupt volume. */
    if (wfs_rd32(p, offsetof(struct wfs_superblock, magic)) != WFS_MAGIC) {
        return WASMOS_ERR_FS_BAD_MAGIC;
    }
    if (wfs_rd32(p, offsetof(struct wfs_superblock, version)) != WFS_VERSION) {
        return WASMOS_ERR_FS_VERSION;
    }

    /* Integrity next. The uuid is read straight out of the image because it
     * seeds the checksum that is about to prove the image, including the uuid
     * itself: a tampered uuid produces a mismatch here rather than a volume
     * that verifies under its own altered identity. */
    for (i = 0; i < WFS_UUID_LEN; ++i) {
        sb.uuid[i] = p[offsetof(struct wfs_superblock, uuid) + i];
    }
    stored_checksum = wfs_rd32(p, offsetof(struct wfs_superblock, checksum));
    computed = wfs_checksum_struct(
        sb.uuid, location, p, WFS_SUPER_SIZE, (uint32_t)offsetof(struct wfs_superblock, checksum));
    if (computed != stored_checksum) {
        return WASMOS_ERR_FS_CHECKSUM;
    }

    /* Capability. An unknown INCOMPAT bit means existing structures would be
     * misread, so the volume is refused outright; an unknown RO_COMPAT bit
     * leaves it readable but unwritable (§6). */
    sb.feature_compat = wfs_rd32(p, offsetof(struct wfs_superblock, feature_compat));
    sb.feature_ro_compat = wfs_rd32(p, offsetof(struct wfs_superblock, feature_ro_compat));
    sb.feature_incompat = wfs_rd32(p, offsetof(struct wfs_superblock, feature_incompat));
    if (sb.feature_incompat & ~(uint32_t)WFS_FEATURE_INCOMPAT_SUPPORTED) {
        return WASMOS_ERR_FS_FEATURE_INCOMPAT;
    }
    sb.read_only = (sb.feature_ro_compat & ~(uint32_t)WFS_FEATURE_RO_COMPAT_SUPPORTED) ? 1u : 0u;

    /* Geometry. */
    sb.block_size = wfs_rd32(p, offsetof(struct wfs_superblock, block_size));
    if (sb.block_size != 4096u && sb.block_size != 8192u && sb.block_size != 16384u) {
        return WASMOS_ERR_FS_GEOMETRY;
    }
    sb.blocks_per_group = wfs_rd32(p, offsetof(struct wfs_superblock, blocks_per_group));
    if (sb.blocks_per_group != WFS_BLOCKS_PER_GROUP(sb.block_size)) {
        return WASMOS_ERR_FS_GEOMETRY;
    }

    /* Address range (§22). Refused rather than truncated: a truncated block
     * count would put the driver's idea of the volume's end inside the volume,
     * and every allocation past it would land on live data. */
    total_blocks = wfs_rd64(p, offsetof(struct wfs_superblock, total_blocks));
    total_objects = wfs_rd64(p, offsetof(struct wfs_superblock, total_objects));
    if (!fits_u32(total_blocks, &sb.total_blocks) || !fits_u32(total_objects, &sb.total_objects)) {
        return WASMOS_ERR_FS_VOLUME_TOO_LARGE;
    }
    if (sb.total_blocks == 0u) {
        return WASMOS_ERR_FS_CORRUPT;
    }

    if (!fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, root_object_id)),
                  &sb.root_object_id) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, group_table_start)),
                  &sb.group_table_start) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, group_table_blocks)),
                  &sb.group_table_blocks) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, object_table_start)),
                  &sb.object_table_start) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, object_table_blocks)),
                  &sb.object_table_blocks) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, bitmap_start)), &sb.bitmap_start) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, bitmap_blocks)), &sb.bitmap_blocks) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, journal_start)), &sb.journal_start) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, journal_blocks)),
                  &sb.journal_blocks) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, free_blocks)), &sb.free_blocks) ||
        !fits_u32(wfs_rd64(p, offsetof(struct wfs_superblock, free_objects)), &sb.free_objects)) {
        return WASMOS_ERR_FS_VOLUME_TOO_LARGE;
    }

    /* Consistency of the regions against the volume they sit in. A region
     * ending past the last block would have the driver reading whatever the
     * device returns beyond it. */
    if (sb.group_table_start >= sb.total_blocks || sb.object_table_start >= sb.total_blocks ||
        sb.bitmap_start >= sb.total_blocks || sb.journal_start >= sb.total_blocks) {
        return WASMOS_ERR_FS_CORRUPT;
    }
    if (sb.group_table_blocks > sb.total_blocks - sb.group_table_start ||
        sb.object_table_blocks > sb.total_blocks - sb.object_table_start ||
        sb.bitmap_blocks > sb.total_blocks - sb.bitmap_start ||
        sb.journal_blocks > sb.total_blocks - sb.journal_start) {
        return WASMOS_ERR_FS_CORRUPT;
    }
    if (sb.root_object_id != WFS_OBJECT_ROOT) {
        return WASMOS_ERR_FS_CORRUPT;
    }

    /* Groups partition the volume, so the count is a ceiling division and the
     * descriptor table must be large enough to describe every one of them. */
    sb.group_count = (sb.total_blocks + sb.blocks_per_group - 1u) / sb.blocks_per_group;
    if (sb.group_table_blocks * wfs_group_descs_per_block(sb.block_size) < sb.group_count) {
        return WASMOS_ERR_FS_CORRUPT;
    }

    sb.state = wfs_rd32(p, offsetof(struct wfs_superblock, state));
    if (sb.state != WFS_STATE_CLEAN && sb.state != WFS_STATE_DIRTY && sb.state != WFS_STATE_ERROR) {
        return WASMOS_ERR_FS_CORRUPT;
    }
    /* Only DIRTY owes a replay. WFS_STATE_ERROR says an inconsistency was
     * already DETECTED and not repaired (§4), so replaying is not what the
     * volume needs -- the last mount that tried is how it came to say ERROR at
     * all, and trying again on every boot would repeat a failure forever. It
     * mounts read-only for fsck (§24) instead. */
    sb.needs_replay = (sb.state == WFS_STATE_DIRTY) ? 1u : 0u;
    if (sb.state == WFS_STATE_ERROR) {
        sb.read_only = 1u;
    }

    sb.generation = wfs_rd64(p, offsetof(struct wfs_superblock, generation));

    *out = sb;
    return WASMOS_ERR_NONE;
}
