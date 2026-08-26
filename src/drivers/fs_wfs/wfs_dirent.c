/* wfs_dirent.c - directory record surgery inside one block (§10). */
#include "wfs_dirent.h"

#include <stddef.h>

#include "wfs_crc32c.h"

static uint32_t rd16(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8);
}

static void wr16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint64_t rd64(const uint8_t* p, uint32_t off) {
    uint64_t v = 0;
    uint32_t i;

    for (i = 0; i < 8u; ++i) {
        v |= (uint64_t)p[off + i] << (i * 8u);
    }
    return v;
}

static void wr64(uint8_t* p, uint32_t off, uint64_t v) {
    uint32_t i;

    for (i = 0; i < 8u; ++i) {
        p[off + i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
    }
}

static void wr32(uint8_t* p, uint32_t off, uint32_t v) {
    wr16(p, off, (uint16_t)(v & 0xFFFFu));
    wr16(p, off + 2u, (uint16_t)((v >> 16) & 0xFFFFu));
}

/* Bytes a record genuinely occupies, as opposed to the stride it claims. Zero for
 * free space: a record whose object_id or name_length is 0 holds nothing, so all
 * of its stride is available (§10). */
static uint32_t record_used(const uint8_t* block, uint32_t off) {
    uint64_t id = rd64(block, off);
    uint32_t name_length = block[off + 10u];

    if (id == 0u || name_length == 0u) {
        return 0u;
    }
    return wfs_dir_record_length(name_length);
}

wasmos_error_code_t wfs_dirent_validate(const uint8_t* block, uint32_t block_size) {
    uint32_t usable;
    uint32_t off = 0u;

    if (!block || block_size <= WFS_DIR_TAIL_SIZE) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    usable = wfs_dir_usable_bytes(block_size);
    while (off + WFS_DIR_ENTRY_HEADER <= usable) {
        uint32_t len = rd16(block, off + 8u);
        uint32_t name_length = block[off + 10u];

        /* The stride is checked before it is used. Zero above all: a scan would
         * never advance, which is why a zeroed block is not a valid one. */
        if (len < WFS_DIR_RECORD_MIN || (len & 7u) != 0u || off + len > usable) {
            return WASMOS_ERR_FS_CORRUPT;
        }
        if (name_length > len - WFS_DIR_ENTRY_HEADER) {
            return WASMOS_ERR_FS_CORRUPT;
        }
        off += len;
    }
    /* The records must cover the usable area exactly, so a scan ends where the
     * tail begins (§10). Stopping short would leave bytes no scan reaches. */
    return off == usable ? WASMOS_ERR_NONE : WASMOS_ERR_FS_CORRUPT;
}

void wfs_dirent_seal(uint8_t* block, uint32_t block_size, const uint8_t* uuid, uint64_t location) {
    uint32_t at;

    if (!block || block_size <= WFS_DIR_TAIL_SIZE) {
        return;
    }
    at = wfs_dir_usable_bytes(block_size) + (uint32_t)offsetof(struct wfs_dir_tail, checksum);
    wr32(block, at, 0u);
    wr32(block, at, wfs_checksum_struct(uuid, location, block, block_size, at));
}

int32_t wfs_dirent_find(const uint8_t* block, uint32_t block_size, const char* name,
                        uint32_t name_len) {
    uint32_t usable;
    uint32_t off = 0u;

    if (!block || !name || name_len == 0u || block_size <= WFS_DIR_TAIL_SIZE) {
        return -1;
    }
    usable = wfs_dir_usable_bytes(block_size);
    while (off + WFS_DIR_ENTRY_HEADER <= usable) {
        uint32_t len = rd16(block, off + 8u);
        uint32_t nl = block[off + 10u];

        if (len < WFS_DIR_RECORD_MIN || (len & 7u) != 0u || off + len > usable) {
            return -1;
        }
        if (rd64(block, off) != 0u && nl == name_len) {
            uint32_t i;
            uint32_t same = 1u;

            for (i = 0; i < name_len; ++i) {
                if (block[off + WFS_DIR_ENTRY_HEADER + i] != (uint8_t)name[i]) {
                    same = 0u;
                    break;
                }
            }
            if (same) {
                return (int32_t)off;
            }
        }
        off += len;
    }
    return -1;
}

void wfs_dirent_init_block(uint8_t* block, uint32_t block_size, const uint8_t* uuid,
                           uint64_t location) {
    uint32_t usable;
    uint32_t i;

    if (!block || block_size <= WFS_DIR_TAIL_SIZE) {
        return;
    }
    usable = wfs_dir_usable_bytes(block_size);
    for (i = 0; i < block_size; ++i) {
        block[i] = 0u;
    }
    /* One free record spanning everything before the tail. object_id 0 already
     * marks it free; what a zeroed block lacks is the STRIDE. */
    wr16(block, 8u, (uint16_t)usable);
    /* And the tail, laid out as a record so a scan skips it under the free-space
     * rule rather than needing to know about it. */
    wr16(block, usable + 8u, (uint16_t)WFS_DIR_TAIL_SIZE);
    block[usable + 11u] = (uint8_t)WFS_DIR_TAIL_TYPE;
    wfs_dirent_seal(block, block_size, uuid, location);
}

wasmos_error_code_t wfs_dirent_insert(uint8_t* block, uint32_t block_size, const uint8_t* uuid,
                                      uint64_t location, const char* name, uint32_t name_len,
                                      uint32_t object_id, uint8_t type) {
    uint32_t usable;
    uint32_t need;
    uint32_t off = 0u;
    wasmos_error_code_t rc;

    if (!block || !name || block_size <= WFS_DIR_TAIL_SIZE) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (name_len == 0u || name_len > WFS_NAME_MAX) {
        return WASMOS_ERR_FS_NAME;
    }
    if (object_id == WFS_OBJECT_INVALID) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    rc = wfs_dirent_validate(block, block_size);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    /* Checked before anything is written, so a refused insert leaves the block
     * exactly as it was and a retry cannot leak a record. */
    if (wfs_dirent_find(block, block_size, name, name_len) >= 0) {
        return WASMOS_ERR_FS_EXISTS;
    }

    usable = wfs_dir_usable_bytes(block_size);
    need = wfs_dir_record_length(name_len);

    while (off + WFS_DIR_ENTRY_HEADER <= usable) {
        uint32_t len = rd16(block, off + 8u);
        uint32_t used = record_used(block, off);
        uint32_t avail = len - used;

        /* The space is either a free record's whole stride or the SLACK inside a
         * used one. A directory mkfs_wfs wrote has only the latter: its last
         * record's stride reaches the tail. */
        if (avail >= need) {
            uint32_t at = off + used;
            uint32_t take = avail;

            if (avail - need >= WFS_DIR_RECORD_MIN) {
                /* Leave the remainder as a free record rather than padding this
                 * one, so the space stays reusable. */
                take = need;
                wr64(block, at + take, 0u);
                wr16(block, at + take + 8u, (uint16_t)(avail - need));
                block[at + take + 10u] = 0u;
                block[at + take + 11u] = 0u;
            }
            if (used != 0u) {
                wr16(block, off + 8u, (uint16_t)used);
            }
            wr64(block, at, (uint64_t)object_id);
            wr16(block, at + 8u, (uint16_t)take);
            block[at + 10u] = (uint8_t)name_len;
            block[at + 11u] = type;
            for (uint32_t i = 0; i < name_len; ++i) {
                block[at + WFS_DIR_ENTRY_HEADER + i] = (uint8_t)name[i];
            }
            /* Bytes between the name and the end of the record are zero (§10), so
             * the image stays fully defined and the checksum reproducible. */
            for (uint32_t i = WFS_DIR_ENTRY_HEADER + name_len; i < take; ++i) {
                block[at + i] = 0u;
            }
            wfs_dirent_seal(block, block_size, uuid, location);
            return WASMOS_ERR_NONE;
        }
        off += len;
    }
    return WASMOS_ERR_FS_NO_SPACE;
}

/* Coalesce runs of adjacent free records into one, in a single forward pass.
 *
 * Without this a directory fragments into holes too small to hold a record, and a
 * long name can never be inserted again even though the bytes are there. */
static void merge_free(uint8_t* block, uint32_t block_size) {
    uint32_t usable = wfs_dir_usable_bytes(block_size);
    uint32_t off = 0u;

    while (off + WFS_DIR_ENTRY_HEADER <= usable) {
        uint32_t len = rd16(block, off + 8u);

        if (record_used(block, off) == 0u) {
            /* Absorb every free record that follows, stopping at the first used
             * one or at the tail. */
            uint32_t next = off + len;

            while (next + WFS_DIR_ENTRY_HEADER <= usable && record_used(block, next) == 0u) {
                uint32_t nlen = rd16(block, next + 8u);

                if (nlen < WFS_DIR_RECORD_MIN || (nlen & 7u) != 0u || next + nlen > usable) {
                    break;
                }
                len += nlen;
                next += nlen;
            }
            wr64(block, off, 0u);
            wr16(block, off + 8u, (uint16_t)len);
            block[off + 10u] = 0u;
            block[off + 11u] = 0u;
        }
        off += len;
    }
}

wasmos_error_code_t wfs_dirent_remove(uint8_t* block, uint32_t block_size, const uint8_t* uuid,
                                      uint64_t location, const char* name, uint32_t name_len) {
    int32_t at;
    wasmos_error_code_t rc;

    if (!block || !name || block_size <= WFS_DIR_TAIL_SIZE) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    rc = wfs_dirent_validate(block, block_size);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    at = wfs_dirent_find(block, block_size, name, name_len);
    if (at < 0) {
        return WASMOS_ERR_FS_NOT_FOUND;
    }
    /* Free space is object_id 0 with no name; the stride stays, so the chain is
     * unbroken between marking and merging. */
    wr64(block, (uint32_t)at, 0u);
    block[(uint32_t)at + 10u] = 0u;
    block[(uint32_t)at + 11u] = 0u;
    merge_free(block, block_size);
    wfs_dirent_seal(block, block_size, uuid, location);
    return WASMOS_ERR_NONE;
}
