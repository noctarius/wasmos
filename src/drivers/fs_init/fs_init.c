#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos_driver_abi.h"

#define INITFS_MAX_OPEN_FILES 16
#define INITFS_MAX_FILES 128
#define INITFS_MAX_DIRS 128
#define INITFS_MAX_CLIENTS 16
#define INITFS_PATH_MAX 112
#define INITFS_NAME_MAX 64

typedef struct {
    int32_t in_use;
    int32_t entry_index;
    int32_t offset;
} initfs_fd_t;

typedef struct {
    int32_t in_use;
    int32_t entry_index;
    int32_t size;
    int32_t dir_index;
    char path[INITFS_PATH_MAX];
    char name[INITFS_NAME_MAX];
} initfs_file_t;

typedef struct {
    int32_t in_use;
    int32_t parent_index;
    char path[INITFS_PATH_MAX];
    char name[INITFS_NAME_MAX];
} initfs_dir_t;

typedef struct {
    int32_t in_use;
    int32_t source;
    int32_t cwd_dir;
} initfs_client_t;

static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static initfs_fd_t g_open_files[INITFS_MAX_OPEN_FILES];
static initfs_file_t g_files[INITFS_MAX_FILES];
static initfs_dir_t g_dirs[INITFS_MAX_DIRS];
static initfs_client_t g_clients[INITFS_MAX_CLIENTS];
static int32_t g_file_count = 0;
static int32_t g_dir_count = 0;

static void
console_write(const char *s)
{
    if (s) {
        (void)printf("%s", s);
    }
}

static void
str_copy(char *dst, uint32_t dst_len, const char *src)
{
    uint32_t i = 0;
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
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
copy_path_from_fs_buffer(int32_t path_len, char *out, uint32_t out_len)
{
    if (!out || out_len == 0 || path_len <= 0 || path_len >= (int32_t)out_len) {
        return -1;
    }
    if (wasmos_fs_buffer_copy((int32_t)(uintptr_t)out, path_len, 0) != 0) {
        return -1;
    }
    if (wasmos_sync_user_read((int32_t)(uintptr_t)out, path_len) != 0) {
        return -1;
    }
    out[path_len] = '\0';
    return 0;
}

static int
initfs_normalize_input_path(const char *in, char *out, uint32_t out_len)
{
    uint32_t ri = 0;
    uint32_t wi = 0;
    if (!in || !out || out_len < 2) {
        return -1;
    }
    while (in[ri] == '/') {
        ri++;
    }
    if ((in[ri] == 'i' || in[ri] == 'I') &&
        (in[ri + 1] == 'n' || in[ri + 1] == 'N') &&
        (in[ri + 2] == 'i' || in[ri + 2] == 'I') &&
        (in[ri + 3] == 't' || in[ri + 3] == 'T') &&
        in[ri + 4] == '/') {
        ri += 5;
    }
    while (in[ri] != '\0' && wi + 1 < out_len) {
        out[wi++] = in[ri++];
    }
    out[wi] = '\0';
    return wi > 0 ? 0 : -1;
}

static int
initfs_build_absolute_path(int32_t cwd_dir, const char *input, char *out, uint32_t out_len)
{
    char norm[INITFS_PATH_MAX];
    const char *cwd = g_dirs[cwd_dir].path;
    if (!input || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if (input[0] == '/' ||
        ((input[0] == 'i' || input[0] == 'I') &&
         (input[1] == 'n' || input[1] == 'N') &&
         (input[2] == 'i' || input[2] == 'I') &&
         (input[3] == 't' || input[3] == 'T') &&
         input[4] == '/') ||
        strcasecmp(input, "init") == 0) {
        return initfs_normalize_input_path(input, out, out_len);
    }
    if (initfs_normalize_input_path(input, norm, sizeof(norm)) != 0) {
        return -1;
    }
    if (cwd[0] == '\0') {
        str_copy(out, out_len, norm);
        return 0;
    }
    if ((uint32_t)snprintf(out, out_len, "%s/%s", cwd, norm) >= out_len) {
        return -1;
    }
    return 0;
}

static int32_t
dir_find_by_path(const char *path)
{
    for (int32_t i = 0; i < g_dir_count; ++i) {
        if (g_dirs[i].in_use && strcasecmp(g_dirs[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int32_t
dir_add(const char *path, int32_t parent_index, const char *name)
{
    if (g_dir_count >= INITFS_MAX_DIRS) {
        return -1;
    }
    initfs_dir_t *d = &g_dirs[g_dir_count];
    d->in_use = 1;
    d->parent_index = parent_index;
    str_copy(d->path, sizeof(d->path), path ? path : "");
    str_copy(d->name, sizeof(d->name), name ? name : "");
    g_dir_count++;
    return g_dir_count - 1;
}

static int32_t
dir_ensure_path(const char *full_path, int32_t *out_parent_dir)
{
    char scratch[INITFS_PATH_MAX];
    char prefix[INITFS_PATH_MAX];
    int32_t parent = 0;
    int32_t len = 0;

    if (!full_path || !out_parent_dir) {
        return -1;
    }
    str_copy(scratch, sizeof(scratch), full_path);
    len = (int32_t)strlen(scratch);
    for (int32_t i = 0; i <= len; ++i) {
        if (scratch[i] != '/' && scratch[i] != '\0') {
            continue;
        }
        if (scratch[i] == '\0') {
            *out_parent_dir = parent;
            return 0;
        }
        scratch[i] = '\0';
        str_copy(prefix, sizeof(prefix), scratch);
        int32_t existing = dir_find_by_path(prefix);
        if (existing < 0) {
            const char *base = prefix;
            for (int32_t j = 0; prefix[j] != '\0'; ++j) {
                if (prefix[j] == '/') {
                    base = &prefix[j + 1];
                }
            }
            existing = dir_add(prefix, parent, base);
            if (existing < 0) {
                return -1;
            }
        }
        parent = existing;
        scratch[i] = '/';
    }
    return -1;
}

static int
initfs_build_index(void)
{
    int32_t count = wasmos_initfs_entry_count();
    if (count < 0) {
        return -1;
    }

    g_file_count = 0;
    g_dir_count = 0;
    if (dir_add("", -1, "") < 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; ++i) {
        char raw_path[INITFS_PATH_MAX];
        char norm_path[INITFS_PATH_MAX];
        int32_t raw_len = wasmos_initfs_entry_name(i,
                                                   (int32_t)(uintptr_t)raw_path,
                                                   (int32_t)sizeof(raw_path));
        if (raw_len <= 0 || raw_len >= (int32_t)sizeof(raw_path)) {
            continue;
        }
        if (wasmos_sync_user_read((int32_t)(uintptr_t)raw_path, raw_len + 1) != 0) {
            continue;
        }
        raw_path[raw_len] = '\0';
        if (initfs_normalize_input_path(raw_path, norm_path, sizeof(norm_path)) != 0) {
            continue;
        }
        int32_t size = wasmos_initfs_entry_size(i);
        if (size < 0) {
            continue;
        }
        int32_t parent_dir = 0;
        if (dir_ensure_path(norm_path, &parent_dir) != 0) {
            continue;
        }
        if (g_file_count >= INITFS_MAX_FILES) {
            break;
        }
        initfs_file_t *f = &g_files[g_file_count];
        f->in_use = 1;
        f->entry_index = i;
        f->size = size;
        f->dir_index = parent_dir;
        str_copy(f->path, sizeof(f->path), norm_path);
        const char *base = f->path;
        for (int32_t j = 0; f->path[j] != '\0'; ++j) {
            if (f->path[j] == '/') {
                base = &f->path[j + 1];
            }
        }
        str_copy(f->name, sizeof(f->name), base);
        g_file_count++;
    }
    return 0;
}

static int32_t
initfs_find_file_path_exact(const char *norm_path)
{
    for (int32_t i = 0; i < g_file_count; ++i) {
        if (g_files[i].in_use && strcasecmp(g_files[i].path, norm_path) == 0) {
            return i;
        }
    }
    return -1;
}

static int32_t
initfs_find_file_by_basename_unique(const char *name)
{
    int32_t match = -1;
    for (int32_t i = 0; i < g_file_count; ++i) {
        if (!g_files[i].in_use) {
            continue;
        }
        if (strcasecmp(g_files[i].name, name) != 0) {
            continue;
        }
        if (match >= 0) {
            return -1;
        }
        match = i;
    }
    return match;
}

static int32_t
initfs_find_file_record(int32_t cwd_dir, const char *path)
{
    char full[INITFS_PATH_MAX];
    if (initfs_build_absolute_path(cwd_dir, path, full, sizeof(full)) != 0) {
        return -1;
    }
    int32_t idx = initfs_find_file_path_exact(full);
    if (idx >= 0) {
        return idx;
    }
    if (!strchr(full, '/')) {
        return initfs_find_file_by_basename_unique(full);
    }
    return -1;
}

static int32_t
initfs_fd_alloc(int32_t file_index)
{
    for (int32_t i = 0; i < INITFS_MAX_OPEN_FILES; ++i) {
        if (!g_open_files[i].in_use) {
            g_open_files[i].in_use = 1;
            g_open_files[i].entry_index = file_index;
            g_open_files[i].offset = 0;
            return i + 3;
        }
    }
    return -1;
}

static initfs_fd_t *
initfs_fd_lookup(int32_t fd)
{
    if (fd < 3 || fd >= (3 + INITFS_MAX_OPEN_FILES)) {
        return 0;
    }
    if (!g_open_files[fd - 3].in_use) {
        return 0;
    }
    return &g_open_files[fd - 3];
}

static int
emit_stream_text(int32_t source, int32_t req_id, const char *text)
{
    int32_t len = (int32_t)strlen(text);
    for (int32_t pos = 0; pos < len; ) {
        int32_t a0 = (int32_t)(uint8_t)text[pos++];
        int32_t a1 = (pos < len) ? (int32_t)(uint8_t)text[pos++] : 0;
        int32_t a2 = (pos < len) ? (int32_t)(uint8_t)text[pos++] : 0;
        int32_t a3 = (pos < len) ? (int32_t)(uint8_t)text[pos++] : 0;
        if (wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_STREAM, req_id, a0, a1, a2, a3) != 0) {
            return -1;
        }
    }
    return 0;
}

static int
emit_init_listing(int32_t source, int32_t req_id)
{
    char line[INITFS_PATH_MAX + 4];
    int32_t cwd_dir = 0;
    for (int32_t i = 0; i < INITFS_MAX_CLIENTS; ++i) {
        if (g_clients[i].in_use && g_clients[i].source == source) {
            cwd_dir = g_clients[i].cwd_dir;
            break;
        }
    }
    for (int32_t i = 0; i < g_dir_count; ++i) {
        if (!g_dirs[i].in_use || i == 0 || g_dirs[i].parent_index != cwd_dir) {
            continue;
        }
        snprintf(line, sizeof(line), "%s/\n", g_dirs[i].name);
        if (emit_stream_text(source, req_id, line) != 0) {
            return -1;
        }
    }
    for (int32_t i = 0; i < g_file_count; ++i) {
        if (!g_files[i].in_use || g_files[i].dir_index != cwd_dir) {
            continue;
        }
        snprintf(line, sizeof(line), "%s\n", g_files[i].name);
        if (emit_stream_text(source, req_id, line) != 0) {
            return -1;
        }
    }
    return 0;
}

static int
chdir_to_path(int32_t *cwd_dir, const char *path)
{
    char full[INITFS_PATH_MAX];
    if (!cwd_dir) {
        return -1;
    }
    if (!path || path[0] == '\0' || strcasecmp(path, "/") == 0 || strcasecmp(path, "init") == 0 || strcasecmp(path, "/init") == 0) {
        *cwd_dir = 0;
        return 0;
    }
    if (strcasecmp(path, "..") == 0) {
        if (*cwd_dir > 0) {
            *cwd_dir = g_dirs[*cwd_dir].parent_index >= 0 ? g_dirs[*cwd_dir].parent_index : 0;
        }
        return 0;
    }
    if (initfs_build_absolute_path(*cwd_dir, path, full, sizeof(full)) != 0) {
        return -1;
    }
    int32_t dir_index = dir_find_by_path(full);
    if (dir_index < 0) {
        return -1;
    }
    *cwd_dir = dir_index;
    return 0;
}

static int32_t *
client_cwd_for_source(int32_t source)
{
    int32_t free_slot = -1;
    for (int32_t i = 0; i < INITFS_MAX_CLIENTS; ++i) {
        if (g_clients[i].in_use && g_clients[i].source == source) {
            return &g_clients[i].cwd_dir;
        }
        if (!g_clients[i].in_use && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return 0;
    }
    g_clients[free_slot].in_use = 1;
    g_clients[free_slot].source = source;
    g_clients[free_slot].cwd_dir = 0;
    return &g_clients[free_slot].cwd_dir;
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
    if (initfs_build_index() != 0) {
        console_write("[fs-init] initfs index build failed\n");
        stall_forever();
    }
    for (int32_t i = 0; i < INITFS_MAX_CLIENTS; ++i) {
        g_clients[i].in_use = 0;
        g_clients[i].source = -1;
        g_clients[i].cwd_dir = 0;
    }
    if (wasmos_svc_register(proc_endpoint, g_fs_endpoint, "initfs.rules", 1) != 0) {
        console_write("[fs-init] register initfs.rules failed\n");
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
        int32_t *cwd_dir = client_cwd_for_source(source);
        int32_t status = -1;
        if (!cwd_dir) {
            (void)wasmos_ipc_send(source, g_fs_endpoint, FS_IPC_ERROR, req_id, -1, 0, 0, 0);
            continue;
        }

        if (type == FS_IPC_OPEN_REQ) {
            char path[INITFS_PATH_MAX];
            if (arg1 == 0 && arg2 == 0 && arg3 == 0 && copy_path_from_fs_buffer(arg0, path, sizeof(path)) == 0) {
            } else {
                unpack_name((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3, path, sizeof(path));
            }
            int32_t file_index = initfs_find_file_record(*cwd_dir, path);
            if (file_index >= 0) {
                status = initfs_fd_alloc(file_index);
            }
        } else if (type == FS_IPC_READ_REQ) {
            initfs_fd_t *fd = initfs_fd_lookup(arg0);
            int32_t req_len = arg1;
            if (fd && req_len >= 0) {
                initfs_file_t *file = &g_files[fd->entry_index];
                int32_t fs_buf_size = wasmos_fs_buffer_size();
                uint8_t tmp[512];
                if (fd->offset >= file->size) {
                    status = 0;
                } else {
                    int32_t remaining = file->size - fd->offset;
                    if (req_len > fs_buf_size) {
                        req_len = fs_buf_size;
                    }
                    if (req_len > (int32_t)sizeof(tmp)) {
                        req_len = (int32_t)sizeof(tmp);
                    }
                    if (req_len > remaining) {
                        req_len = remaining;
                    }
                    int32_t copied = wasmos_initfs_entry_copy(file->entry_index,
                                                              (int32_t)(uintptr_t)tmp,
                                                              req_len,
                                                              fd->offset);
                    if (copied >= 0) {
                        if (copied == 0 || wasmos_fs_buffer_write((int32_t)(uintptr_t)tmp, copied, 0) == 0) {
                            fd->offset += copied;
                            status = copied;
                        }
                    }
                }
            }
        } else if (type == FS_IPC_CLOSE_REQ) {
            initfs_fd_t *fd = initfs_fd_lookup(arg0);
            if (fd) {
                fd->in_use = 0;
                fd->entry_index = -1;
                fd->offset = 0;
                status = 0;
            }
        } else if (type == FS_IPC_READDIR_REQ) {
            status = emit_init_listing(source, req_id);
        } else if (type == FS_IPC_CHDIR_REQ) {
            char name[INITFS_PATH_MAX];
            unpack_name((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3, name, sizeof(name));
            status = chdir_to_path(cwd_dir, name);
        } else if (type == FS_IPC_READY_REQ) {
            status = 0;
        }

        (void)wasmos_ipc_send(source,
                              g_fs_endpoint,
                              status >= 0 ? FS_IPC_RESP : FS_IPC_ERROR,
                              req_id,
                              status,
                              0,
                              0,
                              0);
    }
}
