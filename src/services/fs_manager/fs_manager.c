#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

#define FS_CLIENT_CAP 16
#define FS_BACKEND_CAP 8

typedef enum {
    FS_MOUNT_ROOT = 0,
    FS_MOUNT_BACKEND = 1
} fs_mount_t;

typedef struct {
    uint8_t in_use;
    uint8_t kind;
    int32_t endpoint;
    uint8_t slot;
    char mount_name[16];
} fs_backend_t;

typedef struct {
    uint8_t in_use;
    int32_t source;
    fs_mount_t mount;
    int32_t backend_endpoint;
    uint16_t mount_depth;
} fs_client_state_t;

static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static fs_client_state_t g_clients[FS_CLIENT_CAP];
static fs_backend_t g_backends[FS_BACKEND_CAP];

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
    (void)printf("%s", s);
}

static int str_ieq(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    for (;;) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
    }
}

static void str_copy(char *dst, uint32_t dst_len, const char *src) {
    uint32_t i = 0;
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_len && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void set_mount_name(fs_backend_t *slot, const char *base) {
    char buf[16];
    uint8_t tmp[3];
    uint32_t n = 0;
    uint32_t pos = 0;
    if (!slot) return;
    str_copy(buf, sizeof(buf), base);
    if (slot->slot == 0) {
        str_copy(slot->mount_name, sizeof(slot->mount_name), buf);
        return;
    }
    while (buf[pos] && pos + 1 < sizeof(buf)) {
        ++pos;
    }
    uint8_t value = slot->slot;
    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = (uint8_t)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0 && pos + 1 < sizeof(buf)) {
        buf[pos++] = (char)tmp[--n];
    }
    buf[pos] = '\0';
    str_copy(slot->mount_name, sizeof(slot->mount_name), buf);
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
            g_clients[i].backend_endpoint = -1;
            g_clients[i].mount_depth = 0;
            return &g_clients[i];
        }
    }
    return 0;
}

static fs_backend_t *
backend_find_by_name(const char *name)
{
    if (!name) {
        return 0;
    }
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && str_ieq(g_backends[i].mount_name, name)) {
            return &g_backends[i];
        }
    }
    return 0;
}

static fs_backend_t *
backend_first_of_kind(uint8_t kind)
{
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && g_backends[i].kind == kind) {
            return &g_backends[i];
        }
    }
    return 0;
}

static fs_backend_t *
backend_register(uint8_t kind, int32_t endpoint)
{
    fs_backend_t *slot = 0;
    uint8_t kind_slot = 0;
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && g_backends[i].endpoint == endpoint) {
            slot = &g_backends[i];
            break;
        }
        if (g_backends[i].in_use && g_backends[i].kind == kind && g_backends[i].slot >= kind_slot) {
            kind_slot = (uint8_t)(g_backends[i].slot + 1u);
        }
    }
    if (!slot) {
        for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
            if (!g_backends[i].in_use) {
                slot = &g_backends[i];
                slot->in_use = 1;
                slot->slot = kind_slot;
                break;
            }
        }
    }
    if (!slot) {
        return 0;
    }
    slot->kind = kind;
    slot->endpoint = endpoint;
    if (kind == FSMGR_BACKEND_BOOT) {
        set_mount_name(slot, "boot");
    } else if (kind == FSMGR_BACKEND_INIT) {
        set_mount_name(slot, "init");
    } else {
        set_mount_name(slot, "fs");
    }
    return slot;
}

static int
send_virtual_root_listing(int32_t source, int32_t req_id)
{
    char root_listing[256];
    uint32_t pos = 0;
    root_listing[0] = '\0';
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (!g_backends[i].in_use) {
            continue;
        }
        uint32_t name_len = (uint32_t)strlen(g_backends[i].mount_name);
        if (name_len + 2u >= (sizeof(root_listing) - pos)) {
            break;
        }
        for (uint32_t j = 0; j < name_len; ++j) {
            root_listing[pos++] = g_backends[i].mount_name[j];
        }
        root_listing[pos++] = '/';
        root_listing[pos++] = '\n';
        root_listing[pos] = '\0';
    }
    pos = 0;
    uint32_t len = (uint32_t)strlen(root_listing);
    while (pos < len) {
        int32_t a0 = (int32_t)(uint8_t)root_listing[pos++];
        int32_t a1 = 0, a2 = 0, a3 = 0;
        if (pos < len) a1 = (int32_t)(uint8_t)root_listing[pos++];
        if (pos < len) a2 = (int32_t)(uint8_t)root_listing[pos++];
        if (pos < len) a3 = (int32_t)(uint8_t)root_listing[pos++];
        if (wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_STREAM, req_id, a0, a1, a2, a3) != 0) {
            return -1;
        }
    }
    return wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, req_id, 0, 0, 0, 0);
}

static int forward_request(int32_t backend_endpoint,
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
    if (backend_endpoint < 0) return -1;
    if (wasmos_ipc_send(backend_endpoint, g_reply_endpoint, type, req_id, arg0, arg1, arg2, arg3) != 0) {
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
    log_msg("[fs-manager] init start\n");
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0 || g_fs_endpoint < 0) {
        log_msg("[fs-manager] endpoint init failed\n");
        stall_forever();
    }
    if (wasmos_svc_register(g_proc_endpoint, g_fs_endpoint, "fs.vfs", 1) != 0) {
        log_msg("[fs-manager] register fs.vfs failed\n");
        stall_forever();
    }
    log_msg("[fs-manager] services registered\n");

    for (;;) {
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

        if (type == FSMGR_IPC_REGISTER_BACKEND_REQ) {
            int32_t backend_endpoint = arg1f > 0 ? arg1f : source;
            fs_backend_t *registered = backend_register((uint8_t)arg0, backend_endpoint);
            if (!registered) {
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
            } else {
                (void)wasmos_ipc_send(source, g_fs_endpoint, FSMGR_IPC_REGISTER_BACKEND_RESP, request_id, 0, registered->slot, 0, 0);
            }
            continue;
        }

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
                state->backend_endpoint = -1;
                state->mount_depth = 0;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
            if (str_ieq(path, "..") && state->mount == FS_MOUNT_ROOT) {
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
            if (str_ieq(path, "..") && state->mount != FS_MOUNT_ROOT && state->mount_depth == 0) {
                state->mount = FS_MOUNT_ROOT;
                state->backend_endpoint = -1;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
            const char *mount_name = path;
            if (path[0] == '/') {
                mount_name = &path[1];
            }
            fs_backend_t *target = backend_find_by_name(mount_name);
            if (target) {
                int32_t s0, s1, s2, s3;
                int32_t rr_t, rr0, rr1, rr2, rr3;
                state->mount = FS_MOUNT_BACKEND;
                state->backend_endpoint = target->endpoint;
                state->mount_depth = 0;
                pack_name("/", &s0, &s1, &s2, &s3);
                if (forward_request(target->endpoint, FS_IPC_CHDIR_REQ, request_id, s0, s1, s2, s3,
                                    source, &rr_t, &rr0, &rr1, &rr2, &rr3) != 0) {
                    state->mount = FS_MOUNT_ROOT;
                    state->backend_endpoint = -1;
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
            fs_backend_t *target = backend_find_by_name(name);
            if (!target) {
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
                continue;
            }
            state->mount = FS_MOUNT_BACKEND;
            state->backend_endpoint = target->endpoint;
            state->mount_depth = 0;
            if (forward_request(target->endpoint, FS_IPC_LIST_ROOT_REQ, request_id, 0, 0, 0, 0,
                                source, &resp_type, &r0, &r1, &r2, &r3) != 0) {
                state->mount = FS_MOUNT_ROOT;
                state->backend_endpoint = -1;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
                continue;
            }
            (void)wasmos_ipc_send(source, g_fs_endpoint, resp_type, request_id, r0, r1, r2, r3);
            continue;
        }

        int32_t backend = state->backend_endpoint;
        if (backend < 0) {
            fs_backend_t *fallback_boot = backend_first_of_kind(FSMGR_BACKEND_BOOT);
            backend = fallback_boot ? fallback_boot->endpoint : -1;
        }
        int32_t resp_type = FS_IPC_ERROR;
        int32_t r0 = -1, r1 = 0, r2 = 0, r3 = 0;
        if (forward_request(backend, type, request_id, arg0, arg1f, arg2f, arg3f,
                            source, &resp_type, &r0, &r1, &r2, &r3) != 0) {
            if (type == FS_IPC_CHDIR_REQ && state->mount != FS_MOUNT_ROOT) {
                char path[32];
                unpack_name((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));
                if (str_ieq(path, "..")) {
                    state->mount = FS_MOUNT_ROOT;
                    state->backend_endpoint = -1;
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
                state->backend_endpoint = -1;
                state->mount_depth = 0;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
        }
        if (type == FS_IPC_CHDIR_REQ && resp_type == FS_IPC_RESP && state->mount != FS_MOUNT_ROOT) {
            char path[32];
            unpack_name((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));
            if (str_ieq(path, "..")) {
                if (state->mount_depth > 0) {
                    state->mount_depth--;
                }
            } else if (!str_ieq(path, ".") && !str_ieq(path, "")) {
                if (path[0] == '/') {
                    state->mount_depth = (path[1] == '\0') ? 0 : 1;
                } else if (state->mount_depth < 0xFFFFu) {
                    state->mount_depth++;
                }
            }
        }

        (void)wasmos_ipc_send(source, g_fs_endpoint, resp_type, request_id, r0, r1, r2, r3);
    }
}
