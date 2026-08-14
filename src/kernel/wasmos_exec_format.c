#include "wasmos_exec_format.h"
#include <string.h>

#define WASMOS_EXEC_APP_MAGIC "WASMOSAP"

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
} wasmos_exec_wap_header_prefix_t;

/* Whether `blob` starts with a WASMOS-APP container header: right magic, a known
 * version, and the exact header size that version defines.
 *
 * g_header_sizes is indexed by version and MUST carry an entry for every version
 * up to WASMOS_APP_VERSION; the static assert below enforces that. A missing
 * entry does not reject the package -- it reports "not a WAP", which downgrades
 * the classify() in pm_resolve_spawn_target from WAP to NONE and fails
 * broker-delegated spawns with WASMOS_ERR_PROC_SPAWN_BROKER_PLAN. */
static int wasmos_exec_is_wap_blob(const uint8_t* blob, uint32_t blob_size) {
    static const uint16_t g_header_sizes[] = {
        0u,  44u, /* v1 */
        56u,      /* v2 */
        60u,      /* v3 */
        64u,      /* v4 */
        72u,      /* v5 */
        76u,      /* v6: adds region_count */
    };
    /* One entry per version, plus the unused index 0. */
    _Static_assert(sizeof(g_header_sizes) / sizeof(g_header_sizes[0]) ==
                       (size_t)WASMOS_EXEC_APP_VERSION + 1u,
                   "g_header_sizes must carry an entry for every version up to "
                   "WASMOS_EXEC_APP_VERSION");
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

/* How many leading bytes of a candidate executable a caller must read before
 * wasmos_exec_format_classify can decide.  It is the largest probe any
 * registered exec handler asks for, floored at the WAP header prefix so the
 * built-in container check always has enough — so the value CHANGES as handlers
 * are registered or the registry is reset, and must be re-read per classify
 * rather than cached. */
uint32_t wasmos_exec_format_probe_bytes_needed(void) {
    uint32_t probe_bytes = wasmos_subsystem_registry_exec_max_probe_bytes();

    if (probe_bytes < (uint32_t)sizeof(wasmos_exec_wap_header_prefix_t)) {
        probe_bytes = (uint32_t)sizeof(wasmos_exec_wap_header_prefix_t);
    }
    return probe_bytes;
}

/* Decides how a blob should be executed: as a native WAP container, by a
 * registered broker handler, or not at all.
 *
 * The built-in WAP check runs first and wins; only a blob that is not a WAP is
 * offered to the exec handlers, with `path` and up to
 * wasmos_exec_format_probe_bytes_needed() leading bytes as the probe.  A blob
 * shorter than that is probed with what there is.
 *
 * Returns 0 whenever it produced an answer — including WASMOS_EXEC_FORMAT_NONE,
 * which means "nothing claims this", NOT an error — and -1 only for a NULL
 * out_match.  *out_match is cleared first, so it is defined on every 0 return.
 * A BROKER match leaves out_match->handler pointing at a registry entry, which
 * stays valid only until the registry changes.  blob and path are borrowed. */
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

/* Validates a spawn plan returned by a broker and projects it into *out_plan.
 *
 * The plan arrives as untrusted bytes from a user-space broker, so everything is
 * checked: the response is at least header-sized, its version and plan kind are
 * the ones this build understands, its request and runtime tags match the
 * handler that produced it, and the host path ends in ".wap".
 *
 * Both embedded strings are bounds-checked against plan_size, must contain no
 * interior NUL, and must be followed by one inside the buffer — so out_plan's
 * host_path and host_args are NUL-terminated pointers INTO plan_bytes, borrowed,
 * and valid only as long as that buffer is.  A zero-length host_args yields a
 * NULL pointer rather than an empty string.
 *
 * Returns 0 when the plan is accepted and -1 on any failure; *out_plan is
 * cleared first, so a rejected plan leaves it empty rather than half-filled. */
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
