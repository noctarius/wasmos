/* test_subsystem_registry.c — the subsystem and exec-handler tables
 * (subsystem_registry.h), which map a request tag to the runtime or broker that
 * handles it and a file to the exec handler that claims it.
 *
 * subsystem_registry.c, hashmap.c and kmem.c are compiled in for real, together
 * with the libc string.c that supplies str_copy/str_copy_bytes; the slab
 * allocator underneath kmem is replaced by tests/unit/stubs_slab.c (host heap)
 * and the spinlocks by tests/unit/stubs_spinlock.c. tests/unit/include/klog.h
 * shadows the kernel's, discarding every klog_write the registry makes on a
 * rejection, so a case can assert only the return value and never the reason
 * logged with it. The registry's tables are
 * process-global file statics, so every case brackets itself with
 * wasmos_subsystem_registry_reset() — the cases run in a shuffled order and
 * would otherwise inherit each other's registrations.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all stops
 * at the first failure (test_shuffle.h).
 */
#include "subsystem_registry.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

/* Two distinct non-NULL ops pointers. The registry only null-checks and stores
 * ops, never dereferencing it, so the address of an int serves: what the cases
 * assert is that each entry kept the pointer it was registered with. */
static const int g_ops_a = 1;
static const int g_ops_b = 2;

/* Register the three broker subsystems the exec-handler cases name as owners: an
 * exec handler is refused unless its request tag already belongs to a registered
 * broker. Returns 0, or __LINE__ as a failure marker the caller propagates. */
static int register_test_brokers(void) {
    if (wasmos_subsystem_registry_register_broker("LUA", "NATIVE", "LUA", 101u, 0u, 0u, 0u, 1u) !=
        0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_broker("JAVA", "NATIVE", "JAVA", 102u, 0u, 0u, 0u, 1u) !=
        0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_broker("SCRIPT", "NATIVE", "SCRIPT", 103u, 0u, 0u, 0u,
                                                  1u) != 0) {
        return __LINE__;
    }
    return 0;
}

/* The registry's bucket hash (FNV-1a over at most WASMOS_SUBSYSTEM_TAG_LEN
 * bytes), duplicated here so a case can assert that the two tags it registers
 * still collide. Keep in step with subsystem_tag_hash in
 * src/kernel/subsystem_registry.c. */
static uint32_t test_subsystem_tag_hash(const char* tag) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < WASMOS_SUBSYSTEM_TAG_LEN && tag[i] != '\0'; ++i) {
        hash ^= (uint8_t)tag[i];
        hash *= 16777619u;
    }
    return hash;
}

/* "H67" and "WTAA" hash to the same bucket, so both entries end up on one
 * bucket chain. Lookup must return each tag's own entry rather than whichever
 * one heads the chain. */
static int test_collision_bucket_lookup(void) {
    const char* tag_a = "H67";
    const char* tag_b = "WTAA";
    const wasmos_subsystem_registry_entry_t* entry_a = 0;
    const wasmos_subsystem_registry_entry_t* entry_b = 0;
    const wasmos_subsystem_ops_t* ops_a = (const wasmos_subsystem_ops_t*)&g_ops_a;
    const wasmos_subsystem_ops_t* ops_b = (const wasmos_subsystem_ops_t*)&g_ops_b;

    if (test_subsystem_tag_hash(tag_a) != test_subsystem_tag_hash(tag_b))
        return __LINE__;

    wasmos_subsystem_registry_reset();
    if (wasmos_subsystem_registry_register_builtin(tag_a, "WARP", 1u, 1u, 1u, ops_a) != 0)
        return __LINE__;
    if (wasmos_subsystem_registry_register_builtin(tag_b, "NATIVE", 0u, 0u, 1u, ops_b) != 0)
        return __LINE__;

    entry_a = wasmos_subsystem_registry_find(tag_a);
    entry_b = wasmos_subsystem_registry_find(tag_b);
    if (!entry_a || !entry_b)
        return __LINE__;
    if (strcmp(entry_a->request_tag, tag_a) != 0)
        return __LINE__;
    if (strcmp(entry_b->request_tag, tag_b) != 0)
        return __LINE__;
    if (entry_a->kind != WASMOS_SUBSYSTEM_HANDLER_BUILTIN)
        return __LINE__;
    if (entry_b->kind != WASMOS_SUBSYSTEM_HANDLER_BUILTIN)
        return __LINE__;
    if (strcmp(entry_a->runtime_tag, "WARP") != 0)
        return __LINE__;
    if (strcmp(entry_b->runtime_tag, "NATIVE") != 0)
        return __LINE__;
    if (entry_a->ops != ops_a)
        return __LINE__;
    if (entry_b->ops != ops_b)
        return __LINE__;
    if (wasmos_subsystem_registry_register_builtin(tag_a, "WARP", 1u, 1u, 1u, ops_a) == 0)
        return __LINE__;

    wasmos_subsystem_registry_reset();
    return 0;
}

static int test_broker_registration_lookup(void) {
    const char* tag = "BEAM";
    const char* runtime_tag = "NATIVE";
    const char* broker_name = "BEAM";
    const uint32_t endpoint = 77u;
    const wasmos_subsystem_registry_entry_t* entry = 0;

    wasmos_subsystem_registry_reset();
    if (wasmos_subsystem_registry_register_broker(tag, runtime_tag, broker_name, endpoint, 0u, 0u,
                                                  0u, 1u) != 0) {
        return __LINE__;
    }

    entry = wasmos_subsystem_registry_find(tag);
    if (!entry)
        return __LINE__;
    if (entry->kind != WASMOS_SUBSYSTEM_HANDLER_BROKER)
        return __LINE__;
    if (strcmp(entry->request_tag, tag) != 0)
        return __LINE__;
    if (strcmp(entry->runtime_tag, runtime_tag) != 0)
        return __LINE__;
    if (strcmp(entry->broker_name, broker_name) != 0)
        return __LINE__;
    if (entry->broker_endpoint != endpoint)
        return __LINE__;
    if (entry->uses_wasm_payload != 0u)
        return __LINE__;
    if (entry->needs_runtime_lock != 0u)
        return __LINE__;
    if (entry->gates_ready_for_services != 1u)
        return __LINE__;
    if (entry->ops != 0)
        return __LINE__;
    if (wasmos_subsystem_registry_register_broker(tag, runtime_tag, broker_name, endpoint, 0u, 0u,
                                                  0u, 1u) == 0) {
        return __LINE__;
    }

    wasmos_subsystem_registry_reset();
    return 0;
}

static int test_exec_handler_registration_lookup(void) {
    static const wasmos_exec_match_node_t lua_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_OR,
            .left_index = 1u,
            .right_index = 2u,
        },
        {
            .kind = WASMOS_EXEC_MATCH_EXTENSION,
            .value_len = 4u,
            .value.text = ".lua",
        },
        {
            .kind = WASMOS_EXEC_MATCH_PREFIX,
            .value_len = 2u,
            .value.prefix = {'#', '!'},
        },
    };
    static const wasmos_exec_match_node_t java_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_AND,
            .left_index = 1u,
            .right_index = 2u,
        },
        {
            .kind = WASMOS_EXEC_MATCH_EXTENSION,
            .value_len = 4u,
            .value.text = ".jar",
        },
        {
            .kind = WASMOS_EXEC_MATCH_PREFIX,
            .value_len = 4u,
            .value.prefix = {'P', 'K', 0x03, 0x04},
        },
    };
    static const uint8_t shebang_bytes[] = {'#', '!', '/', 'b', 'i', 'n', '/', 'l', 'u', 'a', '\n'};
    static const uint8_t jar_bytes[] = {'P', 'K', 0x03, 0x04, 0x14, 0x00};
    wasmos_exec_probe_t probe;
    const wasmos_exec_handler_registry_entry_t* handler = 0;

    wasmos_subsystem_registry_reset();
    if (register_test_brokers() != 0)
        return __LINE__;
    if (wasmos_subsystem_registry_register_exec_handler("lua-file", "LUA", 0u, 40u, 2u, lua_nodes,
                                                        3u, 0u) != 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_exec_handler("jar-file", "JAVA", 0u, 50u, 4u, java_nodes,
                                                        3u, 0u) != 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_exec_max_probe_bytes() != 4u)
        return __LINE__;

    memset(&probe, 0, sizeof(probe));
    probe.path = "/boot/apps/demo.lua";
    handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (!handler)
        return __LINE__;
    if (strcmp(handler->handler_name, "lua-file") != 0)
        return __LINE__;
    if (strcmp(handler->request_tag, "LUA") != 0)
        return __LINE__;
    if (handler->broker_endpoint != 101u)
        return __LINE__;

    memset(&probe, 0, sizeof(probe));
    probe.path = "/user/bin/startup";
    probe.initial_bytes = shebang_bytes;
    probe.initial_size = sizeof(shebang_bytes);
    handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (!handler)
        return __LINE__;
    if (strcmp(handler->handler_name, "lua-file") != 0)
        return __LINE__;

    memset(&probe, 0, sizeof(probe));
    probe.path = "/boot/apps/tool.jar";
    probe.initial_bytes = jar_bytes;
    probe.initial_size = sizeof(jar_bytes);
    handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (!handler)
        return __LINE__;
    if (strcmp(handler->handler_name, "jar-file") != 0)
        return __LINE__;
    if (strcmp(handler->request_tag, "JAVA") != 0)
        return __LINE__;

    memset(&probe, 0, sizeof(probe));
    probe.path = "/boot/apps/bad.jar";
    probe.initial_bytes = (const uint8_t*)"NOPE";
    probe.initial_size = 4u;
    handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (handler)
        return __LINE__;

    if (wasmos_subsystem_registry_register_exec_handler("lua-file", "LUA", 0u, 40u, 2u, lua_nodes,
                                                        3u, 0u) == 0) {
        return __LINE__;
    }

    wasmos_subsystem_registry_reset();
    return 0;
}

/* Two handlers at the same priority (10), so the selection is decided by the
 * tie-break rather than by priority or registration order: the
 * lexicographically smaller handler_name wins, which is why a path both match
 * resolves to "aaa-script". The NOT node is what keeps a shebang script whose
 * name ends in .lua away from the generic handler, leaving it unmatched. */
static int test_exec_handler_not_and_priority(void) {
    static const wasmos_exec_match_node_t generic_script_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_AND,
            .left_index = 1u,
            .right_index = 2u,
        },
        {
            .kind = WASMOS_EXEC_MATCH_PREFIX,
            .value_len = 2u,
            .value.prefix = {'#', '!'},
        },
        {
            .kind = WASMOS_EXEC_MATCH_NOT,
            .left_index = 3u,
        },
        {
            .kind = WASMOS_EXEC_MATCH_EXTENSION,
            .value_len = 4u,
            .value.text = ".lua",
        },
    };
    static const wasmos_exec_match_node_t exact_script_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_FILENAME,
            .value_len = 6u,
            .value.text = "script",
        },
    };
    static const uint8_t shebang_bytes[] = {'#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n'};
    wasmos_exec_probe_t probe;
    const wasmos_exec_handler_registry_entry_t* handler = 0;

    wasmos_subsystem_registry_reset();
    if (register_test_brokers() != 0)
        return __LINE__;
    if (wasmos_subsystem_registry_register_exec_handler("generic-script", "SCRIPT", 0u, 10u, 2u,
                                                        generic_script_nodes, 4u, 0u) != 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_exec_handler("aaa-script", "SCRIPT", 0u, 10u, 0u,
                                                        exact_script_nodes, 1u, 0u) != 0) {
        return __LINE__;
    }

    memset(&probe, 0, sizeof(probe));
    probe.path = "/user/bin/script";
    probe.initial_bytes = shebang_bytes;
    probe.initial_size = sizeof(shebang_bytes);
    handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (!handler)
        return __LINE__;
    if (strcmp(handler->handler_name, "aaa-script") != 0)
        return __LINE__;

    memset(&probe, 0, sizeof(probe));
    probe.path = "/user/bin/module.lua";
    probe.initial_bytes = shebang_bytes;
    probe.initial_size = sizeof(shebang_bytes);
    handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (handler)
        return __LINE__;

    memset(&probe, 0, sizeof(probe));
    probe.path = "/user/bin/runme";
    probe.initial_bytes = shebang_bytes;
    probe.initial_size = sizeof(shebang_bytes);
    handler = wasmos_subsystem_registry_find_exec_handler(&probe);
    if (!handler)
        return __LINE__;
    if (strcmp(handler->handler_name, "generic-script") != 0)
        return __LINE__;

    wasmos_subsystem_registry_reset();
    return 0;
}

static int test_exec_handler_validation(void) {
    static const wasmos_exec_match_node_t bad_probe_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_PREFIX,
            .value_len = 4u,
            .value.prefix = {'P', 'K', 0x03, 0x04},
        },
    };
    static const wasmos_exec_match_node_t cycle_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_NOT,
            .left_index = 0u,
        },
    };
    static const wasmos_exec_match_node_t bad_ext_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_EXTENSION,
            .value_len = 3u,
            .value.text = "lua",
        },
    };

    wasmos_subsystem_registry_reset();
    if (register_test_brokers() != 0)
        return __LINE__;
    if (wasmos_subsystem_registry_register_exec_handler("missing-owner", "WARP", 0u, 1u, 4u,
                                                        bad_probe_nodes, 1u, 0u) == 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_exec_handler("short-probe", "LUA", 0u, 1u, 2u,
                                                        bad_probe_nodes, 1u, 0u) == 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_exec_handler("cycle", "LUA", 0u, 1u, 0u, cycle_nodes, 1u,
                                                        0u) == 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_exec_handler("bad-ext", "LUA", 0u, 1u, 0u, bad_ext_nodes,
                                                        1u, 0u) == 0) {
        return __LINE__;
    }

    wasmos_subsystem_registry_reset();
    return 0;
}

/* A broker context that registers a subsystem + an exec handler must have both
 * torn down when that owning context exits (wasmos_subsystem_registry_drop_owner),
 * so a dead broker leaves no stale endpoint or matcher behind. */
static int test_owner_drop(void) {
    static const wasmos_exec_match_node_t ext_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_EXTENSION,
            .value_len = 3u,
            .value.text = ".rc",
        },
    };
    const uint32_t owner = 500u;
    const uint32_t other_owner = 501u;
    wasmos_exec_probe_t probe;

    wasmos_subsystem_registry_reset();
    /* Owner 500 registers a broker subsystem + a handler; owner 501 registers a
     * second broker, which must survive the drop of owner 500. */
    if (wasmos_subsystem_registry_register_broker("SCRIPT", "NATIVE", "SCRIPT", 200u, owner, 0u, 0u,
                                                  1u) != 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_broker("LUA", "NATIVE", "LUA", 201u, other_owner, 0u, 0u,
                                                  1u) != 0) {
        return __LINE__;
    }
    if (wasmos_subsystem_registry_register_exec_handler("rc-file", "SCRIPT", owner, 10u, 4u,
                                                        ext_nodes, 1u, 0u) != 0) {
        return __LINE__;
    }
    if (!wasmos_subsystem_registry_find("SCRIPT"))
        return __LINE__;
    memset(&probe, 0, sizeof(probe));
    probe.path = "/init/apps/hello.rc";
    if (!wasmos_subsystem_registry_find_exec_handler(&probe))
        return __LINE__;
    if (wasmos_subsystem_registry_exec_max_probe_bytes() != 4u)
        return __LINE__;

    wasmos_subsystem_registry_drop_owner(owner);

    /* Owner 500's broker + handler are gone; owner 501's broker survives. */
    if (wasmos_subsystem_registry_find("SCRIPT"))
        return __LINE__;
    if (wasmos_subsystem_registry_find_exec_handler(&probe))
        return __LINE__;
    if (wasmos_subsystem_registry_exec_max_probe_bytes() != 0u)
        return __LINE__;
    if (!wasmos_subsystem_registry_find("LUA"))
        return __LINE__;

    /* After the drop, the freed tag can be registered again. */
    if (wasmos_subsystem_registry_register_broker("SCRIPT", "NATIVE", "SCRIPT", 202u, owner, 0u, 0u,
                                                  1u) != 0) {
        return __LINE__;
    }

    wasmos_subsystem_registry_reset();
    return 0;
}

/* Registration is bounded per owner so one broker context cannot monopolize the
 * tables.  A single owner may register up to WASMOS_SUBSYSTEM_MAX_BROKERS_PER_OWNER
 * brokers and WASMOS_EXEC_HANDLER_MAX_PER_OWNER handlers; the next is rejected. */
static int test_per_owner_caps(void) {
    static const wasmos_exec_match_node_t ext_nodes[] = {
        {
            .kind = WASMOS_EXEC_MATCH_EXTENSION,
            .value_len = 3u,
            .value.text = ".rc",
        },
    };
    const uint32_t owner = 600u;
    char tag[WASMOS_SUBSYSTEM_TAG_LEN + 1];
    char name[WASMOS_EXEC_HANDLER_NAME_LEN + 1];
    uint32_t i;

    wasmos_subsystem_registry_reset();
    for (i = 0; i < WASMOS_SUBSYSTEM_MAX_BROKERS_PER_OWNER; ++i) {
        snprintf(tag, sizeof(tag), "BRK%u", i);
        if (wasmos_subsystem_registry_register_broker(tag, "NATIVE", "BRK", 700u + i, owner, 0u, 0u,
                                                      1u) != 0) {
            return __LINE__;
        }
    }
    /* The next broker for the same owner is rejected by the per-owner cap. */
    if (wasmos_subsystem_registry_register_broker("BRKX", "NATIVE", "BRK", 799u, owner, 0u, 0u,
                                                  1u) == 0) {
        return __LINE__;
    }

    /* Fill the per-owner handler cap against the owner's first broker tag. */
    for (i = 0; i < WASMOS_EXEC_HANDLER_MAX_PER_OWNER; ++i) {
        snprintf(name, sizeof(name), "h%u", i);
        if (wasmos_subsystem_registry_register_exec_handler(name, "BRK0", owner, 10u, 4u, ext_nodes,
                                                            1u, 0u) != 0) {
            return __LINE__;
        }
    }
    if (wasmos_subsystem_registry_register_exec_handler("hX", "BRK0", owner, 10u, 4u, ext_nodes, 1u,
                                                        0u) == 0) {
        return __LINE__;
    }

    wasmos_subsystem_registry_reset();
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_collision_bucket_lookup),
        WASMOS_TEST_CASE(test_broker_registration_lookup),
        WASMOS_TEST_CASE(test_exec_handler_registration_lookup),
        WASMOS_TEST_CASE(test_exec_handler_not_and_priority),
        WASMOS_TEST_CASE(test_exec_handler_validation),
        WASMOS_TEST_CASE(test_owner_drop),
        WASMOS_TEST_CASE(test_per_owner_caps),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_subsystem_registry: ok\n");
    return 0;
}
