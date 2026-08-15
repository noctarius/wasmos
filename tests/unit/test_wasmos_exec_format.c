/* test_wasmos_exec_format.c — what a blob IS (wasmos_exec_format.h): a packed
 * .wap the kernel loads itself, a file some registered broker claims, or
 * nothing. The classification decides which spawn path runs, so a fixture that
 * is accepted as a WAP by accident, or a broker handler that claims a WAP,
 * misroutes a real spawn.
 *
 * wasmos_exec_format.c, subsystem_registry.c, hashmap.c and kmem.c are compiled
 * in for real, with the libc string.c that supplies str_copy/str_copy_bytes; the
 * slab allocator underneath kmem is replaced by tests/unit/stubs_slab.c (host
 * heap), the spinlocks by tests/unit/stubs_spinlock.c, and klog by
 * tests/unit/include/klog.h, which discards it. Classification consults the
 * subsystem registry, so main() registers the fixture brokers and handlers once
 * before the shuffled run and resets the process-global tables afterwards; no
 * case registers or resets on its own.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all stops
 * at the first failure (test_shuffle.h).
 */
#include "subsystem_registry.h"
#include "wasmos_exec_format.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

/* The on-disk .wap header, and a retired layout kept only to prove it is
 * refused.
 *
 * wasmos_exec_format.c accepts a blob only when its version is the current one
 * AND its header_size equals WASMOS_EXEC_APP_HEADER_SIZE, so the current fixture
 * fills that field with sizeof() and must stay byte-identical to the packer's
 * layout; a field added to one and not the other turns every fixture here into a
 * non-WAP blob.
 *
 * test_wap_header_retired_t is the immediately preceding layout, which carried a
 * driver-match table. Nothing in the tree produces it -- every package is
 * repacked from source on each build -- and it exists here so one case can assert
 * that a container from a superseded version classifies as NONE rather than being
 * reinterpreted under the current layout, which would misread every field after
 * the first removed one. */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t mem_hint_count;
    uint32_t compiled_size;
    char subsystem_tag[8];
    uint32_t region_count;
} test_wap_header_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t mem_hint_count;
    uint32_t driver_match_count;
    uint32_t compiled_size;
    char subsystem_tag[8];
    uint32_t region_count;
} test_wap_header_retired_t;

/* Register the two broker subsystems and the two exec handlers the cases match
 * against: LUA claims ".lua OR a #! prefix" and JAVA claims ".jar AND a PK\x03\x04
 * prefix", with probe budgets of 2 and 4 bytes. Returns 0, or -1 on the first
 * registration that is refused. Called once from main(), not per case. */
static int register_test_handlers(void) {
    static const wasmos_exec_match_node_t lua_nodes[] = {
        {.kind = WASMOS_EXEC_MATCH_OR, .left_index = 1u, .right_index = 2u},
        {.kind = WASMOS_EXEC_MATCH_EXTENSION, .value_len = 4u, .value.text = ".lua"},
        {.kind = WASMOS_EXEC_MATCH_PREFIX, .value_len = 2u, .value.prefix = {'#', '!'}},
    };
    static const wasmos_exec_match_node_t java_nodes[] = {
        {.kind = WASMOS_EXEC_MATCH_AND, .left_index = 1u, .right_index = 2u},
        {.kind = WASMOS_EXEC_MATCH_EXTENSION, .value_len = 4u, .value.text = ".jar"},
        {.kind = WASMOS_EXEC_MATCH_PREFIX, .value_len = 4u, .value.prefix = {'P', 'K', 0x03, 0x04}},
    };

    if (wasmos_subsystem_registry_register_broker("LUA", "NATIVE", "LUA", 101u, 0u, 0u, 0u, 1u) !=
        0)
        return -1;
    if (wasmos_subsystem_registry_register_broker("JAVA", "NATIVE", "JAVA", 102u, 0u, 0u, 0u, 1u) !=
        0)
        return -1;
    if (wasmos_subsystem_registry_register_exec_handler(
            "lua-file", "LUA", 0u, 40u, 2u, lua_nodes, 3u, 0u) != 0)
        return -1;
    if (wasmos_subsystem_registry_register_exec_handler(
            "jar-file", "JAVA", 0u, 50u, 4u, java_nodes, 3u, 0u) != 0)
        return -1;
    return 0;
}

/* A container at the current version classifies as WAP even when its path
 * carries an extension a broker handler claims: the in-kernel format wins over a
 * registered broker, otherwise a .wap named demo.jar would be handed to the Java
 * host. A container at a retired version classifies as NONE -- refused outright
 * rather than reinterpreted under the current layout, which would misread every
 * field after the first removed one. */
static int test_classify_real_wap_fixtures(void) {
    static const struct {
        test_wap_header_t header;
        char name[1];
        char entry[1];
        uint8_t payload[1];
    } wap_current = {
        .header =
            {
                .magic = {'W', 'A', 'S', 'M', 'O', 'S', 'A', 'P'},
                .version = WASMOS_EXEC_APP_VERSION,
                .header_size = sizeof(test_wap_header_t),
                .flags = 1u << 2,
                .name_len = 1u,
                .entry_len = 1u,
                .wasm_size = 1u,
                .subsystem_tag = "WASM",
            },
        .name = {'x'},
        .entry = {'m'},
        .payload = {0x00u},
    };
    static const struct {
        test_wap_header_retired_t header;
        char name[1];
        char entry[1];
        uint8_t payload[1];
    } wap_retired = {
        .header =
            {
                .magic = {'W', 'A', 'S', 'M', 'O', 'S', 'A', 'P'},
                .version = 8u,
                .header_size = sizeof(test_wap_header_retired_t),
                .flags = 1u << 2,
                .name_len = 1u,
                .entry_len = 1u,
                .wasm_size = 1u,
                .subsystem_tag = "WASM",
            },
        .name = {'y'},
        .entry = {'n'},
        .payload = {0x00u},
    };
    wasmos_exec_format_match_t match;

    if (sizeof(test_wap_header_t) != WASMOS_EXEC_APP_HEADER_SIZE) {
        return __LINE__;
    }

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify("/boot/apps/demo.jar",
                                    (const uint8_t*)&wap_current,
                                    (uint32_t)sizeof(wap_current),
                                    &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_WAP || match.handler.handler_name[0] != '\0')
        return __LINE__;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify("/boot/apps/legacy.wap",
                                    (const uint8_t*)&wap_retired,
                                    (uint32_t)sizeof(wap_retired),
                                    &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_NONE)
        return __LINE__;

    return 0;
}

static int test_classify_broker_formats(void) {
    static const uint8_t lua_script[] = "#!/system/hosts/lua\nprint('ok')\n";
    static const uint8_t jar_blob[] = {'P', 'K', 0x03, 0x04, 0x14, 0x00, 0x08, 0x00};
    static const uint8_t other_blob[] = "not-a-match";
    wasmos_exec_format_match_t match;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify(
            "/user/bin/demo.lua", lua_script, (uint32_t)sizeof(lua_script) - 1u, &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_BROKER || match.handler.handler_name[0] == '\0')
        return __LINE__;
    if (strcmp(match.handler.handler_name, "lua-file") != 0)
        return __LINE__;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify(
            "/user/bin/tool.jar", jar_blob, (uint32_t)sizeof(jar_blob), &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_BROKER || match.handler.handler_name[0] == '\0')
        return __LINE__;
    if (strcmp(match.handler.handler_name, "jar-file") != 0)
        return __LINE__;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify(
            "/user/bin/other.txt", other_blob, (uint32_t)sizeof(other_blob) - 1u, &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_NONE || match.handler.handler_name[0] != '\0')
        return __LINE__;

    return 0;
}

/* The probe budget is the larger of the registered handlers' max_probe_bytes
 * (2 and 4 here) and the 12-byte WAP header prefix -- magic[8] + version +
 * header_size -- which is the minimum a caller must read for the WAP check to
 * be able to answer at all. */
static int test_probe_budget(void) {
    if (wasmos_exec_format_probe_bytes_needed() != 12u)
        return __LINE__;
    return 0;
}

static int test_validate_broker_plan(void) {
    uint8_t plan_blob[256];
    uint32_t off = sizeof(wasmos_broker_spawn_plan_response_t);
    uint32_t host_path_offset = 0u;
    uint32_t host_args_offset = 0u;
    wasmos_exec_handler_registry_entry_t handler_copy;
    const wasmos_exec_handler_registry_entry_t* handler = 0;
    wasmos_broker_spawn_plan_response_t* plan = 0;
    wasmos_exec_broker_plan_t parsed;

    memset(plan_blob, 0, sizeof(plan_blob));
    if (wasmos_subsystem_registry_find_exec_handler(
            &(wasmos_exec_probe_t){
                .path = "/user/bin/tool.jar",
                .initial_bytes = (const uint8_t*)"PK\x03\x04more",
                .initial_size = 8u,
            },
            &handler_copy) != 0)
        return __LINE__;
    handler = &handler_copy;

    host_path_offset = off;
    memcpy(plan_blob + off,
           "/boot/system/brokers/java-host.wap",
           sizeof("/boot/system/brokers/java-host.wap"));
    off += (uint32_t)sizeof("/boot/system/brokers/java-host.wap");
    host_args_offset = off;
    memcpy(plan_blob + off, "--guest=/user/bin/tool.jar", sizeof("--guest=/user/bin/tool.jar"));
    off += (uint32_t)sizeof("--guest=/user/bin/tool.jar");

    plan = (wasmos_broker_spawn_plan_response_t*)plan_blob;
    plan->version = WASMOS_BROKER_SPAWN_PLAN_VERSION;
    plan->plan_kind = WASMOS_BROKER_PLAN_KIND_WAP_PATH;
    plan->host_path_offset = host_path_offset;
    plan->host_path_len = (uint32_t)sizeof("/boot/system/brokers/java-host.wap") - 1u;
    plan->host_args_offset = host_args_offset;
    plan->host_args_len = (uint32_t)sizeof("--guest=/user/bin/tool.jar") - 1u;
    memcpy(plan->request_tag, handler->request_tag, sizeof(plan->request_tag));
    memcpy(plan->runtime_tag, handler->runtime_tag, sizeof(plan->runtime_tag));

    if (wasmos_exec_broker_plan_validate(plan_blob, off, handler, &parsed) != 0) {
        return __LINE__;
    }
    if (!parsed.host_path || strcmp(parsed.host_path, "/boot/system/brokers/java-host.wap") != 0)
        return __LINE__;
    if (!parsed.host_args || strcmp(parsed.host_args, "--guest=/user/bin/tool.jar") != 0)
        return __LINE__;

    plan->host_path_len = 3u;
    if (wasmos_exec_broker_plan_validate(plan_blob, off, handler, &parsed) == 0)
        return __LINE__;
    plan->host_path_len = (uint32_t)sizeof("/boot/system/brokers/java-host.wap") - 1u;

    memcpy(plan->runtime_tag, "WASM3", sizeof("WASM3"));
    if (wasmos_exec_broker_plan_validate(plan_blob, off, handler, &parsed) == 0)
        return __LINE__;

    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    wasmos_subsystem_registry_reset();
    if (register_test_handlers() != 0) {
        return __LINE__;
    }

    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_classify_real_wap_fixtures),
        WASMOS_TEST_CASE(test_classify_broker_formats),
        WASMOS_TEST_CASE(test_probe_budget),
        WASMOS_TEST_CASE(test_validate_broker_plan),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        wasmos_subsystem_registry_reset();
        return 1;
    }
    wasmos_subsystem_registry_reset();
    printf("test_wasmos_exec_format: ok\n");
    return 0;
}
