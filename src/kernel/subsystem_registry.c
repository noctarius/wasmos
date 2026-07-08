#include "subsystem_registry.h"
#include "hashmap.h"
#include "kmem.h"
#include "spinlock.h"
#include <string.h>

typedef struct {
    wasmos_subsystem_registry_entry_t *head;
} wasmos_subsystem_bucket_t;

static hashmap_t g_subsystem_map;
static uint8_t g_subsystem_map_initialized;
static spinlock_t g_subsystem_lock;

static uint32_t
subsystem_tag_hash(const char *tag)
{
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

static int
subsystem_tag_has_valid_char(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ||
           ((c >= '0') && (c <= '9')) ||
           c == '+' || c == '_' || c == '-';
}

static int
subsystem_tag_validate_string(const char *tag)
{
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

static void
copy_subsystem_tag(char *dst, const char *src)
{
    if (!dst) {
        return;
    }
    for (uint32_t i = 0; i <= WASMOS_SUBSYSTEM_TAG_LEN; ++i) {
        dst[i] = '\0';
    }
    if (!src) {
        return;
    }
    for (uint32_t i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
}

static int
subsystem_registry_init_locked(void)
{
    if (g_subsystem_map_initialized) {
        return 0;
    }
    if (hashmap_init(&g_subsystem_map, sizeof(wasmos_subsystem_bucket_t), 8) != 0) {
        return -1;
    }
    g_subsystem_map_initialized = 1u;
    return 0;
}

int
wasmos_subsystem_registry_register(const char *request_tag,
                                   const char *runtime_tag,
                                   const wasmos_subsystem_ops_t *ops)
{
    wasmos_subsystem_bucket_t *bucket = 0;
    wasmos_subsystem_registry_entry_t *entry = 0;
    if (!request_tag || !runtime_tag || !ops) {
        return -1;
    }
    if (subsystem_tag_validate_string(request_tag) != 0 ||
        subsystem_tag_validate_string(runtime_tag) != 0) {
        return -1;
    }
    spinlock_lock(&g_subsystem_lock);
    if (subsystem_registry_init_locked() != 0) {
        spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    bucket = (wasmos_subsystem_bucket_t *)hashmap_put(&g_subsystem_map, subsystem_tag_hash(request_tag));
    if (!bucket) {
        spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    for (entry = bucket->head; entry; entry = entry->next) {
        if (strcmp(request_tag, entry->request_tag) == 0) {
            spinlock_unlock(&g_subsystem_lock);
            return -1;
        }
    }
    entry = (wasmos_subsystem_registry_entry_t *)kmem_alloc(sizeof(*entry));
    if (!entry) {
        spinlock_unlock(&g_subsystem_lock);
        return -1;
    }
    memset(entry, 0, sizeof(*entry));
    copy_subsystem_tag(entry->request_tag, request_tag);
    copy_subsystem_tag(entry->runtime_tag, runtime_tag);
    entry->ops = ops;
    entry->next = bucket->head;
    bucket->head = entry;
    spinlock_unlock(&g_subsystem_lock);
    return 0;
}

const wasmos_subsystem_registry_entry_t *
wasmos_subsystem_registry_find(const char *request_tag)
{
    wasmos_subsystem_bucket_t *bucket = 0;
    wasmos_subsystem_registry_entry_t *entry = 0;
    if (!request_tag) {
        return 0;
    }
    spinlock_lock(&g_subsystem_lock);
    if (!g_subsystem_map_initialized) {
        spinlock_unlock(&g_subsystem_lock);
        return 0;
    }
    bucket = (wasmos_subsystem_bucket_t *)hashmap_get(&g_subsystem_map, subsystem_tag_hash(request_tag));
    if (!bucket) {
        spinlock_unlock(&g_subsystem_lock);
        return 0;
    }
    for (entry = bucket->head; entry; entry = entry->next) {
        if (strcmp(request_tag, entry->request_tag) == 0) {
            spinlock_unlock(&g_subsystem_lock);
            return entry;
        }
    }
    spinlock_unlock(&g_subsystem_lock);
    return 0;
}

void
wasmos_subsystem_registry_reset(void)
{
    if (!g_subsystem_map_initialized) {
        return;
    }
    spinlock_lock(&g_subsystem_lock);
    if (g_subsystem_map_initialized) {
        hashmap_iter_t it;
        uint32_t key = 0;
        for (wasmos_subsystem_bucket_t *bucket =
                 (wasmos_subsystem_bucket_t *)hashmap_first(&g_subsystem_map, &it, &key);
             bucket;
             bucket = (wasmos_subsystem_bucket_t *)hashmap_next(&it, &key)) {
            wasmos_subsystem_registry_entry_t *entry = bucket->head;
            while (entry) {
                wasmos_subsystem_registry_entry_t *next = entry->next;
                kmem_free(entry);
                entry = next;
            }
            bucket->head = 0;
        }
        hashmap_destroy(&g_subsystem_map);
        memset(&g_subsystem_map, 0, sizeof(g_subsystem_map));
        g_subsystem_map_initialized = 0u;
    }
    spinlock_unlock(&g_subsystem_lock);
}
