#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;

static void
console_write(const char *s)
{
    if (!s) {
        return;
    }
    (void)printf("%s", s);
}

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

static void
unpack_name(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char *out, uint32_t out_len)
{
    uint32_t args[4] = { arg0, arg1, arg2, arg3 };
    uint32_t pos = 0;
    if (!out || out_len == 0) {
        return;
    }
    for (uint32_t i = 0; i < 4 && pos + 1 < out_len; ++i) {
        uint32_t v = args[i];
        for (uint32_t b = 0; b < 4 && pos + 1 < out_len; ++b) {
            char c = (char)(v & 0xFFu);
            if (c == '\0') {
                out[pos] = '\0';
                return;
            }
            out[pos++] = c;
            v >>= 8u;
        }
    }
    out[pos] = '\0';
}

static int
emit_init_listing(void)
{
    for (int32_t i = 0; i < 256; ++i) {
        char name[64];
        int32_t name_len = wasmos_boot_module_name(i, (int32_t)(uintptr_t)name, (int32_t)sizeof(name));
        if (name_len < 0) {
            break;
        }
        if (name_len >= (int32_t)sizeof(name)) {
            name_len = (int32_t)sizeof(name) - 1;
        }
        if (wasmos_sync_user_read((int32_t)(uintptr_t)name, name_len + 1) != 0) {
            continue;
        }
        (void)printf("%s\n", name);
    }
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

    g_fs_endpoint = wasmos_ipc_create_endpoint();
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_fs_endpoint < 0 || g_reply_endpoint < 0) {
        console_write("[fs-init] endpoint create failed\n");
        stall_forever();
    }
    int32_t fsmgr_endpoint = -1;
    for (;;) {
        fsmgr_endpoint = wasmos_svc_lookup(proc_endpoint, g_reply_endpoint, "fs.vfs", 1);
        if (fsmgr_endpoint >= 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (wasmos_ipc_send(fsmgr_endpoint,
                        g_reply_endpoint,
                        FSMGR_IPC_REGISTER_BACKEND_REQ,
                        1,
                        FSMGR_BACKEND_INIT,
                        g_fs_endpoint,
                        0,
                        0) != 0) {
        console_write("[fs-init] register fs-manager send failed\n");
        stall_forever();
    }
    if (wasmos_ipc_recv(g_reply_endpoint) < 0 ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != FSMGR_IPC_REGISTER_BACKEND_RESP) {
        console_write("[fs-init] register fs-manager failed\n");
        stall_forever();
    }
    console_write("[fs-init] register fs-manager ok\n");

    for (;;) {
        if (wasmos_ipc_recv(g_fs_endpoint) < 0) {
            continue;
        }
        int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t req_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t arg1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t arg2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t arg3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        int32_t status = -1;

        if (type == FS_IPC_LIST_ROOT_REQ) {
            status = emit_init_listing();
        } else if (type == FS_IPC_CHDIR_REQ) {
            char name[32];
            unpack_name((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3, name, sizeof(name));
            status = (strcasecmp(name, "/") == 0 ||
                      strcasecmp(name, "..") == 0 ||
                      strcasecmp(name, "init") == 0 ||
                      strcasecmp(name, "/init") == 0) ? 0 : -1;
        } else if (type == FS_IPC_READY_REQ) {
            status = 0;
        }

        (void)wasmos_ipc_send(source,
                              g_fs_endpoint,
                              status == 0 ? FS_IPC_RESP : FS_IPC_ERROR,
                              req_id,
                              status,
                              0,
                              0,
                              0);
    }
}
