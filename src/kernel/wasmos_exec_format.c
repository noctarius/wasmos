#include "wasmos_exec_format.h"
#include <string.h>

#define WASMOS_EXEC_APP_MAGIC "WASMOSAP"
#define WASMOS_EXEC_APP_VERSION 5u

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
} wasmos_exec_wap_header_prefix_t;

static int
wasmos_exec_is_wap_blob(const uint8_t *blob, uint32_t blob_size)
{
    static const uint16_t g_header_sizes[] = {
        0u,
        44u, /* v1 */
        56u, /* v2 */
        60u, /* v3 */
        64u, /* v4 */
        72u, /* v5 */
    };
    const wasmos_exec_wap_header_prefix_t *hdr = 0;
    uint16_t version = 0u;
    uint16_t header_size = 0u;

    if (!blob || blob_size < sizeof(wasmos_exec_wap_header_prefix_t)) {
        return 0;
    }
    hdr = (const wasmos_exec_wap_header_prefix_t *)blob;
    if (memcmp(hdr->magic, WASMOS_EXEC_APP_MAGIC, sizeof(hdr->magic)) != 0) {
        return 0;
    }
    version = hdr->version;
    header_size = hdr->header_size;
    if (version == 0u || version > WASMOS_EXEC_APP_VERSION) {
        return 0;
    }
    if (header_size != g_header_sizes[version]) {
        return 0;
    }
    if ((uint32_t)header_size > blob_size) {
        return 0;
    }
    return 1;
}

uint32_t
wasmos_exec_format_probe_bytes_needed(void)
{
    uint32_t probe_bytes = wasmos_subsystem_registry_exec_max_probe_bytes();

    if (probe_bytes < (uint32_t)sizeof(wasmos_exec_wap_header_prefix_t)) {
        probe_bytes = (uint32_t)sizeof(wasmos_exec_wap_header_prefix_t);
    }
    return probe_bytes;
}

int
wasmos_exec_format_classify(const char *path,
                            const uint8_t *blob,
                            uint32_t blob_size,
                            wasmos_exec_format_match_t *out_match)
{
    wasmos_exec_probe_t probe;
    uint32_t probe_len = 0u;

    if (!out_match) {
        return -1;
    }
    out_match->kind = WASMOS_EXEC_FORMAT_NONE;
    out_match->handler = 0;

    if (wasmos_exec_is_wap_blob(blob, blob_size)) {
        out_match->kind = WASMOS_EXEC_FORMAT_WAP;
        return 0;
    }

    memset(&probe, 0, sizeof(probe));
    probe.path = path;
    probe.initial_bytes = blob;
    probe_len = wasmos_subsystem_registry_exec_max_probe_bytes();
    if (probe_len > blob_size) {
        probe_len = blob_size;
    }
    probe.initial_size = probe_len;
    out_match->handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (out_match->handler) {
        out_match->kind = WASMOS_EXEC_FORMAT_BROKER;
    }
    return 0;
}
