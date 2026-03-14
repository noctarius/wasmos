#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos_driver_abi.h"

static int32_t g_reply_endpoint = -1;
static int32_t g_spawn_request_id = 1;
static int32_t g_proc_endpoint = -1;
static int32_t g_module_count = 0;
static int32_t g_init_index = -1;
static int32_t g_next_index = 0;
static int32_t g_pending_index = -1;
static int32_t g_tick = 0;
static int32_t g_cli_started = 0;
static int32_t g_cli_wait_ticks = 0;
static int32_t (*volatile g_console_write)(int32_t, int32_t);
static int32_t (*volatile g_debug_mark)(int32_t);

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

static int
should_skip_module(int32_t index)
{
    char name[32];
    name[0] = '\0';
    if (wasmos_boot_module_name(index, (int32_t)(uintptr_t)name, (int32_t)sizeof(name)) < 0) {
        return 0;
    }
    if (str_eq(name, "ata") || str_eq(name, "fs-fat")) {
        return 1;
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
           int32_t module_count,
           int32_t init_index,
           int32_t ignored_arg3)
{
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

    if (proc_endpoint < 0 || module_count <= 0 || init_index < 0) {
        log_line("[sysinit] invalid init args\n");
        stall_forever();
    }

    g_proc_endpoint = proc_endpoint;
    g_module_count = module_count;
    g_init_index = init_index;
    g_next_index = 0;
    g_pending_index = -1;
    g_cli_started = proc_running("cli");
    g_cli_wait_ticks = 0;
    trace_line("[sysinit] start\n");
    trace_mark(0x1103);
    trace_line("[sysinit] boot module list\n");
    for (int32_t i = 0; i < g_module_count; ++i) {
        char name[32];
        name[0] = '\0';
        if (wasmos_boot_module_name(i, (int32_t)(uintptr_t)name, (int32_t)sizeof(name)) >= 0) {
            trace_line("[sysinit] module ");
            trace_line(name);
            trace_line(" idx=");
            {
                char buf[12];
                int n = i;
                int k = 0;
                if (n == 0) {
                    buf[k++] = '0';
                } else {
                    int32_t tmp = n;
                    char rev[12];
                    int r = 0;
                    while (tmp > 0 && r < (int)sizeof(rev)) {
                        rev[r++] = (char)('0' + (tmp % 10));
                        tmp /= 10;
                    }
                    while (r > 0) {
                        buf[k++] = rev[--r];
                    }
                }
                buf[k++] = '\n';
                buf[k] = '\0';
                trace_line(buf);
            }
        } else {
            trace_line("[sysinit] module <name-error>\n");
        }
    }

    trace_line("[sysinit] enter loop\n");
    trace_mark(0x1104);
    for (;;) {
        g_tick++;
        if ((g_tick & 0x3FF) == 0) {
            trace_mark(0x11FF);
        }
        for (volatile int spin = 0; spin < 200000; ++spin) {
        }
        (void)wasmos_sched_yield();
        while (g_next_index < g_module_count &&
               (g_next_index == g_init_index || should_skip_module(g_next_index))) {
            trace_line("[sysinit] skip index ");
            char buf[12];
            int n = g_next_index;
            int i = 0;
            if (n == 0) {
                buf[i++] = '0';
            } else {
                int32_t tmp = n;
                char rev[12];
                int r = 0;
                while (tmp > 0 && r < (int)sizeof(rev)) {
                    rev[r++] = (char)('0' + (tmp % 10));
                    tmp /= 10;
                }
                while (r > 0) {
                    buf[i++] = rev[--r];
                }
            }
            buf[i++] = '\n';
            buf[i] = '\0';
            trace_line(buf);
            g_next_index++;
        }
        if (g_next_index >= g_module_count) {
            if (!g_cli_started) {
                if (!proc_running("fs-fat")) {
                    g_cli_wait_ticks = 0;
                    continue;
                }
                if (g_cli_wait_ticks < 8) {
                    g_cli_wait_ticks++;
                    continue;
                }
                trace_line("[sysinit] spawn cli via fs\n");
                if (spawn_named("cli") != 0) {
                    log_line("[sysinit] cli spawn failed\n");
                    stall_forever();
                }
                g_cli_started = 1;
                continue;
            }
            trace_line("[sysinit] idle wait\n");
            (void)wasmos_ipc_recv(g_reply_endpoint);
            continue;
        }

        char name[32];
        name[0] = '\0';
        int32_t name_rc = wasmos_boot_module_name(g_next_index,
                                                  (int32_t)(uintptr_t)name,
                                                  (int32_t)sizeof(name));
        if (name_rc < 0) {
            log_line("[sysinit] module name read failed\n");
            stall_forever();
        }
        trace_line("[sysinit] spawn ");
        trace_line(name);
        trace_line(" idx=");
        {
            char buf[12];
            int n = g_next_index;
            int i = 0;
            if (n == 0) {
                buf[i++] = '0';
            } else {
                int32_t tmp = n;
                char rev[12];
                int r = 0;
                while (tmp > 0 && r < (int)sizeof(rev)) {
                    rev[r++] = (char)('0' + (tmp % 10));
                    tmp /= 10;
                }
                while (r > 0) {
                    buf[i++] = rev[--r];
                }
            }
            buf[i++] = '\n';
            buf[i] = '\0';
            trace_line(buf);
        }

        if (wasmos_ipc_send(g_proc_endpoint, g_reply_endpoint,
                            PROC_IPC_SPAWN,
                            g_spawn_request_id,
                            g_next_index,
                            0,
                            0,
                            0) != 0) {
            log_line("[sysinit] spawn send failed\n");
            stall_forever();
        }

        g_pending_index = g_next_index;

        int32_t recv_rc = wasmos_ipc_recv(g_reply_endpoint);
        if (recv_rc < 0) {
            log_line("[sysinit] spawn recv failed\n");
            stall_forever();
        }

        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        if (resp_req != g_spawn_request_id) {
            log_line("[sysinit] spawn response mismatch\n");
            stall_forever();
        }
        if (resp_type != PROC_IPC_RESP) {
            log_line("[sysinit] spawn failed type=");
            {
                char buf[12];
                int n = resp_type;
                int i = 0;
                if (n == 0) {
                    buf[i++] = '0';
                } else {
                    int32_t tmp = n;
                    char rev[12];
                    int r = 0;
                    while (tmp > 0 && r < (int)sizeof(rev)) {
                        rev[r++] = (char)('0' + (tmp % 10));
                        tmp /= 10;
                    }
                    while (r > 0) {
                        buf[i++] = rev[--r];
                    }
                }
                buf[i++] = '\n';
                buf[i] = '\0';
                log_line(buf);
            }
            stall_forever();
        }

        g_spawn_request_id++;
        g_next_index = g_pending_index + 1;
        g_pending_index = -1;
    }

    trace_line("[sysinit] exit\n");
    trace_mark(0x11EE);
}
