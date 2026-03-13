#include <stdint.h>
#include "wasmos_driver_abi.h"

#if defined(__wasm__)
#define WASMOS_WASM_IMPORT(module_name, symbol_name) \
    __attribute__((import_module(module_name), import_name(symbol_name)))
#define WASMOS_WASM_EXPORT __attribute__((visibility("default")))
#else
#define WASMOS_WASM_IMPORT(module_name, import_name)
#define WASMOS_WASM_EXPORT
#endif

extern int32_t wasmos_ipc_create_endpoint(void)
    WASMOS_WASM_IMPORT("wasmos", "ipc_create_endpoint");
extern int32_t wasmos_ipc_send(int32_t destination_endpoint,
                               int32_t source_endpoint,
                               int32_t type,
                               int32_t request_id,
                               int32_t arg0,
                               int32_t arg1,
                               int32_t arg2,
                               int32_t arg3)
    WASMOS_WASM_IMPORT("wasmos", "ipc_send");
extern int32_t wasmos_ipc_recv(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_recv");
extern int32_t wasmos_ipc_last_field(int32_t field)
    WASMOS_WASM_IMPORT("wasmos", "ipc_last_field");
extern int32_t wasmos_console_write(int32_t ptr, int32_t len)
    WASMOS_WASM_IMPORT("wasmos", "console_write");
extern int32_t wasmos_debug_mark(int32_t tag)
    WASMOS_WASM_IMPORT("wasmos", "debug_mark");
extern int32_t wasmos_boot_module_name(int32_t index, int32_t buf, int32_t buf_len)
    WASMOS_WASM_IMPORT("wasmos", "boot_module_name");
extern int32_t wasmos_proc_count(void)
    WASMOS_WASM_IMPORT("wasmos", "proc_count");
extern int32_t wasmos_proc_info(int32_t index, int32_t buf, int32_t buf_len)
    WASMOS_WASM_IMPORT("wasmos", "proc_info");

static int32_t g_reply_endpoint = -1;
static int32_t g_spawn_request_id = 1;
static int32_t g_proc_endpoint = -1;
static int32_t g_module_count = 0;
static int32_t g_init_index = -1;
static int32_t g_next_index = 0;
static int32_t g_pending_index = -1;
static int32_t g_tick = 0;
static int32_t (*volatile g_console_write)(int32_t, int32_t);
static int32_t (*volatile g_debug_mark)(int32_t);

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
    int i = 0;
    if (!a || !b) {
        return 0;
    }
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void
log_line(const char *s)
{
    if (!s) {
        return;
    }
    int len = 0;
    while (s[len]) {
        len++;
    }
    if (len > 0) {
        int32_t rc = g_console_write((int32_t)(uintptr_t)s, len);
        if (rc < 0) {
            char ch = '!';
            (void)g_console_write((int32_t)(uintptr_t)&ch, 1);
        }
    }
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
    log_line("[sysinit] proc snapshot\n");
    for (int32_t i = 0; i < count; ++i) {
        char buf[32];
        buf[0] = '\0';
        int32_t pid = wasmos_proc_info(i, (int32_t)(uintptr_t)buf, (int32_t)sizeof(buf));
        if (pid <= 0) {
            continue;
        }
        log_line("[sysinit] proc ");
        log_line(buf);
        log_line("\n");
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

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t module_count,
           int32_t init_index,
           int32_t ignored_arg3)
{
    (void)ignored_arg3;

    g_console_write = wasmos_console_write;
    g_debug_mark = wasmos_debug_mark;
    (void)g_debug_mark(0x1001);
    {
        char ch = 'S';
        g_console_write((int32_t)(uintptr_t)&ch, 1);
    }

    g_reply_endpoint = wasmos_ipc_create_endpoint();
    (void)g_debug_mark(0x1002);
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
    log_line("[sysinit] start\n");
    log_line("[sysinit] boot module list\n");
    for (int32_t i = 0; i < g_module_count; ++i) {
        char name[32];
        name[0] = '\0';
        if (wasmos_boot_module_name(i, (int32_t)(uintptr_t)name, (int32_t)sizeof(name)) >= 0) {
            log_line("[sysinit] module ");
            log_line(name);
            log_line(" idx=");
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
                log_line(buf);
            }
        } else {
            log_line("[sysinit] module <name-error>\n");
        }
    }

    for (;;) {
        g_tick++;
        for (volatile int spin = 0; spin < 200000; ++spin) {
        }
        while (g_next_index < g_module_count &&
               (g_next_index == g_init_index || should_skip_module(g_next_index))) {
            log_line("[sysinit] skip index ");
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
            log_line(buf);
            g_next_index++;
        }
        if (g_next_index >= g_module_count) {
            log_line("[sysinit] idle wait\n");
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
        if (str_eq(name, "cli") && !proc_running("fs-fat")) {
            log_line("[sysinit] defer cli until fs-fat\n");
            continue;
        }

        log_line("[sysinit] spawn ");
        log_line(name);
        log_line(" idx=");
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
            log_line(buf);
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
}
