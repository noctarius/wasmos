#include "subsystem_registry.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const int g_ops_a = 1;
static const int g_ops_b = 2;

static uint32_t
test_subsystem_tag_hash(const char *tag)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && tag[i] != '\0'; ++i) {
        hash ^= (uint8_t)tag[i];
        hash *= 16777619u;
    }
    return hash;
}

static int
test_collision_bucket_lookup(void)
{
    const char *tag_a = "H67";
    const char *tag_b = "WTAA";
    const wasmos_subsystem_registry_entry_t *entry_a = 0;
    const wasmos_subsystem_registry_entry_t *entry_b = 0;
    const wasmos_subsystem_ops_t *ops_a = (const wasmos_subsystem_ops_t *)&g_ops_a;
    const wasmos_subsystem_ops_t *ops_b = (const wasmos_subsystem_ops_t *)&g_ops_b;

    if (test_subsystem_tag_hash(tag_a) != test_subsystem_tag_hash(tag_b)) return __LINE__;

    wasmos_subsystem_registry_reset();
    if (wasmos_subsystem_registry_register(tag_a, "WARP", ops_a) != 0) return __LINE__;
    if (wasmos_subsystem_registry_register(tag_b, "NATIVE", ops_b) != 0) return __LINE__;

    entry_a = wasmos_subsystem_registry_find(tag_a);
    entry_b = wasmos_subsystem_registry_find(tag_b);
    if (!entry_a || !entry_b) return __LINE__;
    if (strcmp(entry_a->request_tag, tag_a) != 0) return __LINE__;
    if (strcmp(entry_b->request_tag, tag_b) != 0) return __LINE__;
    if (strcmp(entry_a->runtime_tag, "WARP") != 0) return __LINE__;
    if (strcmp(entry_b->runtime_tag, "NATIVE") != 0) return __LINE__;
    if (entry_a->ops != ops_a) return __LINE__;
    if (entry_b->ops != ops_b) return __LINE__;
    if (wasmos_subsystem_registry_register(tag_a, "WARP", ops_a) == 0) return __LINE__;

    wasmos_subsystem_registry_reset();
    return 0;
}

int
main(void)
{
    int rc = test_collision_bucket_lookup();
    if (rc != 0) {
        return rc;
    }
    printf("test_subsystem_registry: ok\n");
    return 0;
}
