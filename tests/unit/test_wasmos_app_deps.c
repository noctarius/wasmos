/* Host unit test for the .wap v10 dependency table in wasmos_app_parse()
 * (src/kernel/wasmos_app.c): records are parsed into wasmos_app_desc_t.deps,
 * bounds-checked against the blob and the registry's name limits, and walked
 * so every later section still lands at the right offset.
 *
 * wasmos_app.c is compiled in for real with the subsystem registry under it;
 * the runtime start hooks are stubbed (stubs_app_runtime.c), since no case
 * starts a package. Blobs are built byte by byte in the packer's section order.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all stops
 * at the first failure (test_shuffle.h). */

#include "wasmos_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

/* Mirrors wasmos_app_header_t in wasmos_app.c and scripts/make_wasmos_app.c. */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t dep_count;
    uint32_t cap_count;
    uint32_t mem_hint_count;
    uint32_t compiled_size;
    char subsystem_tag[8];
    uint32_t region_count;
} test_header_t;

typedef struct {
    const char* name;
    uint32_t name_len; /* written as the record's u16; strlen(name) when 0 */
    uint8_t strength;
    uint8_t kind;
} test_dep_t;

static uint8_t g_blob[2048];

static uint32_t put(uint32_t off, const void* bytes, uint32_t len) {
    memcpy(&g_blob[off], bytes, len);
    return off + len;
}

/* Build a v10 app package: name "t", entry "m", the given dependency records,
 * one "ipc.basic" capability, no regions, a stack and a heap hint, then a
 * 4-byte payload "\0asm". Returns the blob length. */
static uint32_t build(uint16_t version, uint32_t dep_count_field, const test_dep_t* deps,
                      uint32_t ndeps) {
    test_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "WASMOSAP", 8);
    hdr.version = version;
    hdr.header_size = (uint16_t)sizeof(hdr);
    hdr.flags = WASMOS_APP_FLAG_APP;
    hdr.name_len = 1;
    hdr.entry_len = 1;
    hdr.wasm_size = 4;
    hdr.dep_count = dep_count_field;
    hdr.cap_count = 1;
    hdr.mem_hint_count = 2;
    memcpy(hdr.subsystem_tag, "WASM", 4);

    uint32_t off = put(0, &hdr, sizeof(hdr));
    off = put(off, "t", 1);
    off = put(off, "m", 1);
    for (uint32_t i = 0; i < ndeps; ++i) {
        uint32_t len = deps[i].name_len ? deps[i].name_len : (uint32_t)strlen(deps[i].name);
        uint16_t len16 = (uint16_t)len;
        off = put(off, &deps[i].strength, 1);
        off = put(off, &deps[i].kind, 1);
        off = put(off, &len16, 2);
        off = put(off, deps[i].name, (uint32_t)strlen(deps[i].name));
    }
    uint32_t cap[2] = {9u, 0u};
    off = put(off, cap, sizeof(cap));
    off = put(off, "ipc.basic", 9);
    uint32_t hints[6] = {WASMOS_APP_MEM_HINT_STACK, 3u, 0u, WASMOS_APP_MEM_HINT_HEAP, 5u, 0u};
    off = put(off, hints, sizeof(hints));
    off = put(off, "\0asm", 4);
    return off;
}

static int names_equal(const wasmos_app_dep_t* d, const char* want) {
    return d->name_len == strlen(want) && memcmp(d->name, want, d->name_len) == 0;
}

static int test_records_parse_and_later_sections_follow(void) {
    const test_dep_t deps[] = {
        {"fs.vfs", 0, WASMOS_APP_DEP_NEEDS, WASMOS_APP_DEP_KIND_NAME},
        {"block", 0, WASMOS_APP_DEP_WANTS, WASMOS_APP_DEP_KIND_CLASS},
        {"gfx", 0, WASMOS_APP_DEP_PROVIDES, WASMOS_APP_DEP_KIND_NAME},
    };
    uint32_t len = build(WASMOS_APP_VERSION, 3, deps, 3);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0) {
        return __LINE__;
    }
    if (desc.dep_count != 3) {
        return __LINE__;
    }
    for (uint32_t i = 0; i < 3; ++i) {
        if (desc.deps[i].strength != deps[i].strength || desc.deps[i].kind != deps[i].kind ||
            !names_equal(&desc.deps[i], deps[i].name)) {
            return __LINE__;
        }
    }
    /* Everything after the table is found at the right offset. */
    if (desc.cap_count != 1 || desc.caps[0].name_len != 9 ||
        memcmp(desc.caps[0].name, "ipc.basic", 9) != 0) {
        return __LINE__;
    }
    if (desc.stack_pages_hint != 3 || desc.heap_pages_hint != 5) {
        return __LINE__;
    }
    if (desc.wasm_size != 4 || memcmp(desc.wasm_bytes, "\0asm", 4) != 0) {
        return __LINE__;
    }
    return 0;
}

static int test_no_records(void) {
    uint32_t len = build(WASMOS_APP_VERSION, 0, 0, 0);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) != 0 || desc.dep_count != 0 || desc.cap_count != 1 ||
        memcmp(desc.wasm_bytes, "\0asm", 4) != 0) {
        return __LINE__;
    }
    return 0;
}

/* A blob cut anywhere inside the table is refused, never read past its end. */
static int test_truncated_table_is_refused(void) {
    const test_dep_t deps[] = {{"fs.vfs", 0, WASMOS_APP_DEP_NEEDS, WASMOS_APP_DEP_KIND_NAME}};
    build(WASMOS_APP_VERSION, 1, deps, 1);
    uint32_t table = (uint32_t)sizeof(test_header_t) + 2;
    wasmos_app_desc_t desc;
    for (uint32_t cut = table; cut < table + 4 + 6; ++cut) {
        if (wasmos_app_parse(g_blob, cut, &desc) == 0) {
            return __LINE__;
        }
    }
    return 0;
}

/* A record whose name_len claims more bytes than follow it is refused. */
static int test_name_past_blob_end_is_refused(void) {
    /* 200 bytes is more than the whole rest of the blob (about 50 bytes), so
     * the claimed name cannot be satisfied from what follows it. */
    const test_dep_t deps[] = {{"vt", 200, WASMOS_APP_DEP_NEEDS, WASMOS_APP_DEP_KIND_NAME}};
    uint32_t len = build(WASMOS_APP_VERSION, 1, deps, 1);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__;
    }
    return 0;
}

static int test_count_above_capacity_is_refused(void) {
    test_dep_t deps[WASMOS_APP_MAX_DEPS + 1];
    for (uint32_t i = 0; i < WASMOS_APP_MAX_DEPS + 1; ++i) {
        deps[i] = (test_dep_t){"x", 0, WASMOS_APP_DEP_WANTS, WASMOS_APP_DEP_KIND_NAME};
    }
    wasmos_app_desc_t desc;
    uint32_t len = build(WASMOS_APP_VERSION, WASMOS_APP_MAX_DEPS, deps, WASMOS_APP_MAX_DEPS);
    if (wasmos_app_parse(g_blob, len, &desc) != 0 || desc.dep_count != WASMOS_APP_MAX_DEPS) {
        return __LINE__;
    }
    len = build(WASMOS_APP_VERSION, WASMOS_APP_MAX_DEPS + 1, deps, WASMOS_APP_MAX_DEPS + 1);
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__;
    }
    return 0;
}

static int test_name_limits_follow_the_kind(void) {
    static const char n35[] = "abcdefghijabcdefghijabcdefghijabcde";
    static const char n36[] = "abcdefghijabcdefghijabcdefghijabcdef";
    static const char c15[] = "abcdefghijabcde";
    static const char c16[] = "abcdefghijabcdef";
    const struct {
        const char* name;
        int ok;
        uint8_t kind;
    } cases[] = {
        {n35, 1, WASMOS_APP_DEP_KIND_NAME},
        {n36, 0, WASMOS_APP_DEP_KIND_NAME},
        {c15, 1, WASMOS_APP_DEP_KIND_CLASS},
        {c16, 0, WASMOS_APP_DEP_KIND_CLASS},
        {"", 0, WASMOS_APP_DEP_KIND_NAME},
    };
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const test_dep_t dep = {cases[i].name, 0, WASMOS_APP_DEP_NEEDS, cases[i].kind};
        uint32_t len = build(WASMOS_APP_VERSION, 1, &dep, 1);
        wasmos_app_desc_t desc;
        int parsed = wasmos_app_parse(g_blob, len, &desc) == 0;
        if (parsed != cases[i].ok) {
            return __LINE__;
        }
    }
    return 0;
}

static int test_unknown_strength_or_kind_is_refused(void) {
    const uint8_t strengths[] = {0u, 4u, 0xFFu};
    const uint8_t kinds[] = {0u, 3u, 0xFFu};
    wasmos_app_desc_t desc;
    for (uint32_t i = 0; i < sizeof(strengths); ++i) {
        const test_dep_t dep = {"vt", 0, strengths[i], WASMOS_APP_DEP_KIND_NAME};
        uint32_t len = build(WASMOS_APP_VERSION, 1, &dep, 1);
        if (wasmos_app_parse(g_blob, len, &desc) == 0) {
            return __LINE__;
        }
    }
    for (uint32_t i = 0; i < sizeof(kinds); ++i) {
        const test_dep_t dep = {"vt", 0, WASMOS_APP_DEP_NEEDS, kinds[i]};
        uint32_t len = build(WASMOS_APP_VERSION, 1, &dep, 1);
        if (wasmos_app_parse(g_blob, len, &desc) == 0) {
            return __LINE__;
        }
    }
    return 0;
}

/* v9 laid required-endpoint records where v10 has dependency records, with the
 * same header size; only the version keeps one from being read as the other. */
static int test_v9_is_refused(void) {
    uint32_t len = build(9u, 0, 0, 0);
    wasmos_app_desc_t desc;
    if (wasmos_app_parse(g_blob, len, &desc) == 0) {
        return __LINE__;
    }
    return 0;
}

/* The boot-module lookup the supervisor's boot chain is spawned through: by
 * the app name the package embeds, skipping modules that are not apps or do
 * not parse, and naming nothing when the table is absent. */
static int test_a_boot_module_is_found_by_its_app_name(void) {
    static uint8_t garbage[16] = "not a package";
    uint32_t len = build(WASMOS_APP_VERSION, 0, 0, 0);
    boot_module_t mods[3];
    boot_info_t info;
    memset(mods, 0, sizeof(mods));
    memset(&info, 0, sizeof(info));
    mods[0].type = 0; /* not an app */
    mods[0].base = (uint64_t)(uintptr_t)g_blob;
    mods[0].size = len;
    mods[1].type = BOOT_MODULE_TYPE_WASMOS_APP;
    mods[1].base = (uint64_t)(uintptr_t)garbage;
    mods[1].size = sizeof(garbage);
    mods[2].type = BOOT_MODULE_TYPE_WASMOS_APP;
    mods[2].base = (uint64_t)(uintptr_t)g_blob;
    mods[2].size = len;
    info.flags = BOOT_INFO_FLAG_MODULES_PRESENT;
    info.modules = mods;
    info.module_count = 3;
    info.module_entry_size = sizeof(boot_module_t);
    if (wasmos_app_boot_module_index(&info, "t", 1) != 2u) {
        return __LINE__;
    }
    if (wasmos_app_boot_module_index(&info, "tt", 2) != 0xFFFFFFFFu ||
        wasmos_app_boot_module_index(&info, "x", 1) != 0xFFFFFFFFu ||
        wasmos_app_boot_module_index(&info, "t", 0) != 0xFFFFFFFFu) {
        return __LINE__;
    }
    info.flags = 0;
    if (wasmos_app_boot_module_index(&info, "t", 1) != 0xFFFFFFFFu) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_records_parse_and_later_sections_follow),
        WASMOS_TEST_CASE(test_no_records),
        WASMOS_TEST_CASE(test_truncated_table_is_refused),
        WASMOS_TEST_CASE(test_name_past_blob_end_is_refused),
        WASMOS_TEST_CASE(test_count_above_capacity_is_refused),
        WASMOS_TEST_CASE(test_name_limits_follow_the_kind),
        WASMOS_TEST_CASE(test_unknown_strength_or_kind_is_refused),
        WASMOS_TEST_CASE(test_v9_is_refused),
        WASMOS_TEST_CASE(test_a_boot_module_is_found_by_its_app_name),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    if (wasmos_test_run_all(cases, groups) != 0) {
        return 1;
    }
    printf("test_wasmos_app_deps: %d groups passed\n", groups);
    return 0;
}
