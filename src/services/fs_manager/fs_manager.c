#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

#define FS_CLIENT_CAP 16

typedef enum {
    FS_MOUNT_ROOT = 0,
    FS_MOUNT_BOOT,
    FS_MOUNT_INIT
} fs_mount_t;

typedef struct {
    uint8_t in_use;
    int32_t source;
    fs_mount_t mount;
} fs_client_state_t;

static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static int32_t g_backend_boot = -1;
static int32_t g_backend_init = -1;
static fs_client_state_t g_clients[FS_CLIENT_CAP];

static void stall_forever(void) {
    int32_t endpoint = wasmos_ipc_create_endpoint();
    for (;;) {
        if (endpoint >= 0) {
            (void)wasmos_ipc_recv(endpoint);
        }
    }
}

static void log_msg(const char *s) {
    if (!s) return;
    wasmos_console_write((int32_t)(uintptr_t)s, (int32_t)strlen(s));
}

static int str_ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static void unpack_name(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char *out, uint32_t out_len) {
    uint32_t args[4] = { arg0, arg1, arg2, arg3 };
    uint32_t pos = 0;
    if (!out || out_len == 0) return;
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

static void pack_name(const char *name, int32_t *arg0, int32_t *arg1, int32_t *arg2, int32_t *arg3) {
    uint32_t packed[4] = {0, 0, 0, 0};
    if (name) {
        for (uint32_t i = 0; name[i] && i < 16u; ++i) {
            uint32_t slot = i / 4u;
            uint32_t shift = (i % 4u) * 8u;
            packed[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
        }
    }
    *arg0 = (int32_t)packed[0];
    *arg1 = (int32_t)packed[1];
    *arg2 = (int32_t)packed[2];
    *arg3 = (int32_t)packed[3];
}

static fs_client_state_t *client_state(int32_t source) {
    for (uint32_t i = 0; i < FS_CLIENT_CAP; ++i) {
        if (g_clients[i].in_use && g_clients[i].source == source) {
            return &g_clients[i];
        }
    }
    for (uint32_t i = 0; i < FS_CLIENT_CAP; ++i) {
        if (!g_clients[i].in_use) {
            g_clients[i].in_use = 1;
            g_clients[i].source = source;
            g_clients[i].mount = FS_MOUNT_ROOT;
            return &g_clients[i];
        }
    }
    return 0;
}

static int lookup_backends(void) {
    if (g_backend_boot < 0) {
        g_backend_boot = wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "fs", 1);
    }
    if (g_backend_init < 0) {
        g_backend_init = wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "fs.init", 1);
    }
    return (g_backend_boot >= 0 && g_backend_init >= 0) ? 0 : -1;
}

static int send_virtual_root_listing(int32_t source, int32_t req_id) {
    const char *root_listing = "init/\nboot/\n";
    uint32_t pos = 0;
    uint32_t len = (uint32_t)strlen(root_listing);
    while (pos < len) {
        int32_t a0 = (int32_t)(uint8_t)root_listing[pos++];
        int32_t a1 = 0;
        int32_t a2 = 0;
        int32_t a3 = 0;
        if (pos < len) a1 = (int32_t)(uint8_t)root_listing[pos++];
        if (pos < len) a2 = (int32_t)(uint8_t)root_listing[pos++];
        if (pos < len) a3 = (int32_t)(uint8_t)root_listing[pos++];
        if (wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_STREAM, req_id, a0, a1, a2, a3) != 0) {
            return -1;
        }
    }
    return wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, req_id, 0, 0, 0, 0);
}

static int forward_request(int32_t backend,
                           int32_t type,
                           int32_t req_id,
                           int32_t arg0,
                           int32_t arg1,
                           int32_t arg2,
                           int32_t arg3,
                           int32_t source,
                           int32_t *out_resp_type,
                           int32_t *out_r0,
                           int32_t *out_r1,
                           int32_t *out_r2,
                           int32_t *out_r3) {
    if (backend < 0) return -1;
    if (wasmos_ipc_send(backend, g_reply_endpoint, type, req_id, arg0, arg1, arg2, arg3) != 0) {
        return -1;
    }
    for (;;) {
        if (wasmos_ipc_recv(g_reply_endpoint) < 0) {
            return -1;
        }
        int32_t resp_type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t resp_req = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t rr0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t rr1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t rr2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t rr3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        if (resp_req != req_id) {
            continue;
        }
        if (resp_type == FS_IPC_STREAM) {
            if (wasmos_ipc_send(source, g_fs_endpoint, resp_type, req_id, rr0, rr1, rr2, rr3) != 0) {
                return -1;
            }
            continue;
        }
        *out_resp_type = resp_type;
        *out_r0 = rr0;
        *out_r1 = rr1;
        *out_r2 = rr2;
        *out_r3 = rr3;
        return 0;
    }
}

WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;

    g_proc_endpoint = proc_endpoint;
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    g_fs_endpoint = wasmos_ipc_create_endpoint();
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0 || g_fs_endpoint < 0) {
        log_msg("[fs-manager] endpoint init failed\n");
        stall_forever();
    }
    if (wasmos_svc_register(g_proc_endpoint, g_fs_endpoint, "fs.vfs", 1) != 0) {
        log_msg("[fs-manager] register fs.vfs failed\n");
        stall_forever();
    }

    for (;;) {
        if (lookup_backends() != 0) {
            (void)wasmos_sched_yield();
            continue;
        }
        if (wasmos_ipc_recv(g_fs_endpoint) < 0) {
            continue;
        }

        int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t request_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t arg1f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t arg2f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t arg3f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);

        fs_client_state_t *state = client_state(source);
        if (!state) {
            (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
            continue;
        }

        if (type == FS_IPC_LIST_ROOT_REQ && state->mount == FS_MOUNT_ROOT) {
            (void)send_virtual_root_listing(source, request_id);
            continue;
        }

        if (type == FS_IPC_CHDIR_REQ) {
            char path[32];
            unpack_name((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));

            if (str_ieq(path, "") || str_ieq(path, "/")) {
                state->mount = FS_MOUNT_ROOT;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
            if (str_ieq(path, "..") && state->mount == FS_MOUNT_ROOT) {
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
            if (str_ieq(path, "init") || str_ieq(path, "/init")) {
                int32_t s0, s1, s2, s3;
                int32_t rr_t, rr0, rr1, rr2, rr3;
                state->mount = FS_MOUNT_INIT;
                pack_name("/", &s0, &s1, &s2, &s3);
                if (forward_request(g_backend_init, FS_IPC_CHDIR_REQ, request_id, s0, s1, s2, s3,
                                    source, &rr_t, &rr0, &rr1, &rr2, &rr3) != 0) {
                    state->mount = FS_MOUNT_ROOT;
                    (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
                    continue;
                }
                (void)wasmos_ipc_send(source, g_fs_endpoint, rr_t, request_id, rr0, rr1, rr2, rr3);
                continue;
            }
            if (str_ieq(path, "boot") || str_ieq(path, "/boot")) {
                int32_t s0, s1, s2, s3;
                int32_t rr_t, rr0, rr1, rr2, rr3;
                state->mount = FS_MOUNT_BOOT;
                pack_name("/", &s0, &s1, &s2, &s3);
                if (forward_request(g_backend_boot, FS_IPC_CHDIR_REQ, request_id, s0, s1, s2, s3,
                                    source, &rr_t, &rr0, &rr1, &rr2, &rr3) != 0) {
                    state->mount = FS_MOUNT_ROOT;
                    (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
                    continue;
                }
                (void)wasmos_ipc_send(source, g_fs_endpoint, rr_t, request_id, rr0, rr1, rr2, rr3);
                continue;
            }
        }

        if (type == FS_IPC_CAT_ROOT_REQ && state->mount == FS_MOUNT_ROOT) {
            char name[32];
            int32_t resp_type = FS_IPC_ERROR;
            int32_t r0 = -1, r1 = 0, r2 = 0, r3 = 0;
            unpack_name((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, name, sizeof(name));
            if (str_ieq(name, "init")) {
                state->mount = FS_MOUNT_INIT;
                if (forward_request(g_backend_init, FS_IPC_LIST_ROOT_REQ, request_id, 0, 0, 0, 0,
                                    source, &resp_type, &r0, &r1, &r2, &r3) != 0) {
                    state->mount = FS_MOUNT_ROOT;
                    (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
                    continue;
                }
                (void)wasmos_ipc_send(source, g_fs_endpoint, resp_type, request_id, r0, r1, r2, r3);
                continue;
            } else if (str_ieq(name, "boot")) {
                state->mount = FS_MOUNT_BOOT;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            } else {
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
                continue;
            }
        }

        int32_t backend = (state->mount == FS_MOUNT_INIT) ? g_backend_init : g_backend_boot;
        int32_t resp_type = FS_IPC_ERROR;
        int32_t r0 = -1, r1 = 0, r2 = 0, r3 = 0;
        if (forward_request(backend, type, request_id, arg0, arg1f, arg2f, arg3f,
                            source, &resp_type, &r0, &r1, &r2, &r3) != 0) {
            if (type == FS_IPC_CHDIR_REQ && state->mount != FS_MOUNT_ROOT) {
                char path[32];
                unpack_name((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));
                if (str_ieq(path, "..")) {
                    state->mount = FS_MOUNT_ROOT;
                    (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                    continue;
                }
            }
            (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
            continue;
        }
        if (type == FS_IPC_CHDIR_REQ && resp_type == FS_IPC_ERROR) {
            char path[32];
            unpack_name((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));
            if (str_ieq(path, "..") && state->mount != FS_MOUNT_ROOT) {
                state->mount = FS_MOUNT_ROOT;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
        }

        (void)wasmos_ipc_send(source, g_fs_endpoint, resp_type, request_id, r0, r1, r2, r3);
    }
}
