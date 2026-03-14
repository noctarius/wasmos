#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

/*
 * sysinit has been deliberately narrowed. It no longer owns early bootstrap or
 * boot-module iteration; that responsibility moved into the kernel init task.
 * sysinit now behaves like a late-start launcher for user-facing processes once
 * the filesystem path is already available.
 */

static int32_t g_reply_endpoint = -1;
static int32_t g_spawn_request_id = 1;
static int32_t g_proc_endpoint = -1;
static int32_t g_target_index = 0;
static int32_t (*volatile g_console_write)(int32_t, int32_t);
static int32_t (*volatile g_debug_mark)(int32_t);

static const char *g_targets[] = {
    "chardev-client",
    "cli"
};

#ifndef WASMOS_TRACE
#define WASMOS_TRACE 0
#endif

static void
stall_forever(void)
{
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_recv(endpoint);
        }
    }
}

static int
str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
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
    if (count > 64) {
        count = 64;
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

static void
pack_name_args(const char *name, uint32_t out[4])
{
    if (!out) {
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = 0;
    }
    if (!name) {
        return;
    }
    uint32_t idx = 0;
    for (uint32_t i = 0; name[i] && idx < 16; ++i, ++idx) {
        uint32_t slot = idx / 4;
        uint32_t shift = (idx % 4) * 8;
        out[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
    }
}

static int
spawn_named(const char *name)
{
    uint32_t packed[4];

    if (!name || name[0] == '\0') {
        return -1;
    }

    /* PM's by-name spawn path currently accepts up to sixteen bytes packed into
     * the four IPC argument slots. */
    pack_name_args(name, packed);
    if (wasmos_ipc_send(g_proc_endpoint, g_reply_endpoint,
                        PROC_IPC_SPAWN_NAME,
                        g_spawn_request_id,
                        (int32_t)packed[0],
                        (int32_t)packed[1],
                        (int32_t)packed[2],
                        (int32_t)packed[3]) != 0) {
        return -1;
    }

    int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
    if (recv_rc < 0) {
        return -1;
    }

    int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
    int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
    if (resp_req != g_spawn_request_id || resp_type != PROC_IPC_RESP) {
        return -1;
    }

    g_spawn_request_id++;
    return 0;
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
    {
        char ch = 'S';
        g_console_write((int32_t)(uintptr_t)&ch, 1);
    }

    g_reply_endpoint = wasmos_ipc_create_endpoint();
    trace_mark(0x1102);
    if (g_reply_endpoint < 0) {
        log_line("[sysinit] failed to create reply endpoint\n");
        stall_forever();
    }

    if (proc_endpoint < 0) {
        log_line("[sysinit] invalid init args\n");
        stall_forever();
    }

    g_proc_endpoint = proc_endpoint;
    g_target_index = 0;
    trace_line("[sysinit] start\n");
    trace_mark(0x1103);
    trace_line("[sysinit] enter loop\n");
    trace_mark(0x1104);
    /* The loop is intentionally linear and small: ensure the known targets are
     * running, then idle forever waiting for any future extension point. */
    for (;;) {
        if (g_target_index >= (int32_t)(sizeof(g_targets) / sizeof(g_targets[0]))) {
            trace_line("[sysinit] idle wait\n");
            (void)wasmos_ipc_recv(g_reply_endpoint);
            continue;
        }

        const char *name = g_targets[g_target_index];
        if (proc_running(name)) {
            g_target_index++;
            continue;
        }
        trace_line("[sysinit] spawn ");
        trace_line(name);
        trace_line("\n");
        if (spawn_named(name) != 0) {
            log_line("[sysinit] spawn failed\n");
            stall_forever();
        }
        g_target_index++;
    }

    trace_line("[sysinit] exit\n");
    trace_mark(0x11EE);
}
