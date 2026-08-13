#include "wasmos_exec_format.h"
#include <string.h>

#define WASMOS_EXEC_APP_MAGIC "WASMOSAP"
#define WASMOS_EXEC_APP_VERSION 5u

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
} wasmos_exec_wap_header_prefix_t;

/* Whether `blob` starts with a WASMOS-APP container header: right magic, a known
 * version, and the exact header size that version defines.
 *
 * FIXME(wap-v6): WASMOS_EXEC_APP_VERSION and g_header_sizes stop at v5, but
 * WASMOS_APP_VERSION (src/kernel/include/wasmos_app.h) and the packer
 * (scripts/make_wasmos_app.c) are at v6 (header_size 76), so every .wap produced
 * today is rejected here.  That silently downgrades the second classify() in
 * pm_resolve_spawn_target (process_manager_spawn.c) from WAP to NONE, which
 * fails broker-delegated spawns with WASMOS_ERR_PROC_SPAWN_BROKER_PLAN. The
 * table must track WASMOS_APP_VERSION. */
static int wasmos_exec_is_wap_blob(const uint8_t* blob, uint32_t blob_size) {
    static const uint16_t g_header_sizes[] = {
        0u,  44u, /* v1 */
        56u,      /* v2 */
        60u,      /* v3 */
        64u,      /* v4 */
        72u,      /* v5 */
    };
    const wasmos_exec_wap_header_prefix_t* hdr = 0;
    uint16_t version = 0u;
    uint16_t header_size = 0u;

    if (!blob || blob_size < sizeof(wasmos_exec_wap_header_prefix_t)) {
        return 0;
    }
    hdr = (const wasmos_exec_wap_header_prefix_t*)blob;
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

uint32_t wasmos_exec_format_probe_bytes_needed(void) {
    uint32_t probe_bytes = wasmos_subsystem_registry_exec_max_probe_bytes();

    if (probe_bytes < (uint32_t)sizeof(wasmos_exec_wap_header_prefix_t)) {
        probe_bytes = (uint32_t)sizeof(wasmos_exec_wap_header_prefix_t);
    }
    return probe_bytes;
}

int wasmos_exec_format_classify(const char* path, const uint8_t* blob, uint32_t blob_size,
                                wasmos_exec_format_match_t* out_match) {
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

static int exec_plan_string_region(const uint8_t* plan_bytes, uint32_t plan_size, uint32_t offset,
                                   uint32_t len, const char** out_text) {
    const char* text = 0;

    if (!plan_bytes || !out_text) {
        return -1;
    }
    *out_text = 0;
    if (len == 0u) {
        return 0;
    }
    if (offset >= plan_size || len > (plan_size - offset)) {
        return -1;
    }
    text = (const char*)(plan_bytes + offset);
    for (uint32_t i = 0; i < len; ++i) {
        if (text[i] == '\0') {
            return -1;
        }
    }
    if (offset + len >= plan_size || text[len] != '\0') {
        return -1;
    }
    *out_text = text;
    return 0;
}

int wasmos_exec_broker_plan_validate(const uint8_t* plan_bytes, uint32_t plan_size,
                                     const wasmos_exec_handler_registry_entry_t* handler,
                                     wasmos_exec_broker_plan_t* out_plan) {
    const wasmos_broker_spawn_plan_response_t* plan = 0;
    const char* host_path = 0;
    const char* host_args = 0;

    if (!plan_bytes || !handler || !out_plan) {
        return -1;
    }
    memset(out_plan, 0, sizeof(*out_plan));
    if (plan_size < sizeof(wasmos_broker_spawn_plan_response_t)) {
        return -1;
    }
    plan = (const wasmos_broker_spawn_plan_response_t*)plan_bytes;
    if (plan->version != WASMOS_BROKER_SPAWN_PLAN_VERSION ||
        plan->plan_kind != WASMOS_BROKER_PLAN_KIND_WAP_PATH) {
        return -1;
    }
    if (strcmp(plan->request_tag, handler->request_tag) != 0 ||
        strcmp(plan->runtime_tag, handler->runtime_tag) != 0) {
        return -1;
    }
    if (exec_plan_string_region(plan_bytes, plan_size, plan->host_path_offset, plan->host_path_len,
                                &host_path) != 0 ||
        !host_path) {
        return -1;
    }
    if (plan->host_path_len < 4u || memcmp(host_path + plan->host_path_len - 4u, ".wap", 4u) != 0) {
        return -1;
    }
    if (exec_plan_string_region(plan_bytes, plan_size, plan->host_args_offset, plan->host_args_len,
                                &host_args) != 0) {
        return -1;
    }
    out_plan->host_path = host_path;
    out_plan->host_path_len = plan->host_path_len;
    out_plan->host_args = host_args;
    out_plan->host_args_len = plan->host_args_len;
    out_plan->plan_flags = plan->plan_flags;
    return 0;
}
