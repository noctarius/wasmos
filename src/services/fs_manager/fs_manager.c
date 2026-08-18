/* fs_manager.c - VFS multiplexer service ("fs.vfs"): routes file operations to
 * the FS backend drivers (fs-fat, fs-init) by mount-name prefix, with
 * per-context client state tracked in a custom bump+chunk heap.  Backends are
 * found by subscribing to the FSMGR_BACKEND_CLASS service class and pulling each
 * provider's FSMGR_IPC_BACKEND_INFO, so the set is rebuilt on (re)start. */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"
#include "fs_manager_types.h"
#include "fs_manager_path.h"

/* Retries a reply gets before this service gives up on a client whose endpoint
 * stays full. Generous on purpose: the failure it guards against is permanent
 * (a stranded client hangs forever) while the condition it waits out is
 * transient (a client mid-output-loop drains within a few scheduling rounds). */
#define FSMGR_REPLY_SEND_RETRIES 8192

#define FSMGR_PATH_SCRATCH_SIZE 256

static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static fs_backend_t g_backends[FS_BACKEND_CAP];
extern uint8_t __heap_base;

typedef struct fs_client_chunk {
    struct fs_client_chunk* next;
    uint32_t used;
    fs_client_state_t slots[FS_CLIENT_CHUNK_CAP];
} fs_client_chunk_t;

static fs_client_chunk_t* g_client_chunks = 0;
static uint32_t g_heap_cursor = 0;
static uint32_t g_heap_limit = 0;

/* Initialise the custom bump heap starting at &__heap_base. */
static void fsmgr_heap_init(void) {
    g_heap_cursor = addr_cast(uint32_t, &__heap_base);
    g_heap_limit = (uint32_t)__builtin_wasm_memory_size(0) * 65536u;
    if (g_heap_cursor > g_heap_limit) {
        g_heap_cursor = g_heap_limit;
    }
}

/* Bump-allocate size bytes aligned to align (must be power-of-two);
 * grows WASM memory pages on demand.  Memory is never freed. */
static void* fsmgr_heap_alloc(uint32_t size, uint32_t align) {
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
    return ptr_cast(void, aligned);
}

static fs_client_chunk_t* client_chunk_alloc(void) {
    fs_client_chunk_t* chunk =
        (fs_client_chunk_t*)fsmgr_heap_alloc((uint32_t)sizeof(fs_client_chunk_t), 8u);
    if (!chunk) {
        return 0;
    }
    memset(chunk, 0, sizeof(*chunk));
    return chunk;
}

/* Whether an FS IPC request type carries client buffer data (path or payload)
 * that the backend must borrow. Such requests carry the client's buffer_id in
 * arg2; fs-manager forwards it and the client endpoint so the backend can borrow
 * the client's object directly. */
static int type_uses_client_buffer(int32_t type) {
    return type == FS_IPC_OPEN_REQ || type == FS_IPC_STAT_REQ || type == FS_IPC_UNLINK_REQ ||
           type == FS_IPC_MKDIR_REQ || type == FS_IPC_RMDIR_REQ || type == FS_IPC_READ_REQ ||
           type == FS_IPC_WRITE_REQ || type == FS_IPC_READ_APP_REQ;
}

static void log_msg(const char* s) {
    if (!s)
        return;
    (void)printf("%s", s);
}

static void set_mount_name(fs_backend_t* slot, const char* base) {
    char buf[16];
    uint8_t tmp[3];
    uint32_t n = 0;
    uint32_t pos = 0;
    if (!slot)
        return;
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

static fs_client_state_t* client_state_lookup(int32_t context_id) {
    fs_client_chunk_t* chunk = g_client_chunks;
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

static void client_state_reset_fds(fs_client_state_t* state) {
    if (!state) {
        return;
    }
    memset(state->fds, 0, sizeof(state->fds));
}

/* Find or create per-context state for context_id; allocates a new chunk if
 * the current one is full.  Returns NULL only if heap is exhausted. */
static fs_client_state_t* client_state(int32_t context_id) {
    fs_client_state_t* state = client_state_lookup(context_id);
    fs_client_chunk_t* chunk = g_client_chunks;
    fs_client_chunk_t* last = 0;
    if (state) {
        return state;
    }
    while (chunk) {
        if (chunk->used < FS_CLIENT_CHUNK_CAP) {
            fs_client_state_t* slot = &chunk->slots[chunk->used++];
            slot->in_use = 1;
            slot->context_id = context_id;
            slot->mount = FS_MOUNT_ROOT;
            slot->backend_endpoint = -1;
            slot->mount_depth = 0;
            client_state_reset_fds(slot);
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
    client_state_reset_fds(&chunk->slots[0]);
    return &chunk->slots[0];
}

static int fsmgr_is_fd_op_type(int32_t type) {
    return type == FS_IPC_READ_REQ || type == FS_IPC_WRITE_REQ || type == FS_IPC_CLOSE_REQ ||
           type == FS_IPC_SEEK_REQ;
}

static fsmgr_client_fd_t* fsmgr_fd_entry(fs_client_state_t* state, int32_t client_fd) {
    int32_t index = client_fd - 3;

    if (!state || index < 0 || index >= FSMGR_CLIENT_FD_CAP) {
        return 0;
    }
    if (!state->fds[index].in_use) {
        return 0;
    }
    return &state->fds[index];
}

static int fsmgr_fd_alloc(fs_client_state_t* state, int32_t backend_endpoint, int32_t backend_fd,
                          int32_t* out_client_fd) {
    if (!state || !out_client_fd || backend_endpoint < 0 || backend_fd < 0) {
        return -1;
    }
    for (int32_t i = 0; i < FSMGR_CLIENT_FD_CAP; ++i) {
        if (state->fds[i].in_use) {
            continue;
        }
        state->fds[i].in_use = 1;
        state->fds[i].backend_endpoint = backend_endpoint;
        state->fds[i].backend_fd = backend_fd;
        *out_client_fd = i + 3;
        return 0;
    }
    return -1;
}

static void fsmgr_fd_release(fs_client_state_t* state, int32_t client_fd) {
    fsmgr_client_fd_t* entry = fsmgr_fd_entry(state, client_fd);

    if (!entry) {
        return;
    }
    entry->in_use = 0;
    entry->backend_endpoint = -1;
    entry->backend_fd = -1;
}

static fs_backend_t* backend_find_by_name(const char* name) {
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

static fs_backend_t* backend_first_of_kind(uint8_t kind) {
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && g_backends[i].kind == kind) {
            return &g_backends[i];
        }
    }
    return 0;
}

/* Register or update a backend at endpoint; assigns a slot-based default mount
 * name ("boot"/"user" for BOOT kind; "init"/"init1" for INIT; "fs"/"fs1" for
 * others).  fsmgr_apply_backend_info overwrites that default with the mount
 * name the backend reports, when it reports one.  Idempotent for an endpoint
 * that is already registered.  Returns NULL when all FS_BACKEND_CAP slots are
 * taken. */
static fs_backend_t* backend_register(uint8_t kind, int32_t endpoint) {
    fs_backend_t* slot = 0;
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
            str_copy(slot->mount_name, sizeof(slot->mount_name), "boot");
        } else if (slot->slot == 1) {
            str_copy(slot->mount_name, sizeof(slot->mount_name), "user");
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

/* Query devmgr.query for DEVMGR_MOUNT_INFO and populate PCI metadata fields
 * (bus, device_fn, class, vendor, etc.) on the given BOOT backend slot.
 * The IPC response arg3 bit 31 must be set for the info to be valid.
 * Currently unreferenced: the only place that wanted it is the class-discovery
 * path, which cannot make this synchronous round trip without deadlocking (see
 * fsmgr_apply_backend_info). The metadata it fills is diagnostic only, so `mount`
 * prints no PCI identity until the TODO there is resolved. */
static void backend_refresh_boot_meta(fs_backend_t* slot, int32_t req_seed) {
    int32_t devmgr = -1;
    int32_t req_id = req_seed;
    if (!slot || slot->kind != FSMGR_BACKEND_BOOT || g_proc_endpoint < 0 || g_reply_endpoint < 0) {
        return;
    }
    devmgr = wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "devmgr.query", 1);
    if (devmgr < 0) {
        return;
    }
    int32_t send_rc =
        wasmos_ipc_send(devmgr, g_reply_endpoint, DEVMGR_QUERY_MOUNT_REQ, req_id, 0, 0, 0, 0);
    int32_t sel_rc = (send_rc == 0) ? wasmos_ipc_select_one(g_reply_endpoint) : -1;
    int32_t last_req = (sel_rc >= 0) ? wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID) : -1;
    int32_t last_type = (sel_rc >= 0) ? wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) : -1;
    if (send_rc != 0 || sel_rc < 0 || last_req != req_id || last_type != DEVMGR_MOUNT_INFO) {
        printf("[fs-manager] boot-meta fail devmgr=%d send=%d sel=%d req=%d type=%d src=%d dst=%d "
               "a0=%d a1=%d a2=%d a3=%d\n",
               devmgr,
               send_rc,
               sel_rc,
               last_req,
               last_type,
               wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
               wasmos_ipc_last_field(WASMOS_IPC_FIELD_DESTINATION),
               wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0),
               wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1),
               wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2),
               wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3));
        return;
    }

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

/* Reply to a client, treating a full client queue as backpressure rather than
 * failure: retry with a yield between tries.
 *
 * Every reply this service sends is one a client is blocked waiting for, with no
 * timeout behind it. Dropping one because the client's 32-slot endpoint is
 * momentarily full -- which a READDIR makes likely, since the stream frames just
 * filled it and the terminating reply follows immediately -- hangs that client
 * forever, and behind it everything waiting on that client. The STREAM relay in
 * forward_request has retried for exactly this reason; the terminators it hands
 * back had not.
 *
 * Returns 0 on success, or the send's status (IPC_ERR_FULL once the retries are
 * spent, which means the client is stranded and there is nothing further this
 * service can do about it). */
static int32_t reply_to_client(int32_t source, int32_t type, int32_t request_id, int32_t a0,
                               int32_t a1, int32_t a2, int32_t a3) {
    return wasmos_sys_ipc_send_retry(
        source, g_fs_endpoint, type, request_id, a0, a1, a2, a3, FSMGR_REPLY_SEND_RETRIES);
}

static int send_virtual_root_listing(int32_t source, int32_t req_id) {
    char root_listing[256] = {0};
    uint32_t pos = 0;
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
        if (pos < len)
            a1 = (int32_t)(uint8_t)root_listing[pos++];
        if (pos < len)
            a2 = (int32_t)(uint8_t)root_listing[pos++];
        if (pos < len)
            a3 = (int32_t)(uint8_t)root_listing[pos++];
        if (reply_to_client(source, FS_IPC_STREAM, req_id, a0, a1, a2, a3) != 0) {
            return -1;
        }
    }
    return reply_to_client(source, FS_IPC_RESP, req_id, 0, 0, 0, 0);
}

/* Returns WASMOS_ERR_NONE, or the packed reason the listing could not be sent. */
static wasmos_error_code_t fsmgr_emit_mounts(int32_t source, int32_t req_id, int32_t buffer_id) {
    char mounts[384];
    uint32_t pos = 0;
    str_copy(mounts, sizeof(mounts), "mounts:\n");
    pos = (uint32_t)strlen(mounts);
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        const char* kind = "fs";
        int n = 0;
        if (!g_backends[i].in_use) {
            continue;
        }
        if (g_backends[i].kind == FSMGR_BACKEND_BOOT) {
            kind = "fs-fat";
        } else if (g_backends[i].kind == FSMGR_BACKEND_INIT) {
            kind = "fs-init";
        }
        n = snprintf(
            mounts + pos, sizeof(mounts) - pos, "/%s -> %s", g_backends[i].mount_name, kind);
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
    if (buffer_id <= 0) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (wasmos_sys_buffer_write(buffer_id, mounts, (int32_t)pos, 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    if (reply_to_client(source, FSMGR_IPC_QUERY_MOUNTS_RESP, req_id, (int32_t)pos, 0, 0, 0) != 0) {
        return WASMOS_ERR_FS_REPLY_SEND;
    }
    return WASMOS_ERR_NONE;
}

/* Forward an IPC request to a backend, relaying FS_IPC_STREAM chunks back to
 * source immediately as they arrive, then returning the final reply fields.
 * Blocks until a non-STREAM response with the matching request_id is received. */
static int forward_request(int32_t backend_endpoint, int32_t type, int32_t req_id, int32_t arg0,
                           int32_t arg1, int32_t arg2, int32_t arg3, int32_t source,
                           int32_t* out_resp_type, int32_t* out_r0, int32_t* out_r1,
                           int32_t* out_r2, int32_t* out_r3) {
    if (backend_endpoint < 0)
        return -1;
    if (wasmos_ipc_send(backend_endpoint, g_reply_endpoint, type, req_id, arg0, arg1, arg2, arg3) !=
        0) {
        return -1;
    }
    for (;;) {
        wasmos_ipc_message_t reply;
        /* Anything arriving here that is not this reply is consumed, so it must
         * be reported: if it was a reply someone else awaits, that side blocks
         * forever and the loss is otherwise invisible.  fs-manager is a relay
         * with one request in flight, so a non-matching id means a stale reply
         * to a request already abandoned -- worth seeing, never expected. */
        if (wasmos_sys_ipc_await_reply(
                g_reply_endpoint, req_id, &reply, 0, 0, "fs-manager/forward", 0) != 0) {
            return -1;
        }
        int32_t resp_type = (int32_t)reply.type;
        int32_t rr0 = reply.arg0;
        int32_t rr1 = reply.arg1;
        int32_t rr2 = reply.arg2;
        int32_t rr3 = reply.arg3;
        if (resp_type == FS_IPC_STREAM) {
            /* Retry with yield: the client may be in a slow output loop (e.g.
             * VT-write retries) and temporarily unable to drain its endpoint.
             * A bare non-retrying send would fill the 32-slot queue, abort the
             * relay, drop the error notification (send_fs_error also fails on a
             * full queue), and leave the client blocked in select_one forever. */
            if (wasmos_sys_ipc_send_retry(source,
                                          g_fs_endpoint,
                                          resp_type,
                                          req_id,
                                          rr0,
                                          rr1,
                                          rr2,
                                          rr3,
                                          FSMGR_REPLY_SEND_RETRIES) != 0) {
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

/* `reason` is a packed WASMOS_ERR_FS_* code, or a code relayed unchanged from a
 * backend reply. */
static void send_fs_error(int32_t source, int32_t request_id, wasmos_error_code_t reason) {
    /* Retry with yield for the same reason as forward_request's STREAM relay:
     * dropping the error notification because the client's endpoint is
     * transiently full hangs that client forever in select_one. */
    (void)wasmos_sys_ipc_send_retry(
        source, g_fs_endpoint, FS_IPC_ERROR, request_id, reason, 0, 0, 0, FSMGR_REPLY_SEND_RETRIES);
}

static int32_t resolve_backend_for_state(const fs_client_state_t* state) {
    int32_t backend = state ? state->backend_endpoint : -1;
    if (backend < 0) {
        fs_backend_t* fallback_boot = backend_first_of_kind(FSMGR_BACKEND_BOOT);
        backend = fallback_boot ? fallback_boot->endpoint : -1;
    }
    return backend;
}

static int is_path_op_type(int32_t type) {
    return type == FS_IPC_OPEN_REQ || type == FS_IPC_STAT_REQ || type == FS_IPC_UNLINK_REQ ||
           type == FS_IPC_MKDIR_REQ || type == FS_IPC_RMDIR_REQ;
}

static int32_t route_path_to_backend(const uint8_t* path_bytes, int32_t path_len,
                                     int32_t allow_relative, char* out_path, int32_t out_path_cap,
                                     int32_t* out_path_len, int32_t* out_backend) {
    const char* mount_names[FS_BACKEND_CAP];
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
    routed = fsmgr_route_path_for_mounts((const char*)path_bytes,
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

/* Read the path from the source endpoint's xfer buffer, strip the mount prefix,
 * write the tail path back into the local xfer buffer, and set *out_backend.
 * *inout_arg0 is updated to the tail path length.
 * Returns 1 on successful routing, 0 if path is at VFS root, -1 on error. */
static int route_root_path_request(fs_client_state_t* state, int32_t buffer_id, int32_t type,
                                   int32_t* inout_arg0, int32_t* out_backend) {
    int32_t path_len = inout_arg0 ? *inout_arg0 : 0;
    int32_t fs_buf_size = wasmos_xfer_buffer_size();
    uint8_t scratch[256];
    int32_t routed_backend = out_backend ? *out_backend : -1;
    int32_t open_path_len = path_len;

    if (!state || !inout_arg0 || !out_backend || !is_path_op_type(type) || buffer_id <= 0) {
        return 0;
    }
    if (path_len <= 0 || path_len >= fs_buf_size || path_len >= (int32_t)sizeof(scratch) - 1) {
        return -1;
    }
    /* fs-manager was granted R|W over the client object (client borrow -> arg3);
     * read the path and write the mount-stripped tail back in place for the
     * backend to re-read. No borrow is taken here. */
    if (wasmos_xfer_buffer_read(buffer_id, scratch, path_len, 0) != 0) {
        return -1;
    }
    scratch[path_len] = '\0';
    if (scratch[0] == '/' && scratch[1] != '\0') {
        (void)route_path_to_backend(scratch,
                                    path_len,
                                    0,
                                    (char*)scratch,
                                    (int32_t)sizeof(scratch),
                                    &open_path_len,
                                    &routed_backend);
    }
    if (open_path_len <= 0 || wasmos_xfer_buffer_write(buffer_id, scratch, open_path_len, 0) != 0) {
        return -1;
    }
    *inout_arg0 = open_path_len;
    *out_backend = routed_backend;
    return 1;
}

/* Apply a backend's pulled info: register it at its endpoint, set unit, and
 * read its mount name (arg2 packs (buffer_id<<12)|len; the backend borrowed the
 * buffer READ to fs-manager). Shared by the initial class enumeration and ADD
 * events. */
static void fsmgr_apply_backend_info(int32_t backend_endpoint, int32_t kind, int32_t arg2f,
                                     int32_t unit) {
    fs_backend_t* registered = backend_register((uint8_t)kind, backend_endpoint);
    int32_t mount_len = arg2f & 0xFFF;
    int32_t buffer_id = (int32_t)((uint32_t)arg2f >> 12);
    if (!registered) {
        return;
    }
    registered->unit = (uint8_t)(unit & 0xFF);
    if (buffer_id > 0 && mount_len > 0 && mount_len < (int32_t)sizeof(registered->mount_name)) {
        char mount_name[16];
        int32_t copy_len = mount_len;
        if (copy_len >= (int32_t)sizeof(mount_name)) {
            copy_len = (int32_t)sizeof(mount_name) - 1;
        }
        if (wasmos_sys_buffer_read(buffer_id, mount_name, copy_len, 0) == 0) {
            mount_name[copy_len] = '\0';
            if (mount_name[0] == '/') {
                str_copy(registered->mount_name, sizeof(registered->mount_name), &mount_name[1]);
            } else {
                str_copy(registered->mount_name, sizeof(registered->mount_name), mount_name);
            }
            wasmos_sys_to_lower_ascii(registered->mount_name);
        }
    }
    /* NOTE: do NOT call backend_refresh_boot_meta() here. This runs while
     * handling a class-discovery event, and that helper does a SYNCHRONOUS
     * DEVMGR_QUERY_MOUNT_REQ round-trip to device-manager — which at this point
     * is itself blocked waiting for fs-manager to answer its /boot rules read,
     * producing a mutual-wait deadlock. The boot meta is diagnostic PCI
     * identity, not required to mount.
     * TODO(fs-class-discovery): refetch boot meta out of band (device-manager
     * push, or a fs-manager idle step) rather than a nested synchronous call. */
}

/* Pull a discovered backend's info over FSMGR_IPC_BACKEND_INFO and register it.
 * Synchronous round-trip on the reply endpoint; the backend answers without any
 * dependency on fs-manager, so this cannot deadlock. */
static void fsmgr_pull_backend(int32_t backend_endpoint) {
    if (backend_endpoint < 0) {
        return;
    }
    if (wasmos_ipc_send(
            backend_endpoint, g_reply_endpoint, FSMGR_IPC_BACKEND_INFO_REQ, 1, 0, 0, 0, 0) != 0) {
        return;
    }
    if (wasmos_ipc_select_one(g_reply_endpoint) < 0 ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != FSMGR_IPC_BACKEND_INFO_RESP) {
        return;
    }
    fsmgr_apply_backend_info(backend_endpoint,
                             wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0),
                             wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2),
                             wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3));
}

/* Drop a backend that left its class (provider died / unregistered). */
static void fsmgr_backend_remove(int32_t backend_endpoint) {
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && g_backends[i].endpoint == backend_endpoint) {
            g_backends[i].in_use = 0;
        }
    }
}

/* Discover the current FS backends by class and pull each. Subscribe first so a
 * backend registering between here and the lookup still fires an event; the
 * lookup then captures the current set (and rebuilds it after an fs-manager
 * restart). backend_register is idempotent, so an overlap is harmless. */
static void fsmgr_discover_backends(void) {
    svc_class_entry_t backends[8];
    int32_t n;
    int32_t i;
    (void)wasmos_svc_subscribe_class(
        g_proc_endpoint, g_reply_endpoint, g_fs_endpoint, FSMGR_BACKEND_CLASS, 3);
    n = wasmos_svc_lookup_class(g_proc_endpoint,
                                g_reply_endpoint,
                                FSMGR_BACKEND_CLASS,
                                backends,
                                (int32_t)(sizeof(backends) / sizeof(backends[0])),
                                4);
    for (i = 0; i < n && i < (int32_t)(sizeof(backends) / sizeof(backends[0])); ++i) {
        fsmgr_pull_backend((int32_t)backends[i].endpoint);
    }
}

static int handle_clone_cwd_req(int32_t source, int32_t source_owner, int32_t request_id,
                                int32_t arg0, int32_t arg1f) {
    fs_client_state_t* src_state = 0;
    fs_client_state_t* dst_state = 0;
    int32_t src_context_id = arg0;
    int32_t dst_context_id = arg1f;
    int32_t proc_owner = wasmos_ipc_endpoint_owner(g_proc_endpoint);
    /* Cloning another context's cwd is process-manager only. */
    if (source_owner < 0 || proc_owner < 0 || source_owner != proc_owner) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_AUTHORIZED);
        return 1;
    }
    if (src_context_id <= 0 || dst_context_id <= 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BAD_ARGS);
        return 1;
    }
    src_state = client_state(src_context_id);
    dst_state = client_state(dst_context_id);
    if (!src_state || !dst_state) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NO_CLIENT_SLOT);
        return 1;
    }
    dst_state->mount = src_state->mount;
    dst_state->backend_endpoint = src_state->backend_endpoint;
    dst_state->mount_depth = src_state->mount_depth;
    (void)reply_to_client(source, FSMGR_IPC_CLONE_CWD_RESP, request_id, 0, 0, 0, 0);
    return 1;
}

/* READ_PATH: open+read+close the file named by the path in the client's buffer,
 * with the backend writing the blob straight back into that buffer.
 *
 * Owner-push: the client owns buffer_id and granted fs-manager R|W (client_borrow
 * = arg3). fs-manager reads/routes the path by buffer_id (its grant), reborrows
 * to the backend so the backend can read the path and write the blob directly,
 * then unborrows client_borrow (cascade-revoking the reborrow) before replying so
 * the client's release() succeeds. `capacity` bounds the read to the client
 * buffer size (0 = full transfer size). No relay copy. */
static int handle_read_path_req(fs_client_state_t* state, int32_t source, int32_t request_id,
                                int32_t path_len, int32_t capacity, int32_t buffer_id,
                                int32_t client_borrow) {
    int32_t backend = state ? state->backend_endpoint : -1;
    int32_t open_t = FS_IPC_ERROR, open0 = -1, open1 = 0, open2 = 0, open3 = 0;
    int32_t read_t = FS_IPC_ERROR, read0 = -1, read1 = 0, read2 = 0, read3 = 0;
    int32_t close_t = FS_IPC_ERROR, close0 = -1, close1 = 0, close2 = 0, close3 = 0;
    int32_t fd = -1;
    int32_t fs_buf_size = wasmos_xfer_buffer_size();
    int32_t read_cap = (capacity > 0 && capacity < fs_buf_size) ? capacity : fs_buf_size;
    uint8_t path_scratch[FSMGR_PATH_SCRATCH_SIZE];
    int32_t open_path_len = 0;
    int32_t backend_borrow = -1;

    if (buffer_id <= 0 || path_len <= 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BAD_ARGS);
        return 1;
    }
    if (path_len >= fs_buf_size || path_len >= (int32_t)sizeof(path_scratch) - 1) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_PATH_TOO_LONG);
        return 1;
    }
    /* client_borrow (b1) is the client's grant to fs-manager; fs-manager is its
     * BORROWER and never unborrows it (the client releases it). fs-manager reads
     * the path via its grant, reborrows to the backend (b2, which fs-manager
     * lends and therefore unborrows), and unborrows b2 before replying. */
    if (wasmos_xfer_buffer_read(buffer_id, path_scratch, path_len, 0) != 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BUFFER);
        return 1;
    }
    path_scratch[path_len] = '\0';
    if (path_scratch[0] != '/') {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_ABSOLUTE);
        return 1;
    }
    open_path_len = path_len;
    (void)route_path_to_backend(path_scratch,
                                path_len,
                                0,
                                (char*)path_scratch,
                                (int32_t)sizeof(path_scratch),
                                &open_path_len,
                                &backend);
    if (backend < 0) {
        fs_backend_t* fallback_boot = backend_first_of_kind(FSMGR_BACKEND_BOOT);
        backend = fallback_boot ? fallback_boot->endpoint : -1;
    }
    if (backend < 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NO_BACKEND);
        return 1;
    }
    if (open_path_len <= 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_TRANSLATE);
        return 1;
    }
    if (wasmos_xfer_buffer_write(buffer_id, path_scratch, open_path_len, 0) != 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BUFFER);
        return 1;
    }
    /* Reborrow to the backend (R|W: reads the path, writes the blob). */
    backend_borrow = wasmos_xfer_buffer_reborrow(
        backend, client_borrow, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (backend_borrow < 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_REBORROW);
        return 1;
    }

    int32_t open_rc = forward_request(backend,
                                      FS_IPC_OPEN_REQ,
                                      request_id,
                                      open_path_len,
                                      0,
                                      buffer_id,
                                      backend_borrow,
                                      source,
                                      &open_t,
                                      &open0,
                                      &open1,
                                      &open2,
                                      &open3);
    if (open_rc != 0 || open_t != FS_IPC_RESP || open0 < 0) {
        /* Relay the backend's own reason when it sent one — it names the failure
         * better than anything this relay could substitute. */
        wasmos_error_code_t open_err =
            (open_rc == 0 && open0 < 0) ? open0 : WASMOS_ERR_FS_BACKEND_IPC;
        (void)wasmos_xfer_buffer_unborrow(backend_borrow);
        send_fs_error(source, request_id, open_err);
        return 1;
    }

    fd = open0;
    int32_t read_rc = forward_request(backend,
                                      FS_IPC_READ_REQ,
                                      request_id,
                                      fd,
                                      read_cap,
                                      buffer_id,
                                      backend_borrow,
                                      source,
                                      &read_t,
                                      &read0,
                                      &read1,
                                      &read2,
                                      &read3);
    wasmos_error_code_t read_err = WASMOS_ERR_NONE;
    if (read_rc != 0) {
        read_err = WASMOS_ERR_FS_BACKEND_IPC;
    } else if (read_t != FS_IPC_RESP || read0 < 0) {
        read_err = (read0 < 0) ? read0 : WASMOS_ERR_FS_BACKEND_IPC;
    } else if (read0 == 0 || read0 > read_cap) {
        /* Backend reported a length outside the requested capacity. */
        read_err = WASMOS_ERR_FS_RANGE;
    }
    if (read_err != WASMOS_ERR_NONE) {
        (void)forward_request(backend,
                              FS_IPC_CLOSE_REQ,
                              request_id,
                              fd,
                              0,
                              0,
                              0,
                              source,
                              &close_t,
                              &close0,
                              &close1,
                              &close2,
                              &close3);
        (void)wasmos_xfer_buffer_unborrow(backend_borrow);
        send_fs_error(source, request_id, read_err);
        return 1;
    }
    (void)forward_request(backend,
                          FS_IPC_CLOSE_REQ,
                          request_id,
                          fd,
                          0,
                          0,
                          0,
                          source,
                          &close_t,
                          &close0,
                          &close1,
                          &close2,
                          &close3);
    /* Drop fs-manager's reborrow (cascade-safe) before replying; the client's
     * grant b1 stays until the client releases the buffer. */
    (void)wasmos_xfer_buffer_unborrow(backend_borrow);
    (void)reply_to_client(source, FS_IPC_RESP, request_id, read0, 0, 0, 0);
    return 1;
}

static int handle_chdir_mount(fs_client_state_t* state, int32_t source, int32_t request_id,
                              int32_t arg0, int32_t arg1f, int32_t arg2f, int32_t arg3f) {
    char path[32];
    wasmos_sys_ipc_unpack_name16(
        (uint32_t)arg0, (uint32_t)arg1f, (uint32_t)arg2f, (uint32_t)arg3f, path, sizeof(path));

    if (strcasecmp(path, "") == 0 || strcasecmp(path, "/") == 0) {
        state->mount = FS_MOUNT_ROOT;
        state->backend_endpoint = -1;
        state->mount_depth = 0;
        (void)reply_to_client(source, FS_IPC_RESP, request_id, 0, 0, 0, 0);
        return 1;
    }
    if (strcasecmp(path, "..") == 0 && state->mount == FS_MOUNT_ROOT) {
        (void)reply_to_client(source, FS_IPC_RESP, request_id, 0, 0, 0, 0);
        return 1;
    }
    if (strcasecmp(path, "..") == 0 && state->mount != FS_MOUNT_ROOT && state->mount_depth == 0) {
        state->mount = FS_MOUNT_ROOT;
        state->backend_endpoint = -1;
        (void)reply_to_client(source, FS_IPC_RESP, request_id, 0, 0, 0, 0);
        return 1;
    }

    const char* mount_name = path;
    if (path[0] == '/') {
        mount_name = &path[1];
    }
    fs_backend_t* target = backend_find_by_name(mount_name);
    if (target) {
        int32_t s0, s1, s2, s3;
        int32_t rr_t, rr0, rr1, rr2, rr3;
        state->mount = FS_MOUNT_BACKEND;
        state->backend_endpoint = target->endpoint;
        state->mount_depth = 0;

        int32_t args[4];
        wasmos_sys_ipc_pack_name16("/", args);
        s0 = args[0];
        s1 = args[1];
        s2 = args[2];
        s3 = args[3];

        if (forward_request(target->endpoint,
                            FS_IPC_CHDIR_REQ,
                            request_id,
                            s0,
                            s1,
                            s2,
                            s3,
                            source,
                            &rr_t,
                            &rr0,
                            &rr1,
                            &rr2,
                            &rr3) != 0) {
            state->mount = FS_MOUNT_ROOT;
            state->backend_endpoint = -1;
            send_fs_error(source, request_id, WASMOS_ERR_FS_BACKEND_IPC);
            return 1;
        }
        (void)reply_to_client(source, rr_t, request_id, rr0, rr1, rr2, rr3);
        return 1;
    }

    return 0;
}

/* Service entry point.  Brings up the bump heap and the two endpoints (a service
 * endpoint published as "fs.vfs" and a private reply endpoint used for nested
 * calls to backends), registers the service name, signals readiness, then
 * discovers the FS backends and serves requests forever.
 *
 * Lifecycle ordering is load-bearing: the name must be registered before
 * notify_ready, or a client woken by the ready signal can look up "fs.vfs" and
 * miss.  Backend discovery runs AFTER notify_ready, because it is a class
 * subscription plus a pull from each provider — the providers are peers that may
 * still be starting, and blocking readiness on them would deadlock the boot
 * sequence.  The two discovery mechanisms differ: `wasmos_svc_register` /
 * name lookup binds one well-known name to one endpoint, whereas subscribing to
 * FSMGR_BACKEND_CLASS yields a set that changes over time and delivers
 * SVC_IPC_CLASS_EVENT adds and removes into the main loop.
 *
 * Every request is answered on the service endpoint, and the loop parks in
 * wasmos_ipc_select_one rather than spinning.  Client identity is the owning
 * context of the sending endpoint, not the endpoint itself, so all of a
 * process's endpoints share one cwd.
 *
 * Does not return: a failure to create an endpoint or to register the name
 * parks the process in wasmos_sys_ipc_recv_loop instead of exiting, so the
 * declared int32_t result is never produced.  arg1..arg3 are ignored and
 * proc_endpoint is overwritten from the spawn-info contract. */
WASMOS_WASM_EXPORT int32_t initialize(void) {
    /* The proc endpoint comes from the spawn-info contract; the entry args carry
     * nothing and the parameter is overwritten. */
    int32_t proc_endpoint = wasmos_startup_proc_endpoint();

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
    fsmgr_discover_backends();

    for (;;) {
        if (wasmos_ipc_select_one(g_fs_endpoint) < 0) {
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

        if (type == SVC_IPC_CLASS_EVENT) {
            /* Existence event for FSMGR_BACKEND_CLASS: arg0=event, arg1=instance,
             * arg2=provider endpoint, arg3=pid. */
            if (arg0 == (int32_t)SVC_CLASS_EVENT_ADD) {
                fsmgr_pull_backend(arg2f);
            } else if (arg0 == (int32_t)SVC_CLASS_EVENT_REMOVE) {
                fsmgr_backend_remove(arg2f);
            }
            continue;
        }

        if (type == FSMGR_IPC_CLONE_CWD_REQ) {
            (void)handle_clone_cwd_req(source, source_owner, request_id, arg0, arg1f);
            continue;
        }
        if (type == FSMGR_IPC_QUERY_MOUNTS_REQ) {
            /* Client owns the listing buffer (arg2) and granted fs-manager W;
             * fs-manager writes it and replies. fs-manager is the grant's
             * BORROWER, so it does not unborrow — the client releases (cascade). */
            wasmos_error_code_t mounts_err = fsmgr_emit_mounts(source, request_id, arg2f);
            if (mounts_err != WASMOS_ERR_NONE) {
                send_fs_error(source, request_id, mounts_err);
            }
            continue;
        }

        if (client_key < 0) {
            client_key = source;
        }

        fs_client_state_t* state = client_state(client_key);
        if (!state) {
            send_fs_error(source, request_id, WASMOS_ERR_FS_NO_CLIENT_SLOT);
            continue;
        }

        if (type == FS_IPC_READ_PATH_REQ) {
            (void)handle_read_path_req(state, source, request_id, arg0, arg1f, arg2f, arg3f);
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
        int32_t client_fd = -1;
        /* Buffer-carrying ops arrive as arg2 = client buffer_id, arg3 = the
         * client's grant to fs-manager (client_borrow; fs-manager is its
         * BORROWER, so it never unborrows it — the client tears it down on
         * release). fs-manager reborrows that grant to the backend, forwards the
         * reborrow handle in arg3, and — being the reborrow's lender — unborrows
         * that reborrow once the backend replies. Non-buffer ops (e.g. CHDIR,
         * which packs name bytes into arg2/arg3) keep their args and take none. */
        int32_t uses_buf = type_uses_client_buffer(type);
        int32_t client_borrow = uses_buf ? arg3f : 0;
        int32_t backend_borrow = -1;
        int32_t fwd_arg3 = arg3f;
        if (is_path_op_type(type)) {
            if (route_root_path_request(state, arg2f, type, &req_arg0, &backend) < 0) {
                send_fs_error(source, request_id, WASMOS_ERR_FS_TRANSLATE);
                continue;
            }
        }
        if (fsmgr_is_fd_op_type(type)) {
            fsmgr_client_fd_t* fd_entry = fsmgr_fd_entry(state, arg0);
            if (!fd_entry) {
                send_fs_error(source, request_id, WASMOS_ERR_FS_BAD_FD);
                continue;
            }
            client_fd = arg0;
            req_arg0 = fd_entry->backend_fd;
            backend = fd_entry->backend_endpoint;
        }
        if (uses_buf) {
            backend_borrow = wasmos_xfer_buffer_reborrow(
                backend, client_borrow, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
            if (backend_borrow < 0) {
                send_fs_error(source, request_id, WASMOS_ERR_FS_REBORROW);
                continue;
            }
            fwd_arg3 = backend_borrow;
        }
        if (forward_request(backend,
                            type,
                            request_id,
                            req_arg0,
                            arg1f,
                            arg2f,
                            fwd_arg3,
                            source,
                            &resp_type,
                            &r0,
                            &r1,
                            &r2,
                            &r3) != 0) {
            if (backend_borrow >= 0) {
                (void)wasmos_xfer_buffer_unborrow(backend_borrow);
            }
            if (type == FS_IPC_CHDIR_REQ && state->mount != FS_MOUNT_ROOT) {
                char path[32];
                wasmos_sys_ipc_unpack_name16((uint32_t)arg0,
                                             (uint32_t)arg1f,
                                             (uint32_t)arg2f,
                                             (uint32_t)arg3f,
                                             path,
                                             sizeof(path));
                if (strcasecmp(path, "..") == 0) {
                    state->mount = FS_MOUNT_ROOT;
                    state->backend_endpoint = -1;
                    (void)reply_to_client(source, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                    continue;
                }
            }
            send_fs_error(source, request_id, WASMOS_ERR_FS_BACKEND_IPC);
            continue;
        }
        if (type == FS_IPC_OPEN_REQ && resp_type == FS_IPC_RESP && r0 >= 0) {
            int32_t backend_fd = r0;
            if (fsmgr_fd_alloc(state, backend, backend_fd, &r0) != 0) {
                int32_t close_t = FS_IPC_ERROR;
                int32_t close0 = -1, close1 = 0, close2 = 0, close3 = 0;
                (void)forward_request(backend,
                                      FS_IPC_CLOSE_REQ,
                                      request_id,
                                      backend_fd,
                                      0,
                                      0,
                                      0,
                                      source,
                                      &close_t,
                                      &close0,
                                      &close1,
                                      &close2,
                                      &close3);
                if (backend_borrow >= 0) {
                    (void)wasmos_xfer_buffer_unborrow(backend_borrow);
                }
                send_fs_error(source, request_id, WASMOS_ERR_FS_NO_FD);
                continue;
            }
        }
        if (type == FS_IPC_CLOSE_REQ && resp_type == FS_IPC_RESP && r0 == 0 && client_fd >= 0) {
            fsmgr_fd_release(state, client_fd);
        }
        if (backend_borrow >= 0) {
            (void)wasmos_xfer_buffer_unborrow(backend_borrow);
        }
        if (type == FS_IPC_CHDIR_REQ && resp_type == FS_IPC_ERROR) {
            char path[32];
            wasmos_sys_ipc_unpack_name16((uint32_t)arg0,
                                         (uint32_t)arg1f,
                                         (uint32_t)arg2f,
                                         (uint32_t)arg3f,
                                         path,
                                         sizeof(path));
            if (strcasecmp(path, "..") == 0 && state->mount != FS_MOUNT_ROOT) {
                state->mount = FS_MOUNT_ROOT;
                state->backend_endpoint = -1;
                state->mount_depth = 0;
                (void)reply_to_client(source, FS_IPC_RESP, request_id, 0, 0, 0, 0);
                continue;
            }
        }
        if (type == FS_IPC_CHDIR_REQ && resp_type == FS_IPC_RESP && state->mount != FS_MOUNT_ROOT) {
            char path[32];
            wasmos_sys_ipc_unpack_name16((uint32_t)arg0,
                                         (uint32_t)arg1f,
                                         (uint32_t)arg2f,
                                         (uint32_t)arg3f,
                                         path,
                                         sizeof(path));
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
        (void)reply_to_client(source, resp_type, request_id, r0, r1, r2, r3);
    }
}
