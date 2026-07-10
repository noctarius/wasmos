#include "subsystem_registry.h"
#include "wasmos_exec_format.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
    uint32_t entry_arg_binding_count;
    uint32_t mem_hint_count;
    uint8_t driver_match_class;
    uint8_t driver_match_subclass;
    uint8_t driver_match_prog_if;
    uint8_t driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t driver_match_count;
    uint32_t compiled_size;
    char subsystem_tag[8];
} test_wap_header_v5_t;

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
    uint32_t reserved;
} test_wap_header_v1_t;

static int
register_test_handlers(void)
{
    static const wasmos_exec_match_node_t lua_nodes[] = {
        { .kind = WASMOS_EXEC_MATCH_OR, .left_index = 1u, .right_index = 2u },
        { .kind = WASMOS_EXEC_MATCH_EXTENSION, .value_len = 4u, .value.text = ".lua" },
        { .kind = WASMOS_EXEC_MATCH_PREFIX, .value_len = 2u, .value.prefix = { '#', '!' } },
    };
    static const wasmos_exec_match_node_t java_nodes[] = {
        { .kind = WASMOS_EXEC_MATCH_AND, .left_index = 1u, .right_index = 2u },
        { .kind = WASMOS_EXEC_MATCH_EXTENSION, .value_len = 4u, .value.text = ".jar" },
        { .kind = WASMOS_EXEC_MATCH_PREFIX, .value_len = 4u, .value.prefix = { 'P', 'K', 0x03, 0x04 } },
    };

    if (wasmos_subsystem_registry_register_broker("LUA", "NATIVE", "LUA", 101u, 0u, 0u, 0u, 1u) != 0) return -1;
    if (wasmos_subsystem_registry_register_broker("JAVA", "NATIVE", "JAVA", 102u, 0u, 0u, 0u, 1u) != 0) return -1;
    if (wasmos_subsystem_registry_register_exec_handler("lua-file", "LUA", 0u, 40u, 2u, lua_nodes, 3u, 0u) != 0) return -1;
    if (wasmos_subsystem_registry_register_exec_handler("jar-file", "JAVA", 0u, 50u, 4u, java_nodes, 3u, 0u) != 0) return -1;
    return 0;
}

static int
test_classify_real_wap_fixtures(void)
{
    static const struct {
        test_wap_header_v5_t header;
        char name[1];
        char entry[1];
        uint8_t payload[1];
    } wap_v5 = {
        .header = {
            .magic = { 'W', 'A', 'S', 'M', 'O', 'S', 'A', 'P' },
            .version = 5u,
            .header_size = sizeof(test_wap_header_v5_t),
            .flags = 1u << 2,
            .name_len = 1u,
            .entry_len = 1u,
            .wasm_size = 1u,
            .subsystem_tag = "WASM",
        },
        .name = { 'x' },
        .entry = { 'm' },
        .payload = { 0x00u },
    };
    static const struct {
        test_wap_header_v1_t header;
        char name[1];
        char entry[1];
        uint8_t payload[1];
    } wap_v1 = {
        .header = {
            .magic = { 'W', 'A', 'S', 'M', 'O', 'S', 'A', 'P' },
            .version = 1u,
            .header_size = sizeof(test_wap_header_v1_t),
            .flags = 1u << 2,
            .name_len = 1u,
            .entry_len = 1u,
            .wasm_size = 1u,
        },
        .name = { 'y' },
        .entry = { 'n' },
        .payload = { 0x00u },
    };
    wasmos_exec_format_match_t match;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify("/boot/apps/demo.jar",
                                    (const uint8_t *)&wap_v5,
                                    (uint32_t)sizeof(wap_v5),
                                    &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_WAP || match.handler != 0) return __LINE__;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify("/boot/apps/legacy.lua",
                                    (const uint8_t *)&wap_v1,
                                    (uint32_t)sizeof(wap_v1),
                                    &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_WAP || match.handler != 0) return __LINE__;

    return 0;
}

static int
test_classify_broker_formats(void)
{
    static const uint8_t lua_script[] = "#!/system/hosts/lua\nprint('ok')\n";
    static const uint8_t jar_blob[] = { 'P', 'K', 0x03, 0x04, 0x14, 0x00, 0x08, 0x00 };
    static const uint8_t other_blob[] = "not-a-match";
    wasmos_exec_format_match_t match;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify("/user/bin/demo.lua",
                                    lua_script,
                                    (uint32_t)sizeof(lua_script) - 1u,
                                    &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_BROKER || !match.handler) return __LINE__;
    if (strcmp(match.handler->handler_name, "lua-file") != 0) return __LINE__;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify("/user/bin/tool.jar",
                                    jar_blob,
                                    (uint32_t)sizeof(jar_blob),
                                    &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_BROKER || !match.handler) return __LINE__;
    if (strcmp(match.handler->handler_name, "jar-file") != 0) return __LINE__;

    memset(&match, 0, sizeof(match));
    if (wasmos_exec_format_classify("/user/bin/other.txt",
                                    other_blob,
                                    (uint32_t)sizeof(other_blob) - 1u,
                                    &match) != 0) {
        return __LINE__;
    }
    if (match.kind != WASMOS_EXEC_FORMAT_NONE || match.handler != 0) return __LINE__;

    return 0;
}

static int
test_probe_budget(void)
{
    if (wasmos_exec_format_probe_bytes_needed() != 12u) return __LINE__;
    return 0;
}

static int
test_validate_broker_plan(void)
{
    uint8_t plan_blob[256];
    uint32_t off = sizeof(wasmos_broker_spawn_plan_response_t);
    uint32_t host_path_offset = 0u;
    uint32_t host_args_offset = 0u;
    const wasmos_exec_handler_registry_entry_t *handler = 0;
    wasmos_broker_spawn_plan_response_t *plan = 0;
    wasmos_exec_broker_plan_t parsed;

    memset(plan_blob, 0, sizeof(plan_blob));
    handler = wasmos_subsystem_registry_find_exec_handler(&(wasmos_exec_probe_t){
        .path = "/user/bin/tool.jar",
        .initial_bytes = (const uint8_t *)"PK\x03\x04more",
        .initial_size = 8u,
    });
    if (!handler) return __LINE__;

    host_path_offset = off;
    memcpy(plan_blob + off, "/boot/system/brokers/java-host.wap", sizeof("/boot/system/brokers/java-host.wap"));
    off += (uint32_t)sizeof("/boot/system/brokers/java-host.wap");
    host_args_offset = off;
    memcpy(plan_blob + off, "--guest=/user/bin/tool.jar", sizeof("--guest=/user/bin/tool.jar"));
    off += (uint32_t)sizeof("--guest=/user/bin/tool.jar");

    plan = (wasmos_broker_spawn_plan_response_t *)plan_blob;
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
    if (!parsed.host_path || strcmp(parsed.host_path, "/boot/system/brokers/java-host.wap") != 0) return __LINE__;
    if (!parsed.host_args || strcmp(parsed.host_args, "--guest=/user/bin/tool.jar") != 0) return __LINE__;

    plan->host_path_len = 3u;
    if (wasmos_exec_broker_plan_validate(plan_blob, off, handler, &parsed) == 0) return __LINE__;
    plan->host_path_len = (uint32_t)sizeof("/boot/system/brokers/java-host.wap") - 1u;

    memcpy(plan->runtime_tag, "WASM3", sizeof("WASM3"));
    if (wasmos_exec_broker_plan_validate(plan_blob, off, handler, &parsed) == 0) return __LINE__;

    return 0;
}

int
main(void)
{
    int rc = 0;

    wasmos_subsystem_registry_reset();
    if (register_test_handlers() != 0) return __LINE__;

    rc = test_classify_real_wap_fixtures();
    if (rc != 0) return rc;
    rc = test_classify_broker_formats();
    if (rc != 0) return rc;
    rc = test_probe_budget();
    if (rc != 0) return rc;
    rc = test_validate_broker_plan();
    if (rc != 0) return rc;

    wasmos_subsystem_registry_reset();
    printf("test_wasmos_exec_format: ok\n");
    return 0;
}
