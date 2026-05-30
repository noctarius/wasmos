#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos_driver_abi.h"
#include "fs_manager_types.h"
#include "fs_manager_path.h"

static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static fs_backend_t g_backends[FS_BACKEND_CAP];
extern uint8_t __heap_base;

typedef struct fs_client_chunk {
    struct fs_client_chunk *next;
    uint32_t used;
    fs_client_state_t slots[FS_CLIENT_CHUNK_CAP];
} fs_client_chunk_t;

static fs_client_chunk_t *g_client_chunks = 0;
static uint32_t g_heap_cursor = 0;
static uint32_t g_heap_limit = 0;

static void
fsmgr_heap_init(void)
{
    g_heap_cursor = (uint32_t)(uintptr_t)&__heap_base;
    g_heap_limit = (uint32_t)__builtin_wasm_memory_size(0) * 65536u;
    if (g_heap_cursor > g_heap_limit) {
        g_heap_cursor = g_heap_limit;
    }
}

static void *
fsmgr_heap_alloc(uint32_t size, uint32_t align)
{
    uint32_t aligned = 0;
    uint32_t end = 0;
    if (align == 0 || (align & (align - 1u)) != 0u) {
        return 0;
    }
    aligned = (g_heap_cursor + (align - 1u)) & ~(align - 1u);
    if (aligned < g_heap_cursor) {
        return 0;
    }
    end = aligned + size;
    if (end < aligned) {
        return 0;
    }
    while (end > g_heap_limit) {
        if (__builtin_wasm_memory_grow(0, 1) == (size_t)-1) {
            return 0;
        }
        g_heap_limit += 65536u;
    }
    g_heap_cursor = end;
    return (void *)(uintptr_t)aligned;
}

static fs_client_chunk_t *
client_chunk_alloc(void)
{
    fs_client_chunk_t *chunk =
        (fs_client_chunk_t *)fsmgr_heap_alloc((uint32_t)sizeof(fs_client_chunk_t), 8u);
    if (!chunk) {
        return 0;
    }
    memset(chunk, 0, sizeof(*chunk));
    return chunk;
}

static int32_t
borrow_flags_for_type(int32_t type)
{
    if (type == FS_IPC_WRITE_REQ) {
        return WASMOS_BUFFER_GRANT_READ;
    }
    if (type == FS_IPC_READ_REQ || type == FS_IPC_READ_APP_REQ) {
        return WASMOS_BUFFER_GRANT_WRITE;
    }
    return 0;
}

static void log_msg(const char *s) {
    if (!s) return;
    (void)printf("%s", s);
}

static void set_mount_name(fs_backend_t *slot, const char *base) {
    char buf[16];
    uint8_t tmp[3];
    uint32_t n = 0;
    uint32_t pos = 0;
    if (!slot) return;
    wasmos_sys_strcpy(buf, sizeof(buf), base);
    if (slot->slot == 0) {
        wasmos_sys_strcpy(slot->mount_name, sizeof(slot->mount_name), buf);
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
    wasmos_sys_strcpy(slot->mount_name, sizeof(slot->mount_name), buf);
}

static fs_client_state_t *
client_state_lookup(int32_t context_id)
{
    fs_client_chunk_t *chunk = g_client_chunks;
    while (chunk) {
        for (uint32_t i = 0; i < chunk->used; ++i) {
            if (chunk->slots[i].in_use && chunk->slots[i].context_id == context_id) {
                return &chunk->slots[i];
            }
        }
        chunk = chunk->next;
    }
    return 0;
}

static fs_client_state_t *
client_state(int32_t context_id)
{
    fs_client_state_t *state = client_state_lookup(context_id);
    fs_client_chunk_t *chunk = g_client_chunks;
    fs_client_chunk_t *last = 0;
    if (state) {
        return state;
    }
    while (chunk) {
        if (chunk->used < FS_CLIENT_CHUNK_CAP) {
            fs_client_state_t *slot = &chunk->slots[chunk->used++];
            slot->in_use = 1;
            slot->context_id = context_id;
            slot->mount = FS_MOUNT_ROOT;
            slot->backend_endpoint = -1;
            slot->mount_depth = 0;
            return slot;
        }
        last = chunk;
        chunk = chunk->next;
    }
    chunk = client_chunk_alloc();
    if (!chunk) {
        return 0;
    }
    if (last) {
        last->next = chunk;
    } else {
        g_client_chunks = chunk;
    }
    chunk->used = 1;
    chunk->slots[0].in_use = 1;
    chunk->slots[0].context_id = context_id;
    chunk->slots[0].mount = FS_MOUNT_ROOT;
    chunk->slots[0].backend_endpoint = -1;
    chunk->slots[0].mount_depth = 0;
    return &chunk->slots[0];
}

static fs_backend_t *
backend_find_by_name(const char *name)
{
    if (!name) {
        return 0;
    }
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && strcasecmp(g_backends[i].mount_name, name) == 0) {
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
    slot->has_meta = 0;
    slot->unit = 0xFFu;
    if (kind == FSMGR_BACKEND_BOOT) {
        if (slot->slot == 0) {
            wasmos_sys_strcpy(slot->mount_name, sizeof(slot->mount_name), "boot");
        } else if (slot->slot == 1) {
            wasmos_sys_strcpy(slot->mount_name, sizeof(slot->mount_name), "user");
        } else {
            set_mount_name(slot, "boot");
        }
    } else if (kind == FSMGR_BACKEND_INIT) {
        set_mount_name(slot, "init");
    } else {
        set_mount_name(slot, "fs");
    }
    return slot;
}

static void
backend_refresh_boot_meta(fs_backend_t *slot, int32_t req_seed)
{
    int32_t devmgr = -1;
    int32_t req_id = req_seed;
    if (!slot || slot->kind != FSMGR_BACKEND_BOOT || g_proc_endpoint < 0 || g_reply_endpoint < 0) {
        return;
    }
    devmgr = wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "devmgr.query", 1);
    if (devmgr < 0) {
        return;
    }
    if (wasmos_ipc_send(devmgr, g_reply_endpoint, DEVMGR_QUERY_MOUNT_REQ, req_id, 0, 0, 0, 0) != 0 ||
        wasmos_ipc_recv(g_reply_endpoint) < 0 ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) != req_id ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != DEVMGR_MOUNT_INFO) {
        return;
    }
    {
        uint32_t a1 = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        uint32_t a2 = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        uint32_t a3 = (uint32_t)wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        if ((a3 & (1u << 31)) == 0u) {
            return;
        }
        slot->has_meta = 1;
        slot->bus = (uint8_t)((a1 >> 24) & 0xFFu);
        slot->device_fn = (uint8_t)((a1 >> 8) & 0xFFu);
        slot->class_code = (uint8_t)(a1 & 0xFFu);
        slot->subclass = (uint8_t)((a2 >> 24) & 0xFFu);
        slot->prog_if = (uint8_t)((a2 >> 16) & 0xFFu);
        slot->vendor_id = (uint16_t)(a2 & 0xFFFFu);
        slot->device_id = (uint16_t)(a3 & 0xFFFFu);
    }
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

static int
fsmgr_emit_mounts(int32_t source, int32_t req_id)
{
    char mounts[384];
    uint32_t pos = 0;
    wasmos_sys_strcpy(mounts, sizeof(mounts), "mounts:\n");
    pos = (uint32_t)strlen(mounts);
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        const char *kind = "fs";
        int n = 0;
        if (!g_backends[i].in_use) {
            continue;
        }
        if (g_backends[i].kind == FSMGR_BACKEND_BOOT) {
            kind = "fs-fat";
        } else if (g_backends[i].kind == FSMGR_BACKEND_INIT) {
            kind = "fs-init";
        }
        n = snprintf(mounts + pos,
                     sizeof(mounts) - pos,
                     "/%s -> %s",
                     g_backends[i].mount_name,
                     kind);
        if (n > 0 && (uint32_t)n < sizeof(mounts) - pos &&
            g_backends[i].kind == FSMGR_BACKEND_BOOT && g_backends[i].has_meta) {
            uint8_t dev = (uint8_t)((g_backends[i].device_fn >> 4) & 0x1Fu);
            uint8_t fun = (uint8_t)(g_backends[i].device_fn & 0x07u);
            int m = snprintf(mounts + pos + (uint32_t)n,
                             sizeof(mounts) - (pos + (uint32_t)n),
                             " pci %02X:%02X.%02X class %02X:%02X:%02X vid:did %04X:%04X unit %u",
                             (unsigned)g_backends[i].bus,
                             (unsigned)dev,
                             (unsigned)fun,
                             (unsigned)g_backends[i].class_code,
                             (unsigned)g_backends[i].subclass,
                             (unsigned)g_backends[i].prog_if,
                             (unsigned)g_backends[i].vendor_id,
                             (unsigned)g_backends[i].device_id,
                             (unsigned)g_backends[i].unit);
            if (m > 0) {
                n += m;
            }
        }
        if (n > 0 && pos + (uint32_t)n + 1u < sizeof(mounts)) {
            mounts[pos + (uint32_t)n] = '\n';
            mounts[pos + (uint32_t)n + 1u] = '\0';
            n += 1;
        }
        if (n <= 0) {
            continue;
        }
        pos += (uint32_t)n;
        if (pos + 1u >= sizeof(mounts)) {
            break;
        }
    }
    if (wasmos_sys_buffer_write_to(WASMOS_BUFFER_KIND_FS,
                                   source,
                                   WASMOS_BUFFER_GRANT_WRITE,
                                   mounts,
                                   (int32_t)pos,
                                   0) != 0) {
        return -1;
    }
    return wasmos_ipc_send(source, g_fs_endpoint, FSMGR_IPC_QUERY_MOUNTS_RESP, req_id, (int32_t)pos, 0, 0, 0);
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

static void
send_fs_error(int32_t source, int32_t request_id)
{
    (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, request_id, -1, 0, 0, 0);
}

static int32_t
resolve_backend_for_state(const fs_client_state_t *state)
{
    int32_t backend = state ? state->backend_endpoint : -1;
    if (backend < 0) {
        fs_backend_t *fallback_boot = backend_first_of_kind(FSMGR_BACKEND_BOOT);
        backend = fallback_boot ? fallback_boot->endpoint : -1;
    }
    return backend;
}

static int
is_path_op_type(int32_t type)
{
    return type == FS_IPC_OPEN_REQ ||
           type == FS_IPC_STAT_REQ ||
           type == FS_IPC_UNLINK_REQ ||
           type == FS_IPC_MKDIR_REQ ||
           type == FS_IPC_RMDIR_REQ;
}

static int32_t
route_path_to_backend(const uint8_t *path_bytes,
                      int32_t path_len,
                      int32_t allow_relative,
                      char *out_path,
                      int32_t out_path_cap,
                      int32_t *out_path_len,
                      int32_t *out_backend)
{
    const char *mount_names[FS_BACKEND_CAP];
    int32_t mount_endpoints[FS_BACKEND_CAP];
    int32_t mount_count = 0;
    int32_t mount_index = -1;
    int32_t routed = 0;

    if (!path_bytes || path_len <= 0 || !out_path || !out_path_len || !out_backend) {
        return 0;
    }
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (!g_backends[i].in_use) {
            continue;
        }
        mount_names[mount_count] = g_backends[i].mount_name;
        mount_endpoints[mount_count] = g_backends[i].endpoint;
        mount_count++;
    }
    if (mount_count <= 0) {
        return 0;
    }
    routed = fsmgr_route_path_for_mounts((const char *)path_bytes,
                                         path_len,
                                         mount_names,
                                         mount_count,
                                         allow_relative,
                                         &mount_index,
                                         out_path,
                                         out_path_cap,
                                         out_path_len);
    if (!routed || mount_index < 0 || mount_index >= mount_count) {
        return 0;
    }
    *out_backend = mount_endpoints[mount_index];
    return 1;
}

static int
route_root_path_request(fs_client_state_t *state,
                        int32_t source,
                        int32_t type,
                        int32_t *inout_arg0,
                        int32_t *out_backend)
{
    int32_t path_len = inout_arg0 ? *inout_arg0 : 0;
    int32_t fs_buf_size = wasmos_fs_buffer_size();
    uint8_t scratch[256];
    int32_t routed_backend = out_backend ? *out_backend : -1;
    int32_t open_path_len = path_len;

    if (!state || !inout_arg0 || !out_backend || !is_path_op_type(type)) {
        return 0;
    }
    if (path_len <= 0 || path_len >= fs_buf_size || path_len >= (int32_t)sizeof(scratch) - 1) {
        return -1;
    }
    if (wasmos_sys_fs_buffer_copy_from_endpoint(source, scratch, path_len, 0) != 0) {
        return -1;
    }
    scratch[path_len] = '\0';
    if (scratch[0] == '/' && scratch[1] != '\0') {
        (void)route_path_to_backend(scratch,
                                    path_len,
                                    0,
                                    (char *)scratch,
                                    (int32_t)sizeof(scratch),
                                    &open_path_len,
                                    &routed_backend);
    }
    if (open_path_len <= 0 ||
        wasmos_fs_buffer_write((int32_t)(uintptr_t)scratch, open_path_len, 0) != 0) {
        return -1;
    }
    *inout_arg0 = open_path_len;
    *out_backend = routed_backend;
    return 1;
}

static int
handle_register_backend_req(int32_t source, int32_t request_id, int32_t arg0, int32_t arg1f, int32_t arg2f, int32_t arg3f)
{
    int32_t backend_endpoint = arg1f > 0 ? arg1f : source;
    fs_backend_t *registered = backend_register((uint8_t)arg0, backend_endpoint);
    int32_t mount_len = arg2f;
    if (!registered) {
        send_fs_error(source, request_id);
        return 1;
    }
    registered->unit = (uint8_t)(arg3f & 0xFF);
    if (mount_len > 0 && mount_len < (int32_t)sizeof(registered->mount_name)) {
        char mount_name[16];
        int32_t copy_len = mount_len;
        if (copy_len >= (int32_t)sizeof(mount_name)) {
            copy_len = (int32_t)sizeof(mount_name) - 1;
        }
        if (wasmos_buffer_borrow(WASMOS_BUFFER_KIND_FS, source, WASMOS_BUFFER_GRANT_READ) == 0) {
            if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)mount_name, copy_len, 0) == 0) {
                mount_name[copy_len] = '\0';
                if (mount_name[0] == '/') {
                                wasmos_sys_strcpy(registered->mount_name, sizeof(registered->mount_name), &mount_name[1]);
                            } else {
                                wasmos_sys_strcpy(registered->mount_name, sizeof(registered->mount_name), mount_name);
                            }
                            wasmos_sys_to_lower_ascii(registered->mount_name);
            }
            (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
        }
    }
    backend_refresh_boot_meta(registered, request_id + 1);
    (void)wasmos_ipc_send(source, g_fs_endpoint, FSMGR_IPC_REGISTER_BACKEND_RESP, request_id, 0, registered->slot, 0, 0);
    return 1;
}

static int
handle_clone_cwd_req(int32_t source, int32_t source_owner, int32_t request_id, int32_t arg0, int32_t arg1f)
{
    fs_client_state_t *src_state = 0;
    fs_client_state_t *dst_state = 0;
    int32_t src_context_id = arg0;
    int32_t dst_context_id = arg1f;
    int32_t proc_owner = wasmos_ipc_endpoint_owner(g_proc_endpoint);
    if (source_owner < 0 ||
        proc_owner < 0 ||
        source_owner != proc_owner ||
        src_context_id <= 0 ||
        dst_context_id <= 0) {
        send_fs_error(source, request_id);
        return 1;
    }
    src_state = client_state(src_context_id);
    dst_state = client_state(dst_context_id);
    if (!src_state || !dst_state) {
        send_fs_error(source, request_id);
        return 1;
    }
    dst_state->mount = src_state->mount;
    dst_state->backend_endpoint = src_state->backend_endpoint;
    dst_state->mount_depth = src_state->mount_depth;
    (void)wasmos_ipc_send(source, g_fs_endpoint, FSMGR_IPC_CLONE_CWD_RESP, request_id, 0, 0, 0, 0);
    return 1;
}

static int
handle_read_path_req(fs_client_state_t *state, int32_t source, int32_t request_id, int32_t path_len)
{
    int32_t backend = state ? state->backend_endpoint : -1;
    int32_t resp_type = FS_IPC_ERROR;
    int32_t r0 = -1, r1 = 0, r2 = 0, r3 = 0;
    int32_t open_t = FS_IPC_ERROR, open0 = -1, open1 = 0, open2 = 0, open3 = 0;
    int32_t read_t = FS_IPC_ERROR, read0 = -1, read1 = 0, read2 = 0, read3 = 0;
    int32_t close_t = FS_IPC_ERROR, close0 = -1, close1 = 0, close2 = 0, close3 = 0;
    int32_t fd = -1;
    int32_t fs_buf_size = wasmos_fs_buffer_size();
    uint8_t scratch[256];
    int32_t open_path_len = 0;

    if (path_len <= 0 ||
        path_len >= fs_buf_size ||
        path_len >= (int32_t)sizeof(scratch) - 1) {
        send_fs_error(source, request_id);
        return 1;
    }
    if (wasmos_buffer_borrow(WASMOS_BUFFER_KIND_FS,
                             source,
                             WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE) != 0) {
        send_fs_error(source, request_id);
        return 1;
    }
    if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)scratch, path_len, 0) != 0) {
        (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
        send_fs_error(source, request_id);
        return 1;
    }
    scratch[path_len] = '\0';
    if (scratch[0] != '/') {
        (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
        send_fs_error(source, request_id);
        return 1;
    }
    open_path_len = path_len;
    (void)route_path_to_backend(scratch,
                                path_len,
                                0,
                                (char *)scratch,
                                (int32_t)sizeof(scratch),
                                &open_path_len,
                                &backend);
    if (backend < 0) {
        fs_backend_t *fallback_boot = backend_first_of_kind(FSMGR_BACKEND_BOOT);
        backend = fallback_boot ? fallback_boot->endpoint : -1;
    }
    if (backend < 0) {
        (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
        send_fs_error(source, request_id);
        return 1;
    }
    (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
    if (open_path_len <= 0 ||
        wasmos_fs_buffer_write((int32_t)(uintptr_t)scratch, open_path_len, 0) != 0) {
        send_fs_error(source, request_id);
        return 1;
    }
    {
        int32_t open_rc = forward_request(backend, FS_IPC_OPEN_REQ, request_id, open_path_len, 0, 0, 0,
                                          source, &open_t, &open0, &open1, &open2, &open3);
        if (open_rc != 0 || open_t != FS_IPC_RESP || open0 < 0) {
            send_fs_error(source, request_id);
            return 1;
        }
    }
    fd = open0;
    if (forward_request(backend, FS_IPC_READ_REQ, request_id, fd, fs_buf_size, 0, 0,
                        source, &read_t, &read0, &read1, &read2, &read3) != 0 ||
        read_t != FS_IPC_RESP || read0 <= 0 || read0 > fs_buf_size) {
        (void)forward_request(backend, FS_IPC_CLOSE_REQ, request_id, fd, 0, 0, 0,
                              source, &close_t, &close0, &close1, &close2, &close3);
        send_fs_error(source, request_id);
        return 1;
    }
    (void)forward_request(backend, FS_IPC_CLOSE_REQ, request_id, fd, 0, 0, 0,
                          source, &close_t, &close0, &close1, &close2, &close3);
    {
        int32_t copied = 0;
        while (copied < read0) {
            int32_t chunk = read0 - copied;
            if (chunk > (int32_t)sizeof(scratch)) {
                chunk = (int32_t)sizeof(scratch);
            }
            if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)scratch, chunk, copied) != 0) {
                send_fs_error(source, request_id);
                return 1;
            }
            if (wasmos_sys_buffer_write_to(WASMOS_BUFFER_KIND_FS,
                                           source,
                                           WASMOS_BUFFER_GRANT_WRITE,
                                           scratch,
                                           chunk,
                                           copied) != 0) {
                send_fs_error(source, request_id);
                return 1;
            }
            copied += chunk;
        }
    }
    resp_type = FS_IPC_RESP;
    r0 = read0;
    (void)wasmos_ipc_send(source, g_fs_endpoint, resp_type, request_id, r0, r1, r2, r3);
    return 1;
}

static int
handle_chdir_mount(fs_client_state_t *state,
                   int32_t source,
                   int32_t request_id,
                   int32_t arg0,
                   int32_t arg1f,
                   int32_t arg2f,
                   int32_t arg3f)
{
    char path[32];
    wasmos_sys_ipc_unpack_name16((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));

    if (strcasecmp(path, "") == 0 || strcasecmp(path, "/") == 0) {
        state->mount = FS_MOUNT_ROOT;
        state->backend_endpoint = -1;
        state->mount_depth = 0;
        (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
        return 1;
    }
    if (strcasecmp(path, "..") == 0 && state->mount == FS_MOUNT_ROOT) {
        (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
        return 1;
    }
    if (strcasecmp(path, "..") == 0 && state->mount != FS_MOUNT_ROOT && state->mount_depth == 0) {
        state->mount = FS_MOUNT_ROOT;
        state->backend_endpoint = -1;
        (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
        return 1;
    }
    {
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
            {
                int32_t args[4];
                wasmos_sys_ipc_pack_name16("/", args);
                s0 = args[0];
                s1 = args[1];
                s2 = args[2];
                s3 = args[3];
            }
            if (forward_request(target->endpoint, FS_IPC_CHDIR_REQ, request_id, s0, s1, s2, s3,
                                source, &rr_t, &rr0, &rr1, &rr2, &rr3) != 0) {
                state->mount = FS_MOUNT_ROOT;
                state->backend_endpoint = -1;
                send_fs_error(source, request_id);
                return 1;
            }
            (void)wasmos_ipc_send(source, g_fs_endpoint, rr_t, request_id, rr0, rr1, rr2, rr3);
            return 1;
        }
    }
    return 0;
}

WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;

    g_proc_endpoint = proc_endpoint;
    fsmgr_heap_init();
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    g_fs_endpoint = wasmos_ipc_create_endpoint();
    log_msg("[fs-manager] init start\n");
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0 || g_fs_endpoint < 0) {
        log_msg("[fs-manager] endpoint init failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    if (wasmos_svc_register(g_proc_endpoint, g_fs_endpoint, "fs.vfs", 1) != 0) {
        log_msg("[fs-manager] register fs.vfs failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    log_msg("[fs-manager] services registered\n");
    wasmos_sys_notify_ready(g_proc_endpoint, g_fs_endpoint);

    for (;;) {
        if (wasmos_ipc_recv(g_fs_endpoint) < 0) {
            continue;
        }

        int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t request_id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t source = wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE);
        int32_t source_owner = wasmos_ipc_endpoint_owner(source);
        int32_t client_key = source_owner;
        int32_t arg0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t arg1f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t arg2f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t arg3f = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);

        if (type == FSMGR_IPC_REGISTER_BACKEND_REQ) {
            (void)handle_register_backend_req(source, request_id, arg0, arg1f, arg2f, arg3f);
            continue;
        }

        if (type == FSMGR_IPC_CLONE_CWD_REQ) {
            (void)handle_clone_cwd_req(source, source_owner, request_id, arg0, arg1f);
            continue;
        }
        if (type == FSMGR_IPC_QUERY_MOUNTS_REQ) {
            if (fsmgr_emit_mounts(source, request_id) != 0) {
                send_fs_error(source, request_id);
            }
            continue;
        }

        if (client_key < 0) {
            client_key = source;
        }

        fs_client_state_t *state = client_state(client_key);
        if (!state) {
            send_fs_error(source, request_id);
            continue;
        }

        if (type == FS_IPC_READ_PATH_REQ) {
            (void)handle_read_path_req(state, source, request_id, arg0);
            continue;
        }

        if (type == FS_IPC_READDIR_REQ && state->mount == FS_MOUNT_ROOT) {
            (void)send_virtual_root_listing(source, request_id);
            continue;
        }

        if (type == FS_IPC_CHDIR_REQ) {
            if (handle_chdir_mount(state, source, request_id, arg0, arg1f, arg2f, arg3f) != 0) {
                continue;
            }
        }

        int32_t req_arg0 = arg0;
        int32_t backend = resolve_backend_for_state(state);
        int32_t resp_type = FS_IPC_ERROR;
        int32_t r0 = -1, r1 = 0, r2 = 0, r3 = 0;
        int32_t borrow_flags = borrow_flags_for_type(type);
        int32_t borrowed = 0;
        if (is_path_op_type(type)) {
            if (route_root_path_request(state, source, type, &req_arg0, &backend) < 0) {
                send_fs_error(source, request_id);
                continue;
            }
        }
        if (borrow_flags != 0) {
            if (wasmos_buffer_borrow(WASMOS_BUFFER_KIND_FS, source, borrow_flags) != 0) {
                send_fs_error(source, request_id);
                continue;
            }
            borrowed = 1;
        }
        if (forward_request(backend, type, request_id, req_arg0, arg1f, arg2f, arg3f,
                            source, &resp_type, &r0, &r1, &r2, &r3) != 0) {
            if (borrowed) {
                (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
            }
            if (type == FS_IPC_CHDIR_REQ && state->mount != FS_MOUNT_ROOT) {
                char path[32];
                wasmos_sys_ipc_unpack_name16((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));
                if (strcasecmp(path, "..") == 0) {
                    state->mount = FS_MOUNT_ROOT;
                    state->backend_endpoint = -1;
                    (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                    continue;
                }
            }
            send_fs_error(source, request_id);
            continue;
        }
        if (borrowed) {
            (void)wasmos_buffer_release(WASMOS_BUFFER_KIND_FS);
        }
        if (type == FS_IPC_CHDIR_REQ && resp_type == FS_IPC_ERROR) {
            char path[32];
            wasmos_sys_ipc_unpack_name16((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));
            if (strcasecmp(path, "..") == 0 && state->mount != FS_MOUNT_ROOT) {
                state->mount = FS_MOUNT_ROOT;
                state->backend_endpoint = -1;
                state->mount_depth = 0;
                (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
        }
        if (type == FS_IPC_CHDIR_REQ && resp_type == FS_IPC_RESP && state->mount != FS_MOUNT_ROOT) {
            char path[32];
            wasmos_sys_ipc_unpack_name16((uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));
            if (strcasecmp(path, "..") == 0) {
                if (state->mount_depth > 0) {
                    state->mount_depth--;
                }
            } else if (strcasecmp(path, ".") != 0 && strcasecmp(path, "") != 0) {
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
