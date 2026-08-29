/* wfs_crc32c.c - CRC32C over a 256-entry table.
 *
 * Byte-at-a-time against a 1 KiB table rather than slicing-by-8 against 8 KiB:
 * the driver's working set competes with staged metadata blocks in a wasm
 * module's linear memory, and metadata checksums are not the hot path that
 * would justify the eight-fold table.
 */
#include "wfs_crc32c.h"

/* CRC-32C reflected polynomial (RFC 3720). */
#define WFS_CRC32C_POLY 0x82F63B78u

static uint32_t g_table[256];
static int g_table_ready;

static void build_table(void) {
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (WFS_CRC32C_POLY & (uint32_t)(-(int32_t)(crc & 1u)));
        }
        g_table[i] = crc;
    }
    g_table_ready = 1;
}

uint32_t wfs_crc32c_update(uint32_t crc, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;

    if (!g_table_ready) {
        build_table();
    }
    if (!p) {
        return crc;
    }
    for (uint32_t i = 0; i < len; ++i) {
        crc = g_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t wfs_crc32c(const void* data, uint32_t len) {
    return wfs_crc32c_finish(wfs_crc32c_update(WFS_CRC32C_INIT, data, len));
}

uint32_t wfs_crc32c_seed(const uint8_t uuid[WFS_UUID_LEN], uint64_t location) {
    uint8_t le[8];
    uint32_t crc;

    /* Serialised explicitly rather than by casting the uint64: the on-disk
     * format is little-endian everywhere, and a big-endian host tool must
     * produce the same seed as the driver. */
    for (int i = 0; i < 8; ++i) {
        le[i] = (uint8_t)((location >> (i * 8)) & 0xFFu);
    }

    crc = wfs_crc32c_update(WFS_CRC32C_INIT, uuid, WFS_UUID_LEN);
    return wfs_crc32c_update(crc, le, sizeof(le));
}

uint32_t wfs_checksum_struct(const uint8_t uuid[WFS_UUID_LEN], uint64_t location, const void* base,
                             uint32_t size, uint32_t checksum_offset) {
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    const uint8_t* p = (const uint8_t*)base;
    uint32_t crc;

    if (!p || checksum_offset + 4u > size) {
        return 0;
    }

    crc = wfs_crc32c_seed(uuid, location);
    crc = wfs_crc32c_update(crc, p, checksum_offset);
    crc = wfs_crc32c_update(crc, zeros, sizeof(zeros));
    crc = wfs_crc32c_update(crc, p + checksum_offset + 4u, size - checksum_offset - 4u);
    return wfs_crc32c_finish(crc);
}
