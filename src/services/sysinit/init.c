#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"
#include "sysinit_types.h"

/*
 * sysinit has been deliberately narrowed. It no longer owns early bootstrap or
 * boot-module iteration; that responsibility moved into the kernel init task.
 * sysinit now behaves like a late-start launcher for user-facing processes once
 * the filesystem path is already available.
 */

static sysinit_state_t g_state = {
    .reply_endpoint = -1,
    .spawn_request_id = 1,
    .proc_endpoint = -1,
    .target_index = 0,
    .target_count = 0,
    .targets = { 0 },
};
static int32_t (*volatile g_console_write)(int32_t, int32_t);
static int32_t (*volatile g_debug_mark)(int32_t);
static uint8_t g_boot_config[512];
/* TODO: Lift the small fixed limits once sysinit needs larger startup sets or
 * richer boot policy than the current manifest-driven name list. */

#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

static uint32_t
load_u32_le(const uint8_t *src)
{
    if (!src) {
        return 0;
    }
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static int
str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static int
target_name_len(const uint8_t *table, uint32_t string_table_size, uint32_t string_offset, uint32_t *out_len)
{
    if (!table || string_offset >= string_table_size) {
        return -1;
    }

    for (uint32_t j = string_offset; j < string_table_size; ++j) {
        if (table[j] == '\0') {
            if (out_len) {
                *out_len = j - string_offset;
            }
            return 0;
        }
    }
    return -1;
}

static int
load_boot_targets(void)
{
    int32_t size = wasmos_boot_config_size();
    uint32_t version;
    uint32_t boot_count;
    uint32_t sysinit_count;
    uint32_t string_table_size;
    uint32_t total_count;
    uint32_t offsets_base;
    uint32_t strings_base;

    if (size < 24 || size > (int32_t)sizeof(g_boot_config)) {
        return -1;
    }
    if (wasmos_boot_config_copy((int32_t)(uintptr_t)g_boot_config, size, 0) != 0) {
        return -1;
    }
    if (wasmos_sync_user_read((int32_t)(uintptr_t)g_boot_config, size) != 0) {
        return -1;
    }
    if (memcmp(g_boot_config, BOOT_CONFIG_MAGIC, 8) != 0) {
        return -1;
    }

    version = load_u32_le(g_boot_config + 8);
    boot_count = load_u32_le(g_boot_config + 12);
    sysinit_count = load_u32_le(g_boot_config + 16);
    string_table_size = load_u32_le(g_boot_config + 20);
    if (version != BOOT_CONFIG_VERSION ||
        sysinit_count == 0 || sysinit_count > SYSINIT_MAX_TARGETS) {
        return -1;
    }
    if (boot_count > 0xFFFFFFFFu - sysinit_count) {
        return -1;
    }
    total_count = boot_count + sysinit_count;
    if (total_count > (((uint32_t)size) - 24u) / 4u) {
        return -1;
    }

    offsets_base = 24u + boot_count * 4u;
    strings_base = 24u + total_count * 4u;
    if (strings_base > (uint32_t)size || string_table_size > (uint32_t)size ||
        strings_base + string_table_size > (uint32_t)size) {
        return -1;
    }

    g_state.target_count = 0;
    for (uint32_t i = 0; i < sysinit_count; ++i) {
        uint32_t offset_pos = offsets_base + i * 4u;
        uint32_t string_offset;
        uint32_t name_len = 0;
        const char *name;

        if (offset_pos + 4u > (uint32_t)size) {
            return -1;
        }
        string_offset = load_u32_le(g_boot_config + offset_pos);
        if (target_name_len(g_boot_config + strings_base, string_table_size, string_offset, &name_len) != 0) {
            return -1;
        }
        if (name_len == 0 || name_len > SYSINIT_MAX_TARGET_NAME_LEN) {
            return -1;
        }

        name = (const char *)(g_boot_config + strings_base + string_offset);
        for (int32_t j = 0; j < g_state.target_count; ++j) {
            if (str_eq(g_state.targets[j], name)) {
                return -1;
            }
        }

        g_state.targets[g_state.target_count] = name;
        g_state.target_count++;
    }

    return 0;
}

static void
log_line(const char *s)
{
    if (!s) {
        return;
    }
    int len = (int)strlen(s);
    if (len > 0) {
        int32_t rc = g_console_write((int32_t)(uintptr_t)s, len);
        if (rc < 0) {
            char ch = '!';
            (void)g_console_write((int32_t)(uintptr_t)&ch, 1);
        }
    }
}

static void
trace_line(const char *s)
{
#if WASMOS_TRACE
    log_line(s);
#else
    (void)s;
#endif
}

static void
trace_mark(int32_t tag)
{
#if WASMOS_TRACE
    (void)g_debug_mark(tag);
#else
    (void)tag;
#endif
}

static int
proc_running(const char *name)
{
    int32_t count = wasmos_proc_count();
    if (count <= 0) {
        return 0;
    }
    if (count > SYSINIT_MAX_PROC_SNAPSHOT) {
        count = SYSINIT_MAX_PROC_SNAPSHOT;
    }
    trace_line("[sysinit] proc snapshot\n");
    for (int32_t i = 0; i < count; ++i) {
        char buf[32];
        buf[0] = '\0';
        int32_t pid = wasmos_proc_info(i, (int32_t)(uintptr_t)buf, (int32_t)sizeof(buf));
        if (pid <= 0) {
            continue;
        }
        trace_line("[sysinit] proc ");
        trace_line(buf);
        trace_line("\n");
        if (str_eq(buf, name)) {
            return 1;
        }
    }
    return 0;
}

static int
proc_count_named(const char *name)
{
    int32_t count = wasmos_proc_count();
    int matches = 0;
    if (count <= 0 || !name || !name[0]) {
        return 0;
    }
    if (count > SYSINIT_MAX_PROC_SNAPSHOT) {
        count = SYSINIT_MAX_PROC_SNAPSHOT;
    }
    for (int32_t i = 0; i < count; ++i) {
        char buf[32];
        buf[0] = '\0';
        if (wasmos_proc_info(i, (int32_t)(uintptr_t)buf, (int32_t)sizeof(buf)) <= 0) {
            continue;
        }
        if (str_eq(buf, name)) {
            matches++;
        }
    }
    return matches;
}

static int
target_spawn_path(const char *name, char *out, uint32_t out_len)
{
    const char *src = 0;
    uint32_t i = 0;
    if (!name || !out || out_len == 0) {
        return -1;
    }
    if (str_eq(name, "chardev-client")) {
        src = "/boot/apps/chardevc.wap";
    } else if (str_eq(name, "font-service")) {
        src = "/boot/system/services/fontsvc.wap";
    } else if (str_eq(name, "vt")) {
        src = "/boot/system/services/vt.wap";
    } else if (str_eq(name, "cli")) {
        src = "/boot/system/services/cli.wap";
    } else if (str_eq(name, "gfx-compositor")) {
        src = "/boot/system/services/gfxcomp.wap";
    } else {
        return -1;
    }

    while (src[i] != '\0') {
        if (i + 1u >= out_len) {
            return -1;
        }
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

static int
spawn_path(const char *path)
{
    uint32_t path_len = 0;
    if (!path || path[0] == '\0') {
        return -1;
    }
    while (path[path_len]) {
        path_len++;
    }
    if (path_len == 0 || path_len > 240u) {
        return -1;
    }
    if (wasmos_fs_buffer_write((int32_t)(uintptr_t)path, (int32_t)path_len, 0) != 0) {
        return -1;
    }
    for (uint32_t attempt = 0; attempt < SYSINIT_MAX_SPAWN_ATTEMPTS; ++attempt) {
        if (wasmos_ipc_send(g_state.proc_endpoint,
                            g_state.reply_endpoint,
                            PROC_IPC_SPAWN_PATH,
                            g_state.spawn_request_id,
                            0,
                            (int32_t)path_len,
                            0,
                            0) != 0) {
            return -1;
        }

        int32_t recv_rc = wasmos_ipc_recv(g_state.reply_endpoint);
        if (recv_rc < 0) {
            return -1;
        }

        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != g_state.spawn_request_id) {
            return -1;
        }
        if (resp_type == PROC_IPC_RESP) {
            g_state.spawn_request_id++;
            return 0;
        }
        if (resp_type == PROC_IPC_ERROR &&
            wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1) == -2) {
            wasmos_sched_yield();
            continue;
        }
        return -1;
    }
    return -1;
}

static void
fatal_stall(const char *msg)
{
    log_line(msg);
    wasmos_sys_ipc_recv_loop();
}

static void
spawn_font_dependency_or_stall(void)
{
    char dep_path[96];
    trace_line("[sysinit] spawn font-service (dep)\n");
    if (target_spawn_path("font-service", dep_path, sizeof(dep_path)) != 0 ||
        spawn_path(dep_path) != 0) {
        fatal_stall("[sysinit] spawn failed: font-service\n");
    }
    /* Let service registration settle before compositor starts. */
    for (uint32_t wait = 0; wait < SYSINIT_DEP_SETTLE_YIELDS; ++wait) {
        wasmos_sched_yield();
    }
}

static int
should_skip_target(const char *name)
{
    if (str_eq(name, "cli")) {
        int cli_count = proc_count_named("cli");
        /* tty0 stays system console; keep one CLI per VT tty1..tty3. */
        return cli_count >= 3;
    }
    return proc_running(name);
}

static void
spawn_target_or_handle_optional(const char *name)
{
    char path[96];
    if (target_spawn_path(name, path, sizeof(path)) == 0 &&
        spawn_path(path) == 0) {
        return;
    }
    if (str_eq(name, "chardev-client")) {
        log_line("[sysinit] optional spawn failed: chardev-client\n");
        return;
    }
    log_line("[sysinit] spawn failed: ");
    log_line(name);
    log_line("\n");
    wasmos_sys_ipc_recv_loop();
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    g_console_write = wasmos_console_write;
    g_debug_mark = wasmos_debug_mark;
    trace_mark(0x1101);

    g_state.reply_endpoint = wasmos_ipc_create_endpoint();
    trace_mark(0x1102);
    if (g_state.reply_endpoint < 0) {
        fatal_stall("[sysinit] failed to create reply endpoint\n");
    }

    if (proc_endpoint < 0) {
        fatal_stall("[sysinit] invalid init args\n");
    }

    g_state.proc_endpoint = proc_endpoint;
    g_state.target_index = 0;
    if (load_boot_targets() != 0) {
        fatal_stall("[sysinit] invalid boot config\n");
    }
    trace_line("[sysinit] start\n");
    trace_mark(0x1103);
    trace_line("[sysinit] enter loop\n");
    trace_mark(0x1104);
    /* The loop is intentionally linear and small: ensure the configured
     * late-start targets are running, then idle forever waiting for any future
     * extension point. */
    for (;;) {
        if (g_state.target_index >= g_state.target_count) {
            trace_line("[sysinit] idle wait\n");
            (void)wasmos_ipc_recv(g_state.reply_endpoint);
            continue;
        }

        const char *name = g_state.targets[g_state.target_index];
        if (str_eq(name, "gfx-compositor") && !proc_running("font-service")) {
            spawn_font_dependency_or_stall();
        }
        if (should_skip_target(name)) {
            g_state.target_index++;
            continue;
        }
        trace_line("[sysinit] spawn ");
        trace_line(name);
        trace_line("\n");
        spawn_target_or_handle_optional(name);
        g_state.target_index++;
    }

    trace_line("[sysinit] exit\n");
    trace_mark(0x11EE);
}
