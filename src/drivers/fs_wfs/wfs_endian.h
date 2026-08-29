/* wfs_endian.h - little-endian field access over a raw block image.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §2 "Byte Order".
 *
 * Every on-disk field is little-endian whatever the host is, and a staged block
 * is a byte image whose alignment is the staging buffer's rather than a
 * structure's. Both rule out casting the buffer to `struct wfs_*` and reading
 * fields through it: on a big-endian host the values would be wrong, and on any
 * host an unaligned member access is undefined. Fields are therefore assembled
 * and taken apart a byte at a time, here and nowhere else.
 *
 * `off` is a byte offset from `p`, normally an `offsetof` into the structure the
 * block holds. Nothing here bounds-checks it: the caller knows the block size
 * and the structure it is addressing, and a check that cannot say what the limit
 * is would only hide the mistake.
 *
 * `static inline` rather than linked functions, so a translation unit that uses
 * only some of them carries no unused-function warning and no dead code.
 */
#ifndef FS_WFS_WFS_ENDIAN_H
#define FS_WFS_WFS_ENDIAN_H

#include <stdint.h>

static inline uint16_t wfs_rd16(const uint8_t* p, uint32_t off) {
    return (uint16_t)((uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8));
}

static inline uint32_t wfs_rd32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) | ((uint32_t)p[off + 2u] << 16) |
           ((uint32_t)p[off + 3u] << 24);
}

static inline uint64_t wfs_rd64(const uint8_t* p, uint32_t off) {
    return (uint64_t)wfs_rd32(p, off) | ((uint64_t)wfs_rd32(p, off + 4u) << 32);
}

static inline void wfs_wr16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1u] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void wfs_wr32(uint8_t* p, uint32_t off, uint32_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    p[off + 2u] = (uint8_t)((v >> 16) & 0xFFu);
    p[off + 3u] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void wfs_wr64(uint8_t* p, uint32_t off, uint64_t v) {
    wfs_wr32(p, off, (uint32_t)(v & 0xFFFFFFFFu));
    wfs_wr32(p, off + 4u, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

#endif /* FS_WFS_WFS_ENDIAN_H */
