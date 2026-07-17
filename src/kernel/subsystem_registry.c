#include "subsystem_registry.h"
#include "hashmap.h"
#include "klog.h"
#include "kmem.h"
#include "sync/spinlock.h"
#include <string.h>

typedef struct {
    wasmos_subsystem_registry_entry_t* head;
} wasmos_subsystem_bucket_t;

static hashmap_t g_subsystem_map;
static uint8_t g_subsystem_map_initialized;
static ksync_spinlock_t g_subsystem_lock;
static wasmos_exec_handler_registry_entry_t* g_exec_handlers;
static uint32_t g_exec_max_probe_bytes;

static uint32_t subsystem_tag_hash(const char* tag) {
    uint32_t hash = 2166136261u;
    if (!tag) {
        return 0;
    }
    for (uint32_t i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && tag[i] != '\0'; ++i) {
        hash ^= (uint8_t)tag[i];
        hash *= 16777619u;
    }
    return hash;
}

static int subsystem_tag_has_valid_char(char c) {
    return ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || c == '+' || c == '_' ||
           c == '-';
}

static int subsystem_tag_validate_string(const char* tag) {
    if (!tag || tag[0] == '\0') {
        return -1;
    }
    for (uint32_t i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN; ++i) {
        char c = tag[i];
        if (c == '\0') {
            return 0;
        }
        if (!subsystem_tag_has_valid_char(c)) {
            return -1;
        }
    }
    return tag[WASMOS_SUBSYSTEM_TAG_LEN] == '\0' ? 0 : -1;
}

static int subsystem_tag_validate_optional_string(const char* tag) {
    if (!tag || tag[0] == '\0') {
        return 0;
    }
    return subsystem_tag_validate_string(tag);
}

static void copy_subsystem_tag(char* dst, const char* src) {
    if (!dst) {
        return;
    }
    /* Zero-fill the whole field, then truncate-copy the tag into it. */
    memset(dst, 0, WASMOS_SUBSYSTEM_TAG_LEN + 1);
    (void)str_copy(dst, WASMOS_SUBSYSTEM_TAG_LEN + 1, src);
}

static int copy_exec_text(char* dst, uint32_t dst_len, const char* src) {
    if (!dst || dst_len == 0u) {
        return -1;
    }
    memset(dst, 0, dst_len);
    if (!src || src[0] == '\0') {
        return -1;
    }
    /* Reject rather than truncate: str_copy_bytes fails if src does not fit. */
    return str_copy_bytes(dst, dst_len, (const uint8_t*)src, strlen(src));
}

static int subsystem_registry_init_locked(void) {
    if (g_subsystem_map_initialized) {
        return 0;
    }
    if (hashmap_init(&g_subsystem_map, sizeof(wasmos_subsystem_bucket_t), 8) != 0) {
        return -1;
    }
    g_subsystem_map_initialized = 1u;
    return 0;
}

static wasmos_subsystem_registry_entry_t* subsystem_registry_find_locked(const char* request_tag) {
    wasmos_subsystem_bucket_t* bucket = 0;
    wasmos_subsystem_registry_entry_t* entry = 0;

    if (!request_tag || !g_subsystem_map_initialized) {
        return 0;
    }
    bucket =
        (wasmos_subsystem_bucket_t*)hashmap_get(&g_subsystem_map, subsystem_tag_hash(request_tag));
    if (!bucket) {
        return 0;
    }
    for (entry = bucket->head; entry; entry = entry->next) {
        if (strcmp(request_tag, entry->request_tag) == 0) {
            return entry;
        }
    }
    return 0;
}

static const char* exec_probe_filename(const wasmos_exec_probe_t* probe) {
    const char* name = 0;
    const char* p = 0;

    if (!probe) {
        return 0;
    }
    if (probe->filename && probe->filename[0] != '\0') {
        return probe->filename;
    }
    name = probe->path;
    if (!name) {
        return 0;
    }
    for (p = name; *p != '\0'; ++p) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    return name;
}

static int exec_match_text_equals(const char* lhs, const char* rhs, uint8_t rhs_len) {
    uint8_t i = 0;

    if (!lhs || !rhs || rhs_len == 0u) {
        return 0;
    }
    for (i = 0; i < rhs_len; ++i) {
        if (lhs[i] == '\0' || lhs[i] != rhs[i]) {
            return 0;
        }
    }
    return lhs[rhs_len] == '\0';
}

static int exec_match_text_suffix(const char* text, const char* suffix, uint8_t suffix_len) {
    size_t text_len = 0u;
    size_t i = 0u;

    if (!text || !suffix || suffix_len == 0u) {
        return 0;
    }
    text_len = strlen(text);
    if (text_len < (size_t)suffix_len) {
        return 0;
    }
    for (i = 0u; i < (size_t)suffix_len; ++i) {
        if (text[text_len - (size_t)suffix_len + i] != suffix[i]) {
            return 0;
        }
    }
    return 1;
}

static int exec_match_node_eval(const wasmos_exec_match_node_t* nodes, uint32_t node_count,
                                uint32_t node_index, const wasmos_exec_probe_t* probe,
                                uint32_t depth) {
    const wasmos_exec_match_node_t* node = 0;
    const char* filename = 0;

    if (!nodes || !probe || node_index >= node_count || depth > node_count) {
        return 0;
    }
    node = &nodes[node_index];
    switch (node->kind) {
    case WASMOS_EXEC_MATCH_PREFIX:
        if (!probe->initial_bytes || probe->initial_size < (uint32_t)node->value_len) {
            return 0;
        }
        return memcmp(probe->initial_bytes, node->value.prefix, node->value_len) == 0 ? 1 : 0;
    case WASMOS_EXEC_MATCH_EXTENSION:
        filename = exec_probe_filename(probe);
        return exec_match_text_suffix(filename, node->value.text, node->value_len);
    case WASMOS_EXEC_MATCH_FILENAME:
        filename = exec_probe_filename(probe);
        return exec_match_text_equals(filename, node->value.text, node->value_len);
    case WASMOS_EXEC_MATCH_AND:
        return exec_match_node_eval(nodes, node_count, node->left_index, probe, depth + 1u) &&
               exec_match_node_eval(nodes, node_count, node->right_index, probe, depth + 1u);
    case WASMOS_EXEC_MATCH_OR:
        return exec_match_node_eval(nodes, node_count, node->left_index, probe, depth + 1u) ||
               exec_match_node_eval(nodes, node_count, node->right_index, probe, depth + 1u);
    case WASMOS_EXEC_MATCH_NOT:
        return !exec_match_node_eval(nodes, node_count, node->left_index, probe, depth + 1u);
    default:
        return 0;
    }
}

static int exec_match_validate_node(const wasmos_exec_match_node_t* nodes, uint32_t node_count,
                                    uint32_t node_index, uint8_t* visiting, uint8_t* visited,
                                    uint32_t* out_max_prefix) {
    const wasmos_exec_match_node_t* node = 0;

    if (!nodes || !visiting || !visited || !out_max_prefix || node_index >= node_count) {
        return -1;
    }
    if (visiting[node_index]) {
        return -1;
    }
    if (visited[node_index]) {
        return 0;
    }

    node = &nodes[node_index];
    visiting[node_index] = 1u;

    switch (node->kind) {
    case WASMOS_EXEC_MATCH_PREFIX:
        if (node->value_len == 0u || node->value_len > WASMOS_EXEC_MATCH_MAX_BYTES) {
            return -1;
        }
        if (*out_max_prefix < (uint32_t)node->value_len) {
            *out_max_prefix = (uint32_t)node->value_len;
        }
        break;
    case WASMOS_EXEC_MATCH_EXTENSION:
        if (node->value_len == 0u || node->value_len > WASMOS_EXEC_MATCH_TEXT_LEN ||
            node->value.text[0] != '.') {
            return -1;
        }
        break;
    case WASMOS_EXEC_MATCH_FILENAME:
        if (node->value_len == 0u || node->value_len > WASMOS_EXEC_MATCH_TEXT_LEN) {
            return -1;
        }
        break;
    case WASMOS_EXEC_MATCH_AND:
    case WASMOS_EXEC_MATCH_OR:
        if (node->left_index >= node_count || node->right_index >= node_count) {
            return -1;
        }
        if (exec_match_validate_node(nodes, node_count, node->left_index, visiting, visited,
                                     out_max_prefix) != 0 ||
            exec_match_validate_node(nodes, node_count, node->right_index, visiting, visited,
                                     out_max_prefix) != 0) {
            return -1;
        }
        break;
    case WASMOS_EXEC_MATCH_NOT:
        if (node->left_index >= node_count) {
            return -1;
        }
        if (exec_match_validate_node(nodes, node_count, node->left_index, visiting, visited,
                                     out_max_prefix) != 0) {
            return -1;
        }
        break;
    default:
        return -1;
    }

    visiting[node_index] = 0u;
    visited[node_index] = 1u;
    return 0;
}

static int exec_match_validate_tree(const wasmos_exec_match_node_t* nodes, uint32_t node_count,
                                    uint32_t root_index, uint32_t max_probe_bytes) {
    uint8_t visiting[WASMOS_EXEC_MATCH_MAX_NODES];
    uint8_t visited[WASMOS_EXEC_MATCH_MAX_NODES];
    uint32_t max_prefix = 0u;

    if (!nodes || node_count == 0u || node_count > WASMOS_EXEC_MATCH_MAX_NODES ||
        root_index >= node_count) {
        return -1;
    }
    memset(visiting, 0, sizeof(visiting));
    memset(visited, 0, sizeof(visited));
    if (exec_match_validate_node(nodes, node_count, root_index, visiting, visited, &max_prefix) !=
        0) {
        return -1;
    }
    return max_prefix <= max_probe_bytes ? 0 : -1;
}

int wasmos_subsystem_registry_register_builtin(const char* request_tag, const char* runtime_tag,
                                               uint8_t uses_wasm_payload,
                                               uint8_t needs_runtime_lock,
                                               uint8_t gates_ready_for_services,
                                               const wasmos_subsystem_ops_t* ops) {
    wasmos_subsystem_bucket_t* bucket = 0;
    wasmos_subsystem_registry_entry_t* entry = 0;
    if (!request_tag || !runtime_tag || !ops) {
        klog_write("[subsystem] register invalid args\n");
        return -1;
    }
    if (subsystem_tag_validate_string(request_tag) != 0 ||
        subsystem_tag_validate_string(runtime_tag) != 0) {
        klog_write("[subsystem] register invalid tag\n");
        return -1;
    }
    ksync_spinlock_lock(&g_subsystem_lock);
    if (subsystem_registry_init_locked() != 0) {
        klog_write("[subsystem] register map init failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    bucket =
        (wasmos_subsystem_bucket_t*)hashmap_put(&g_subsystem_map, subsystem_tag_hash(request_tag));
    if (!bucket) {
        klog_write("[subsystem] register bucket alloc failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    for (entry = bucket->head; entry; entry = entry->next) {
        if (strcmp(request_tag, entry->request_tag) == 0) {
            klog_printf("[subsystem] duplicate request=%s runtime=%s\n", request_tag, runtime_tag);
            ksync_spinlock_unlock(&g_subsystem_lock);
            return -1;
        }
    }
    entry = (wasmos_subsystem_registry_entry_t*)kmem_alloc(sizeof(*entry));
    if (!entry) {
        klog_write("[subsystem] register entry alloc failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    memset(entry, 0, sizeof(*entry));
    copy_subsystem_tag(entry->request_tag, request_tag);
    copy_subsystem_tag(entry->runtime_tag, runtime_tag);
    entry->kind = WASMOS_SUBSYSTEM_HANDLER_BUILTIN;
    entry->uses_wasm_payload = uses_wasm_payload ? 1u : 0u;
    entry->needs_runtime_lock = needs_runtime_lock ? 1u : 0u;
    entry->gates_ready_for_services = gates_ready_for_services ? 1u : 0u;
    entry->ops = ops;
    entry->next = bucket->head;
    bucket->head = entry;
    klog_printf("[subsystem] register request=%s runtime=%s\n", entry->request_tag,
                entry->runtime_tag);
    ksync_spinlock_unlock(&g_subsystem_lock);
    return 0;
}

/* Count broker-kind subsystem entries across every hashmap bucket.  Requires
 * g_subsystem_lock held and g_subsystem_map initialized. */
static void subsystem_count_brokers_locked(uint32_t owner_context_id, uint32_t* out_total,
                                           uint32_t* out_owner) {
    hashmap_iter_t it;
    uint32_t key = 0;
    uint32_t total = 0u;
    uint32_t owned = 0u;
    for (wasmos_subsystem_bucket_t* bucket =
             (wasmos_subsystem_bucket_t*)hashmap_first(&g_subsystem_map, &it, &key);
         bucket; bucket = (wasmos_subsystem_bucket_t*)hashmap_next(&it, &key)) {
        for (wasmos_subsystem_registry_entry_t* entry = bucket->head; entry; entry = entry->next) {
            if (entry->kind != WASMOS_SUBSYSTEM_HANDLER_BROKER) {
                continue;
            }
            total++;
            if (entry->owner_context_id == owner_context_id) {
                owned++;
            }
        }
    }
    if (out_total) {
        *out_total = total;
    }
    if (out_owner) {
        *out_owner = owned;
    }
}

int wasmos_subsystem_registry_register_broker(const char* request_tag, const char* runtime_tag,
                                              const char* broker_name, uint32_t broker_endpoint,
                                              uint32_t owner_context_id, uint8_t uses_wasm_payload,
                                              uint8_t needs_runtime_lock,
                                              uint8_t gates_ready_for_services) {
    wasmos_subsystem_bucket_t* bucket = 0;
    wasmos_subsystem_registry_entry_t* entry = 0;
    uint32_t broker_total = 0u;
    uint32_t broker_owned = 0u;
    if (!request_tag || !runtime_tag) {
        klog_write("[subsystem] register invalid args\n");
        return -1;
    }
    if (subsystem_tag_validate_string(request_tag) != 0 ||
        subsystem_tag_validate_string(runtime_tag) != 0 ||
        subsystem_tag_validate_optional_string(broker_name) != 0) {
        klog_write("[subsystem] register invalid tag\n");
        return -1;
    }
    ksync_spinlock_lock(&g_subsystem_lock);
    if (subsystem_registry_init_locked() != 0) {
        klog_write("[subsystem] register map init failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    bucket =
        (wasmos_subsystem_bucket_t*)hashmap_put(&g_subsystem_map, subsystem_tag_hash(request_tag));
    if (!bucket) {
        klog_write("[subsystem] register bucket alloc failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    for (entry = bucket->head; entry; entry = entry->next) {
        if (strcmp(request_tag, entry->request_tag) == 0) {
            klog_printf("[subsystem] duplicate request=%s runtime=%s\n", request_tag, runtime_tag);
            ksync_spinlock_unlock(&g_subsystem_lock);
            return -1;
        }
    }
    subsystem_count_brokers_locked(owner_context_id, &broker_total, &broker_owned);
    if (broker_total >= WASMOS_SUBSYSTEM_MAX_BROKERS ||
        (owner_context_id != 0u && broker_owned >= WASMOS_SUBSYSTEM_MAX_BROKERS_PER_OWNER)) {
        klog_printf("[subsystem] broker cap reached total=%u owner=%u\n", broker_total,
                    broker_owned);
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    entry = (wasmos_subsystem_registry_entry_t*)kmem_alloc(sizeof(*entry));
    if (!entry) {
        klog_write("[subsystem] register entry alloc failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    memset(entry, 0, sizeof(*entry));
    copy_subsystem_tag(entry->request_tag, request_tag);
    copy_subsystem_tag(entry->runtime_tag, runtime_tag);
    copy_subsystem_tag(entry->broker_name, broker_name);
    entry->kind = WASMOS_SUBSYSTEM_HANDLER_BROKER;
    entry->owner_context_id = owner_context_id;
    entry->uses_wasm_payload = uses_wasm_payload ? 1u : 0u;
    entry->needs_runtime_lock = needs_runtime_lock ? 1u : 0u;
    entry->gates_ready_for_services = gates_ready_for_services ? 1u : 0u;
    entry->broker_endpoint = broker_endpoint;
    entry->next = bucket->head;
    bucket->head = entry;
    klog_printf("[subsystem] register request=%s runtime=%s broker=%s endpoint=%u\n",
                entry->request_tag, entry->runtime_tag,
                entry->broker_name[0] != '\0' ? entry->broker_name : "-", entry->broker_endpoint);
    ksync_spinlock_unlock(&g_subsystem_lock);
    return 0;
}

int wasmos_subsystem_registry_register_exec_handler(const char* handler_name,
                                                    const char* request_tag,
                                                    uint32_t owner_context_id, uint32_t priority,
                                                    uint32_t max_probe_bytes,
                                                    const wasmos_exec_match_node_t* nodes,
                                                    uint32_t node_count, uint32_t root_index) {
    wasmos_subsystem_registry_entry_t* owner = 0;
    wasmos_exec_handler_registry_entry_t* entry = 0;
    char validated_handler_name[WASMOS_EXEC_HANDLER_NAME_LEN + 1];
    uint32_t handler_total = 0u;
    uint32_t handler_owned = 0u;

    if (!handler_name || !request_tag || !nodes) {
        klog_write("[subsystem] exec handler invalid args\n");
        return -1;
    }
    if (subsystem_tag_validate_string(request_tag) != 0 ||
        exec_match_validate_tree(nodes, node_count, root_index, max_probe_bytes) != 0) {
        klog_write("[subsystem] exec handler invalid matcher\n");
        return -1;
    }
    if (copy_exec_text(validated_handler_name, sizeof(validated_handler_name), handler_name) != 0) {
        klog_write("[subsystem] exec handler invalid name\n");
        return -1;
    }

    ksync_spinlock_lock(&g_subsystem_lock);
    if (subsystem_registry_init_locked() != 0) {
        klog_write("[subsystem] exec handler map init failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    owner = subsystem_registry_find_locked(request_tag);
    if (!owner || owner->kind != WASMOS_SUBSYSTEM_HANDLER_BROKER) {
        klog_write("[subsystem] exec handler owner missing\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    for (entry = g_exec_handlers; entry; entry = entry->next) {
        handler_total++;
        if (entry->owner_context_id == owner_context_id) {
            handler_owned++;
        }
        if (strcmp(entry->handler_name, validated_handler_name) == 0 &&
            strcmp(entry->request_tag, request_tag) == 0) {
            klog_write("[subsystem] exec handler duplicate\n");
            ksync_spinlock_unlock(&g_subsystem_lock);
            return -1;
        }
    }
    if (handler_total >= WASMOS_EXEC_HANDLER_MAX ||
        (owner_context_id != 0u && handler_owned >= WASMOS_EXEC_HANDLER_MAX_PER_OWNER)) {
        klog_printf("[subsystem] exec handler cap reached total=%u owner=%u\n", handler_total,
                    handler_owned);
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }

    entry = (wasmos_exec_handler_registry_entry_t*)kmem_alloc(sizeof(*entry));
    if (!entry) {
        klog_write("[subsystem] exec handler alloc failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    memset(entry, 0, sizeof(*entry));
    entry->nodes = (wasmos_exec_match_node_t*)kmem_alloc(sizeof(*nodes) * node_count);
    if (!entry->nodes) {
        kmem_free(entry);
        klog_write("[subsystem] exec handler nodes alloc failed\n");
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    memset(entry->nodes, 0, sizeof(*nodes) * node_count);
    memcpy(entry->nodes, nodes, sizeof(*nodes) * node_count);
    if (copy_exec_text(entry->handler_name, sizeof(entry->handler_name), validated_handler_name) !=
        0) {
        kmem_free(entry->nodes);
        kmem_free(entry);
        ksync_spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    copy_subsystem_tag(entry->request_tag, owner->request_tag);
    copy_subsystem_tag(entry->runtime_tag, owner->runtime_tag);
    copy_subsystem_tag(entry->broker_name, owner->broker_name);
    entry->broker_endpoint = owner->broker_endpoint;
    entry->priority = priority;
    entry->max_probe_bytes = max_probe_bytes;
    entry->node_count = node_count;
    entry->root_index = root_index;
    entry->owner_context_id = owner_context_id;
    entry->next = g_exec_handlers;
    g_exec_handlers = entry;
    if (g_exec_max_probe_bytes < max_probe_bytes) {
        g_exec_max_probe_bytes = max_probe_bytes;
    }
    klog_printf("[subsystem] exec handler register name=%s subsystem=%s priority=%u probe=%u\n",
                entry->handler_name, entry->request_tag, entry->priority, entry->max_probe_bytes);
    ksync_spinlock_unlock(&g_subsystem_lock);
    return 0;
}

const wasmos_subsystem_registry_entry_t* wasmos_subsystem_registry_find(const char* request_tag) {
    if (!request_tag) {
        return 0;
    }
    ksync_spinlock_lock(&g_subsystem_lock);

    wasmos_subsystem_registry_entry_t* entry = subsystem_registry_find_locked(request_tag);
    if (entry) {
        ksync_spinlock_unlock(&g_subsystem_lock);
        return entry;
    }

    ksync_spinlock_unlock(&g_subsystem_lock);
    return 0;
}

const wasmos_exec_handler_registry_entry_t*
wasmos_subsystem_registry_find_exec_handler(const wasmos_exec_probe_t* probe) {
    wasmos_exec_handler_registry_entry_t* entry = 0;
    wasmos_exec_handler_registry_entry_t* best = 0;

    if (!probe) {
        return 0;
    }
    ksync_spinlock_lock(&g_subsystem_lock);
    for (entry = g_exec_handlers; entry; entry = entry->next) {
        if (!exec_match_node_eval(entry->nodes, entry->node_count, entry->root_index, probe, 0u)) {
            continue;
        }
        if (!best || entry->priority > best->priority ||
            (entry->priority == best->priority &&
             strcmp(entry->handler_name, best->handler_name) < 0) ||
            (entry->priority == best->priority &&
             strcmp(entry->handler_name, best->handler_name) == 0 &&
             strcmp(entry->request_tag, best->request_tag) < 0)) {
            best = entry;
        }
    }
    ksync_spinlock_unlock(&g_subsystem_lock);
    return best;
}

uint32_t wasmos_subsystem_registry_exec_max_probe_bytes(void) {
    uint32_t max_probe_bytes = 0u;

    ksync_spinlock_lock(&g_subsystem_lock);
    max_probe_bytes = g_exec_max_probe_bytes;
    ksync_spinlock_unlock(&g_subsystem_lock);
    return max_probe_bytes;
}

void wasmos_subsystem_registry_reset(void) {
    if (!g_subsystem_map_initialized && !g_exec_handlers) {
        return;
    }
    ksync_spinlock_lock(&g_subsystem_lock);
    if (g_subsystem_map_initialized) {
        hashmap_iter_t it;
        uint32_t key = 0;
        for (wasmos_subsystem_bucket_t* bucket =
                 (wasmos_subsystem_bucket_t*)hashmap_first(&g_subsystem_map, &it, &key);
             bucket; bucket = (wasmos_subsystem_bucket_t*)hashmap_next(&it, &key)) {
            wasmos_subsystem_registry_entry_t* entry = bucket->head;
            while (entry) {
                wasmos_subsystem_registry_entry_t* next = entry->next;
                kmem_free(entry);
                entry = next;
            }
            bucket->head = 0;
        }
        hashmap_destroy(&g_subsystem_map);
        memset(&g_subsystem_map, 0, sizeof(g_subsystem_map));
        g_subsystem_map_initialized = 0u;
    }
    while (g_exec_handlers) {
        wasmos_exec_handler_registry_entry_t* next = g_exec_handlers->next;
        if (g_exec_handlers->nodes) {
            kmem_free(g_exec_handlers->nodes);
        }
        kmem_free(g_exec_handlers);
        g_exec_handlers = next;
    }
    g_exec_max_probe_bytes = 0u;
    ksync_spinlock_unlock(&g_subsystem_lock);
}

void wasmos_subsystem_registry_drop_owner(uint32_t owner_context_id) {
    wasmos_exec_handler_registry_entry_t** link = 0;
    wasmos_exec_handler_registry_entry_t* entry = 0;
    uint32_t recomputed_probe = 0u;

    /* Context 0 is the kernel built-in owner and is never torn down here. */
    if (owner_context_id == 0u) {
        return;
    }
    ksync_spinlock_lock(&g_subsystem_lock);
    if (g_subsystem_map_initialized) {
        hashmap_iter_t it;
        uint32_t key = 0;
        for (wasmos_subsystem_bucket_t* bucket =
                 (wasmos_subsystem_bucket_t*)hashmap_first(&g_subsystem_map, &it, &key);
             bucket; bucket = (wasmos_subsystem_bucket_t*)hashmap_next(&it, &key)) {
            wasmos_subsystem_registry_entry_t** bhead = &bucket->head;
            while (*bhead) {
                wasmos_subsystem_registry_entry_t* cur = *bhead;
                if (cur->kind == WASMOS_SUBSYSTEM_HANDLER_BROKER &&
                    cur->owner_context_id == owner_context_id) {
                    *bhead = cur->next;
                    kmem_free(cur);
                    continue;
                }
                bhead = &cur->next;
            }
        }
    }
    link = &g_exec_handlers;
    while (*link) {
        entry = *link;
        if (entry->owner_context_id == owner_context_id) {
            *link = entry->next;
            if (entry->nodes) {
                kmem_free(entry->nodes);
            }
            kmem_free(entry);
            continue;
        }
        if (entry->max_probe_bytes > recomputed_probe) {
            recomputed_probe = entry->max_probe_bytes;
        }
        link = &entry->next;
    }
    g_exec_max_probe_bytes = recomputed_probe;
    ksync_spinlock_unlock(&g_subsystem_lock);
}
