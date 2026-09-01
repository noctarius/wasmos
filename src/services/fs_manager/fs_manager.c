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
#include "fs_manager_backends.h"

/* Retries a reply gets before this service gives up on a client whose endpoint
 * stays full. Generous on purpose: the failure it guards against is permanent
 * (a stranded client hangs forever) while the condition it waits out is
 * transient (a client mid-output-loop drains within a few scheduling rounds). */
#define FSMGR_REPLY_SEND_RETRIES 8192

#define FSMGR_PATH_SCRATCH_SIZE 256

/* fs-manager's own path buffer, used to tell a backend which directory a
 * path-less request (READDIR) refers to. The client's buffer cannot serve: a
 * READDIR carries none. Acquired once at startup and never released. */
static int32_t g_cwd_bid = -1;

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
           type == FS_IPC_WRITE_REQ || type == FS_IPC_READ_APP_REQ || type == FS_IPC_RENAME_REQ;
}

static void log_msg(const char* s) {
    if (!s)
        return;
    (void)printf("%s", s);
}

/* Reset a freshly claimed slot to the VFS root. A client with no working
 * directory would otherwise have every relative name routed by guesswork; the
 * root is the one directory that needs no backend to name. */
static void client_state_init_cwd(fs_client_state_t* slot) {
    slot->cwd[0] = '/';
    slot->cwd[1] = '\0';
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
            client_state_init_cwd(slot);
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
    client_state_init_cwd(&chunk->slots[0]);
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

/* Register or update a backend at endpoint.  The caller sets the mount name from
 * what the backend reported; none is assigned here, because nothing on this side
 * knows where a backend belongs and a default could only be a guess.  `slot` is
 * the per-kind ordinal, kept for diagnostics.  Idempotent for an endpoint that is
 * already registered.  Returns NULL when all FS_BACKEND_CAP slots are taken. */
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
    if (!slot || slot->kind != FSMGR_BACKEND_BLOCK || g_proc_endpoint < 0 || g_reply_endpoint < 0) {
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
        kind = fsmgr_backend_fs_name(&g_backends[i]);
        n = snprintf(
            mounts + pos, sizeof(mounts) - pos, "%s -> %s", g_backends[i].mount_path, kind);
        if (n > 0 && (uint32_t)n < sizeof(mounts) - pos &&
            g_backends[i].kind == FSMGR_BACKEND_BLOCK && g_backends[i].has_meta) {
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

/* Defined below, next to the rest of the routing; declared here because the
 * backend a client's directory resolves to is answered by routing it. */
static int32_t route_absolute_path(const char* path, char* out_path, int32_t out_cap,
                                   int32_t* out_path_len, int32_t* out_backend);

/* The backend serving the client's working directory.
 *
 * `state->backend_endpoint` is a CACHE, filled in on chdir, and it is not the
 * authority: a client that has never chdir'd stands at "/" with the cache unset,
 * and every client starts that way. So an unset cache is answered by routing the
 * working directory, which is where the answer comes from in the first place.
 *
 * Regression: 2026-08-31-readdir-without-chdir. Trusting the cache made `ls` fail
 * with BACKEND_IPC as the first command of a session -- forward_request refuses a
 * negative endpoint -- and it was invisible while "/" was not a mount, because
 * READDIR at the root was short-circuited into the virtual mount-table listing
 * and never reached the forward path.
 *
 * There is deliberately no FALLBACK here. This used to answer -1 with "the boot
 * backend", which turned "this client is not in any mount" into a plausible
 * reply: a relative name typed in /wfs was handed to the FAT driver, which
 * answered NOT_FOUND, and the driver that actually held the file was never asked.
 * Routing the cwd is not that: it asks the same question chdir asked, of the same
 * mount table, and answers -1 when nothing owns it.
 *
 * Returns the backend, or -1 when no mount owns the client's directory -- which
 * on a system with a root filesystem means only that the directory is gone. */
static int32_t resolve_backend_for_state(const fs_client_state_t* state) {
    char tail[FSMGR_CWD_MAX];
    int32_t tail_len = 0;
    int32_t backend = -1;

    if (!state) {
        return -1;
    }
    if (state->backend_endpoint >= 0) {
        return state->backend_endpoint;
    }
    if (!route_absolute_path(state->cwd, tail, (int32_t)sizeof(tail), &tail_len, &backend)) {
        return -1;
    }
    return backend;
}

static int is_path_op_type(int32_t type) {
    return type == FS_IPC_OPEN_REQ || type == FS_IPC_STAT_REQ || type == FS_IPC_UNLINK_REQ ||
           type == FS_IPC_MKDIR_REQ || type == FS_IPC_RMDIR_REQ;
}

static int32_t route_path_to_backend(const uint8_t* path_bytes, int32_t path_len, char* out_path,
                                     int32_t out_path_cap, int32_t* out_path_len,
                                     int32_t* out_backend) {
    const char* mount_paths[FS_BACKEND_CAP];
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
        mount_paths[mount_count] = g_backends[i].mount_path;
        mount_endpoints[mount_count] = g_backends[i].endpoint;
        mount_count++;
    }
    if (mount_count <= 0) {
        return 0;
    }
    routed = fsmgr_route_path_for_mounts((const char*)path_bytes,
                                         path_len,
                                         mount_paths,
                                         mount_count,
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

/* Route an absolute VFS path to the backend that serves it, and report the path
 * that backend should see, with the owning mount stripped. The caller ends up with
 * a backend and a path that backend can resolve on its own, which is what keeps a
 * client's working directory out of the backends.
 *
 * The owner is the LONGEST mount path prefixing the request on a whole-segment
 * boundary, so `/mnt/usb` is its own filesystem inside `/mnt` and `/wfs` does not
 * own `/wfsx`. `/` prefixes every path, so once a root filesystem is mounted
 * every absolute path routes somewhere and the root is the owner of last resort.
 *
 * With NO root filesystem mounted, a path matching no other mount is not served
 * and the caller reports WASMOS_ERR_FS_NOT_FOUND. That is not a fallback rule:
 * the root is an ordinary mount that happens to prefix everything, whereas the
 * fallback this replaced sent unmatched paths to the BOOT volume, which made
 * `/system/utils/ip` a second name for `/boot/system/utils/ip` that appeared in
 * no listing. A second fallback of the same shape, keyed on the client rather
 * than the path, had already hidden broken working-directory inheritance (see
 * resolve_backend_for_state).
 *
 * Returns 1 with *out_backend and out_path set, 0 when no backend can serve it. */
static int32_t route_absolute_path(const char* path, char* out_path, int32_t out_cap,
                                   int32_t* out_path_len, int32_t* out_backend) {
    int32_t path_len = (int32_t)strlen(path);

    if (path_len <= 0 || path[0] != '/' || !out_path || out_cap < 2 || !out_path_len ||
        !out_backend) {
        return 0;
    }
    if (route_path_to_backend(
            (const uint8_t*)path, path_len, out_path, out_cap, out_path_len, out_backend) &&
        *out_backend >= 0) {
        return 1;
    }
    return 0;
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
    char abs_path[FSMGR_CWD_MAX + 256];
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
    /* Every name is resolved against the client's working directory before it is
     * routed, so a relative name reaches the backend that actually holds that
     * directory. Forwarding it unresolved is what let a name typed in one mount
     * be answered by another. */
    if (!fsmgr_cwd_join(state->cwd, (const char*)scratch, abs_path, (int32_t)sizeof(abs_path))) {
        return -1;
    }
    /* The root itself names no file; no backend can serve it. */
    if (abs_path[1] == '\0') {
        return -1;
    }
    if (!route_absolute_path(
            abs_path, (char*)scratch, (int32_t)sizeof(scratch), &open_path_len, &routed_backend)) {
        return -1;
    }
    if (open_path_len <= 0 || wasmos_xfer_buffer_write(buffer_id, scratch, open_path_len, 0) != 0) {
        return -1;
    }
    *inout_arg0 = open_path_len;
    *out_backend = routed_backend;
    return 1;
}

/* RENAME carries two paths in one buffer (source at 0, destination at
 * arg0 + 1), so it needs its own routing: each has its mount prefix stripped,
 * and stripping the FIRST moves the second, which is why the buffer is rewritten
 * as a pair rather than in place one at a time.
 *
 * Both paths must resolve to the SAME backend. A cross-mount rename would mean
 * copying the data between filesystems, which no backend can express and this
 * layer will not fake; it is refused so a caller sees a failure rather than a
 * half-move. Returns 1 when routed, 0 when not applicable, -1 on refusal.  */
static int route_rename_request(fs_client_state_t* state, int32_t buffer_id, int32_t* inout_arg0,
                                int32_t* inout_arg1, int32_t* out_backend) {
    uint8_t old_buf[256];
    uint8_t new_buf[256];
    char old_abs[FSMGR_CWD_MAX + 256];
    char new_abs[FSMGR_CWD_MAX + 256];
    int32_t old_len = inout_arg0 ? *inout_arg0 : 0;
    int32_t new_len = inout_arg1 ? *inout_arg1 : 0;
    int32_t fs_buf_size = wasmos_xfer_buffer_size();
    int32_t old_backend = out_backend ? *out_backend : -1;
    int32_t new_backend = old_backend;
    int32_t out_old = old_len;
    int32_t out_new = new_len;

    if (!state || !inout_arg0 || !inout_arg1 || !out_backend || buffer_id <= 0) {
        return 0;
    }
    if (old_len <= 0 || new_len <= 0 || old_len >= (int32_t)sizeof(old_buf) - 1 ||
        new_len >= (int32_t)sizeof(new_buf) - 1 || old_len + new_len + 2 > fs_buf_size) {
        return -1;
    }
    if (wasmos_xfer_buffer_read(buffer_id, old_buf, old_len, 0) != 0 ||
        wasmos_xfer_buffer_read(buffer_id, new_buf, new_len, old_len + 1) != 0) {
        return -1;
    }
    old_buf[old_len] = '\0';
    new_buf[new_len] = '\0';

    /* Both sides resolve against the client's working directory, then route the
     * same way every other path does -- including onto the root filesystem when
     * neither names a mount. */
    if (!fsmgr_cwd_join(state->cwd, (const char*)old_buf, old_abs, (int32_t)sizeof(old_abs)) ||
        !fsmgr_cwd_join(state->cwd, (const char*)new_buf, new_abs, (int32_t)sizeof(new_abs))) {
        return -1;
    }
    if (old_abs[1] != '\0') {
        (void)route_absolute_path(
            old_abs, (char*)old_buf, (int32_t)sizeof(old_buf), &out_old, &old_backend);
    }
    if (new_abs[1] != '\0') {
        (void)route_absolute_path(
            new_abs, (char*)new_buf, (int32_t)sizeof(new_buf), &out_new, &new_backend);
    }
    if (out_old <= 0 || out_new <= 0) {
        return -1;
    }
    if (old_backend != new_backend) {
        return -1; /* cross-mount: refused, see above */
    }
    /* Rewrite the pair so the destination follows the STRIPPED source. */
    if (wasmos_xfer_buffer_write(buffer_id, old_buf, out_old + 1, 0) != 0 ||
        wasmos_xfer_buffer_write(buffer_id, new_buf, out_new + 1, out_old + 1) != 0) {
        return -1;
    }
    *inout_arg0 = out_old;
    *inout_arg1 = out_new;
    *out_backend = old_backend;
    return 1;
}

/* Register a backend from its pulled info. `reported_mount` is what the backend
 * reported, already read out of the caller-owned buffer; normalizing it into the
 * absolute mount PATH fs-manager holds belongs to fsmgr_mount_path_from_reported,
 * which also decides which reports name no mount at all.
 *
 * `fs_type` is the backend's FS_TYPE_*; `kind` distinguishes block-backed from
 * pseudo and cannot stand in for it. A backend that reports FS_TYPE_UNKNOWN is
 * registered as such rather than assumed.
 *
 * A backend whose name yields no mount is not registered, and in particular
 * takes no slot: a backend seated under an empty name is reachable by no path,
 * because routing compares a whole first segment and no segment is empty.
 *
 * Shared by the initial class enumeration and ADD events. */
static void fsmgr_apply_backend_info(int32_t backend_endpoint, int32_t kind, int32_t fs_type,
                                     const char* reported_mount, int32_t unit) {
    fs_backend_t* registered;
    /* Sized off the field it ends up in, so the two cannot drift. */
    char mount[sizeof(((fs_backend_t*)0)->mount_path)];

    if (!fsmgr_mount_path_from_reported(reported_mount, mount, (uint32_t)sizeof(mount))) {
        printf("[fs-manager] backend reported no usable mount path; not registered\n");
        return;
    }
    registered = backend_register((uint8_t)kind, backend_endpoint);
    if (!registered) {
        /* The table is full, so this filesystem is simply not in the namespace.
         * Said out loud because the alternative is a mount that was requested,
         * started, and is serving nobody, with no path leading to it. */
        printf(
            "[fs-manager] backend table full (%d); %s not mounted\n", (int)FS_BACKEND_CAP, mount);
        return;
    }
    registered->unit = (uint8_t)(unit & 0xFF);
    registered->fs_type = (uint32_t)fs_type;
    str_copy(registered->mount_path, sizeof(registered->mount_path), mount);
    /* NOTE: do NOT call backend_refresh_boot_meta() here. This runs while
     * handling a class-discovery event, and that helper does a SYNCHRONOUS
     * DEVMGR_QUERY_MOUNT_REQ round-trip to device-manager — which at this point
     * is itself blocked waiting for fs-manager to answer its /boot rules read,
     * producing a mutual-wait deadlock. The boot meta is diagnostic PCI
     * identity, not required to mount.
     * TODO(fs-class-discovery): refetch boot meta out of band (device-manager
     * push, or a fs-manager idle step) rather than a nested synchronous call. */
}

/* Create one directory in `backend`, under the path that backend sees.
 *
 * fs-manager owns the buffer the path travels in (g_cwd_bid), because a
 * registration supplies none -- the same reason backend_sync_cwd owns one. An
 * EXISTS reply is success: this runs on every registration and a mount point that
 * is already there is the expected case, not a conflict.
 *
 * Returns 0 when the directory exists afterwards, whether this call made it. */
static int fsmgr_backend_mkdir(int32_t backend, const char* path, int32_t request_id) {
    int32_t rr_t = FS_IPC_ERROR, rr0 = -1, rr1 = 0, rr2 = 0, rr3 = 0;
    int32_t len;
    int32_t borrow;
    int32_t rc;

    if (backend < 0 || !path || path[0] != '/' || path[1] == '\0' || g_cwd_bid < 0) {
        return -1;
    }
    len = (int32_t)strlen(path);
    if (len >= wasmos_xfer_buffer_size()) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(g_cwd_bid, (const uint8_t*)path, len, 0) != 0) {
        return -1;
    }
    borrow = wasmos_xfer_buffer_borrow(backend, g_cwd_bid, WASMOS_BUFFER_GRANT_READ);
    if (borrow < 0) {
        return -1;
    }
    rc = forward_request(backend,
                         FS_IPC_MKDIR_REQ,
                         request_id,
                         len,
                         0,
                         g_cwd_bid,
                         borrow,
                         /* No client is waiting on this, so there is nobody to
                          * relay a STREAM frame to. MKDIR never sends one; if it
                          * ever did, the relay's send fails and the call reports
                          * failure rather than answering a stranger. */
                         -1,
                         &rr_t,
                         &rr0,
                         &rr1,
                         &rr2,
                         &rr3);
    (void)wasmos_xfer_buffer_unborrow(borrow);
    if (rc != 0) {
        return -1;
    }
    if (rr_t == FS_IPC_RESP && rr0 == 0) {
        return 0;
    }
    return rr0 == WASMOS_ERR_FS_EXISTS ? 0 : -1;
}

/* Make every registered mount PATH exist as a directory in the filesystem that
 * covers it, so a mount point is an ordinary entry in an ordinary listing.
 *
 * This is what retires the invented root listing: `ls /` forwards to the root
 * filesystem and finds `boot`, `user` and `wfs` there because they were created,
 * rather than because fs-manager appended the mount table to the reply.
 *
 * Each ancestor of a mount path is created in turn, shallowest first, so a mount
 * at `/mnt/usb` gets `/mnt` too -- routing an ancestor finds whichever mount
 * covers it, which is the root for a top-level mount and the parent filesystem
 * for a nested one.
 *
 * Runs after EVERY registration and is idempotent, because the order backends
 * register in is not ours to choose: a volume can mount before the root
 * filesystem does, and its mount point has to appear once the root arrives.
 * Failures are ignored -- a read-only backend refuses MKDIR, and a mount under
 * one is simply not visible in its listing, which is a cosmetic loss and not a
 * routing one. */
static void fsmgr_ensure_mount_points(void) {
    uint32_t i;
    int32_t request_id = 8;

    for (i = 0; i < FS_BACKEND_CAP; ++i) {
        const char* mount;
        int32_t seg_end;

        if (!g_backends[i].in_use) {
            continue;
        }
        mount = g_backends[i].mount_path;
        if (mount[0] != '/' || mount[1] == '\0') {
            continue; /* the root itself is nobody's mount point */
        }
        /* Walk the ancestors: "/mnt", then "/mnt/usb". */
        for (seg_end = 1; mount[seg_end - 1] != '\0';) {
            char ancestor[FSMGR_MOUNT_PATH_MAX];
            char parent[FSMGR_MOUNT_PATH_MAX];
            char tail[FSMGR_MOUNT_PATH_MAX];
            char target[FSMGR_MOUNT_PATH_MAX + FSMGR_MOUNT_PATH_MAX];
            int32_t tail_len = 0;
            int32_t backend = -1;
            int32_t cut;

            while (mount[seg_end] != '/' && mount[seg_end] != '\0') {
                seg_end++;
            }
            if (seg_end >= (int32_t)sizeof(ancestor)) {
                break;
            }
            memcpy(ancestor, mount, (uint32_t)seg_end);
            ancestor[seg_end] = '\0';

            /* The covering mount is found by routing the ancestor's PARENT: the
             * ancestor may itself be a mount, and routing it would then answer
             * with that mount rather than with what contains it. */
            cut = seg_end;
            while (cut > 1 && ancestor[cut - 1] != '/') {
                cut--;
            }
            while (cut > 1 && ancestor[cut - 1] == '/') {
                cut--;
            }
            memcpy(parent, ancestor, (uint32_t)cut);
            parent[cut] = '\0';
            if (cut == 0) {
                parent[0] = '/';
                parent[1] = '\0';
            }
            if (route_absolute_path(parent, tail, (int32_t)sizeof(tail), &tail_len, &backend)) {
                const char* leaf = &ancestor[cut];
                while (*leaf == '/') {
                    leaf++;
                }
                if (tail_len == 1 && tail[0] == '/') {
                    (void)snprintf(target, sizeof(target), "/%s", leaf);
                } else {
                    (void)snprintf(target, sizeof(target), "%s/%s", tail, leaf);
                }
                (void)fsmgr_backend_mkdir(backend, target, request_id++);
            }
            if (mount[seg_end] == '\0') {
                break;
            }
            seg_end++; /* step over the separator onto the next segment */
        }
    }
}

/* Pull a discovered backend's info over FSMGR_IPC_BACKEND_INFO and register it.
 * Synchronous round-trip on the reply endpoint; the backend answers without any
 * dependency on fs-manager, so this cannot deadlock.
 *
 * fs-manager is the CLIENT of this exchange and therefore owns the buffer the
 * backend writes its mount name into: acquired here, borrowed to the backend
 * WRITE, released on every path including the failures. `release`
 * cascade-revokes the grant, so there is nothing else to clean up
 * (docs/architecture/12-dma-transfers.md). A backend lending its own buffer
 * instead is refused ALREADY_BORROWED on the second pull, and each backend is
 * pulled more than once -- the class lookup and the ADD event both land here.
 *
 * A backend that names no mount is not registered: nothing else knows where it
 * belongs, so there is no default to fall back to. */
static void fsmgr_pull_backend(int32_t backend_endpoint) {
    char reported_mount[FSMGR_MOUNT_PATH_MAX];
    int32_t bid;
    int32_t name_len;
    if (backend_endpoint < 0) {
        return;
    }
    bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(reported_mount));
    if (bid < 0) {
        return;
    }
    if (wasmos_xfer_buffer_borrow(backend_endpoint, bid, WASMOS_BUFFER_GRANT_WRITE) < 0 ||
        wasmos_ipc_send(
            backend_endpoint, g_reply_endpoint, FSMGR_IPC_BACKEND_INFO_REQ, 1, bid, 0, 0, 0) != 0 ||
        wasmos_ipc_select_one(g_reply_endpoint) < 0 ||
        wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE) != FSMGR_IPC_BACKEND_INFO_RESP) {
        (void)wasmos_xfer_buffer_release(bid);
        return;
    }
    name_len = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
    if (name_len <= 0 || name_len >= (int32_t)sizeof(reported_mount) ||
        wasmos_sys_buffer_read(bid, reported_mount, name_len, 0) != 0) {
        printf("[fs-manager] backend named no mount; not registered\n");
        (void)wasmos_xfer_buffer_release(bid);
        return;
    }
    reported_mount[name_len] = '\0';
    fsmgr_apply_backend_info(backend_endpoint,
                             wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0),
                             wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1),
                             reported_mount,
                             wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3));
    (void)wasmos_xfer_buffer_release(bid);
    /* The table changed, so the mount points may be incomplete: this backend's
     * own point did not exist before now, and an earlier backend's point could
     * not be created until the filesystem covering it registered. */
    fsmgr_ensure_mount_points();
}

/* Forget every cached reference to `endpoint` in the client table.
 *
 * fs_client_state_t caches the backend serving its working directory, and an fd
 * records the backend that issued it. Both outlive the backend if nothing clears
 * them, and a stale endpoint is worse than an absent one: forwarding to it either
 * reaches whatever reused the id or fails with a transport error that says
 * nothing about the mount having gone. Clearing the cache is enough for the cwd,
 * because an unset cache is answered by routing; an fd cannot be re-derived, so
 * it is released and the client's next use of it is a clean BAD_FD.
 *
 * Applies to a provider that DIED as much as to one that was unmounted -- the
 * table entry goes either way, and the caches have to follow it. */
static void fsmgr_forget_backend_in_clients(int32_t endpoint) {
    fs_client_chunk_t* chunk = g_client_chunks;

    while (chunk) {
        for (uint32_t i = 0; i < chunk->used; ++i) {
            fs_client_state_t* state = &chunk->slots[i];
            if (!state->in_use) {
                continue;
            }
            if (state->backend_endpoint == endpoint) {
                state->mount = FS_MOUNT_ROOT;
                state->backend_endpoint = -1;
            }
            for (uint32_t f = 0; f < FSMGR_CLIENT_FD_CAP; ++f) {
                if (state->fds[f].in_use && state->fds[f].backend_endpoint == endpoint) {
                    state->fds[f].in_use = 0;
                    state->fds[f].backend_endpoint = -1;
                    state->fds[f].backend_fd = -1;
                }
            }
        }
        chunk = chunk->next;
    }
}

/* Why `mount_path` cannot be unmounted yet, or WASMOS_ERR_NONE when it can.
 *
 * Two things count as standing in a mount, and both are the namespace saying
 * someone is still there rather than a shortage to retry:
 *
 *  - a DEEPER mount inside it. Removing the outer one first would leave the
 *    inner one addressable only through a path whose prefix no longer routes.
 *    This is also what normally makes "/" unremovable, since every other mount
 *    is inside it.
 *  - an OPEN FILE on it. The fd names a backend that would stop existing, and
 *    the client cannot re-derive it.
 *
 * A client whose WORKING DIRECTORY is under the mount is deliberately NOT
 * counted, though Linux refuses on exactly that. fs-manager never releases a
 * client's state -- there is no exit notification to release it on -- so a cwd
 * recorded by a process that has since exited would refuse the unmount forever.
 * A single `cat` run inside a mount would make it permanently unremovable, which
 * is a worse failure than the one the rule prevents: a client standing in a
 * removed mount gets NOT_FOUND on its next operation and recovers by moving,
 * whereas nothing recovers a mount pinned by a dead process.
 *
 * TODO: count the working directory once client state is reaped on process
 * exit. The same staleness applies to the open-file rule above, which is kept
 * because an fd is a resource fs-manager actually holds and because a client
 * that exits normally closes it. See docs/TASKS.md.
 */
static wasmos_error_code_t fsmgr_mount_busy_reason(const fs_backend_t* mount) {
    fs_client_chunk_t* chunk = g_client_chunks;

    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (!g_backends[i].in_use || &g_backends[i] == mount) {
            continue;
        }
        if (fsmgr_path_is_within(mount->mount_path, g_backends[i].mount_path)) {
            return WASMOS_ERR_FS_MOUNT_BUSY;
        }
    }
    while (chunk) {
        for (uint32_t i = 0; i < chunk->used; ++i) {
            fs_client_state_t* state = &chunk->slots[i];
            if (!state->in_use) {
                continue;
            }
            for (uint32_t f = 0; f < FSMGR_CLIENT_FD_CAP; ++f) {
                if (state->fds[f].in_use && state->fds[f].backend_endpoint == mount->endpoint) {
                    return WASMOS_ERR_FS_MOUNT_BUSY;
                }
            }
        }
        chunk = chunk->next;
    }
    return WASMOS_ERR_NONE;
}

/* Drop a backend that left its class (provider died / unregistered). */
static void fsmgr_backend_remove(int32_t backend_endpoint) {
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && g_backends[i].endpoint == backend_endpoint) {
            g_backends[i].in_use = 0;
        }
    }
    fsmgr_forget_backend_in_clients(backend_endpoint);
}

/* Discover the current FS backends by class and pull each. Subscribe first so a
 * backend registering between here and the lookup still fires an event; the
 * lookup then captures the current set (and rebuilds it after an fs-manager
 * restart). backend_register is idempotent, so an overlap is harmless. */
/* Enumerate FSMGR_BACKEND_CLASS and pull every provider's identity.
 *
 * Separate from the subscription: this runs again after a mount request spawns a
 * driver, so the mount is in the table before the request is answered rather than
 * whenever the class event happens to be dispatched. Pulling a provider already
 * registered is harmless -- backend_register keys on the endpoint and updates the
 * entry in place -- which is what makes calling this twice for the same backend
 * (once here, once from the class event) correct rather than duplicating it. */
static void fsmgr_pull_all_backends(void) {
    svc_class_entry_t backends[8];
    int32_t n;
    int32_t i;
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

static void fsmgr_discover_backends(void) {
    (void)wasmos_svc_subscribe_class(
        g_proc_endpoint, g_reply_endpoint, g_fs_endpoint, FSMGR_BACKEND_CLASS, 3);
    fsmgr_pull_all_backends();
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
    /* The working directory is the full VFS path, so that is what is cloned. A
     * child that inherited only (mount, depth) landed at the mount root, which
     * is not where its spawner stood. */
    str_copy(dst_state->cwd, sizeof(dst_state->cwd), src_state->cwd);
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
    char abs_path[FSMGR_CWD_MAX + FSMGR_PATH_SCRATCH_SIZE];
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
    /* Resolved against the client's working directory, same as every other path
     * op: READ_PATH is a fused open+read+close, not a different namespace. */
    if (!fsmgr_cwd_join(state ? state->cwd : "/",
                        (const char*)path_scratch,
                        abs_path,
                        (int32_t)sizeof(abs_path))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_PATH_TOO_LONG);
        return 1;
    }
    if (!route_absolute_path(abs_path,
                             (char*)path_scratch,
                             (int32_t)sizeof(path_scratch),
                             &open_path_len,
                             &backend)) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_FOUND);
        return 1;
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

/* Count segments below the mount root in a canonical absolute VFS path, so
 * "/wfs" is 0 and "/wfs/docs" is 1. */
/* Tell `backend` which directory subsequent path-less operations refer to.
 *
 * READDIR carries no path, so the backend has to hold a current directory of its
 * own, and fs-manager is the only party that knows whose directory it should be:
 * backends see every client through fs-manager's single reply endpoint and
 * cannot tell two clients apart. Re-asserting the directory before a path-less
 * operation is what keeps one client's chdir from deciding another client's
 * listing.
 *
 * `mount_tail` is the directory relative to the mount root ("/" at the mount
 * root, "/docs/notes" below it) and travels as a path in fs-manager's own
 * transfer buffer, so its depth and its component lengths are bounded only by
 * that buffer -- not by what fits in the request arguments. Returns 0 once the
 * backend stands in that directory. */
static int backend_sync_cwd(int32_t backend, int32_t request_id, const char* mount_tail,
                            int32_t reply_to) {
    int32_t rr_t = FS_IPC_ERROR, rr0 = -1, rr1 = 0, rr2 = 0, rr3 = 0;
    int32_t tail_len;
    int32_t borrow;
    int32_t rc;

    if (backend < 0 || !mount_tail || mount_tail[0] != '/' || g_cwd_bid < 0) {
        return -1;
    }
    tail_len = (int32_t)strlen(mount_tail);
    if (tail_len >= wasmos_xfer_buffer_size()) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(g_cwd_bid, (const uint8_t*)mount_tail, tail_len, 0) != 0) {
        return -1;
    }
    borrow = wasmos_xfer_buffer_borrow(backend, g_cwd_bid, WASMOS_BUFFER_GRANT_READ);
    if (borrow < 0) {
        return -1;
    }
    rc = forward_request(backend,
                         FS_IPC_CHDIR_REQ,
                         request_id,
                         tail_len,
                         0,
                         g_cwd_bid,
                         borrow,
                         reply_to,
                         &rr_t,
                         &rr0,
                         &rr1,
                         &rr2,
                         &rr3);
    (void)wasmos_xfer_buffer_unborrow(borrow);
    if (rc != 0 || rr_t != FS_IPC_RESP || rr0 != 0) {
        return -1;
    }
    return 0;
}

/* Write the client's resulting working directory back into the buffer it
 * supplied and return its length, or 0 when it could not be written.
 *
 * fs-manager canonicalizes the path, so it is the only party that knows what the
 * directory ended up being: a client that derived its own answer would drift
 * from this one on "..", on redundant slashes, and on a refused chdir. Reporting
 * it back leaves exactly one authority. Requires the client to have granted
 * WRITE; a client that granted only READ gets 0 and keeps its own idea.
 *
 * Reported in arg1, not arg0: arg0 of an FS_IPC_RESP is the operation status and
 * a client reads any non-zero value there as a failure. */
static int32_t chdir_report_cwd(const fs_client_state_t* state, int32_t buffer_id) {
    int32_t len = (int32_t)strlen(state->cwd);

    if (buffer_id <= 0 || len <= 0 || len >= wasmos_xfer_buffer_size()) {
        return 0;
    }
    if (wasmos_xfer_buffer_write(buffer_id, (const uint8_t*)state->cwd, len, 0) != 0) {
        return 0;
    }
    return len;
}

/* CHDIR: move the client's working directory.
 *
 * The working directory is a full VFS path and fs-manager owns it, so the
 * incoming name is joined onto the current cwd, canonicalized, and then routed
 * to a mount. Nothing is committed until the backend confirms the directory
 * exists, so a failed chdir leaves the client exactly where it was.
 *
 * `/` is an ordinary mount now, so it needs no case of its own: it routes to the
 * root filesystem like any other path. The one exception is a system with NO root
 * filesystem mounted, where `cd /` still has to work -- a client must be able to
 * stand somewhere it can name other mounts from -- so the client is placed at the
 * root with no backend and a listing there finds nothing.
 *
 * Always answers the client; the return value is 1 so the caller stops. */
static int handle_chdir_mount(fs_client_state_t* state, int32_t source, int32_t request_id,
                              int32_t path_len, int32_t buffer_id) {
    uint8_t requested[FSMGR_CWD_MAX];
    char target[FSMGR_CWD_MAX];
    char tail[FSMGR_CWD_MAX];
    int32_t tail_len = 0;
    int32_t backend = -1;
    int32_t synced;

    if (buffer_id <= 0 || path_len < 0 || path_len >= (int32_t)sizeof(requested) ||
        path_len >= wasmos_xfer_buffer_size()) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_PATH_TOO_LONG);
        return 1;
    }
    /* A zero-length target is how a client says "the VFS root"; anything else is
     * a path to be resolved against where the client currently stands. */
    if (path_len == 0) {
        requested[0] = '/';
        requested[1] = '\0';
    } else {
        if (wasmos_xfer_buffer_read(buffer_id, requested, path_len, 0) != 0) {
            send_fs_error(source, request_id, WASMOS_ERR_FS_BUFFER);
            return 1;
        }
        requested[path_len] = '\0';
    }

    if (!fsmgr_cwd_join(state->cwd, (const char*)requested, target, (int32_t)sizeof(target))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_PATH_TOO_LONG);
        return 1;
    }
    if (!route_absolute_path(target, tail, (int32_t)sizeof(tail), &tail_len, &backend)) {
        /* Nothing owns it. For the root that means no root filesystem is
         * mounted, and standing there is still legal; for anything else the
         * directory does not exist. */
        if (target[1] != '\0') {
            send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_FOUND);
            return 1;
        }
        state->mount = FS_MOUNT_ROOT;
        state->backend_endpoint = -1;
        state->cwd[0] = '/';
        state->cwd[1] = '\0';
        (void)reply_to_client(
            source, FS_IPC_RESP, request_id, 0, chdir_report_cwd(state, buffer_id), 0, 0);
        return 1;
    }
    synced = backend_sync_cwd(backend, request_id, tail, source);
    if (synced != 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_FOUND);
        return 1;
    }
    /* Committed only now: a refused chdir leaves the client exactly where it
     * was, rather than somewhere the backend never confirmed. */
    str_copy(state->cwd, sizeof(state->cwd), target);
    state->mount = FS_MOUNT_BACKEND;
    state->backend_endpoint = backend;
    (void)reply_to_client(
        source, FS_IPC_RESP, request_id, 0, chdir_report_cwd(state, buffer_id), 0, 0);
    return 1;
}

/* Filesystem types fs-manager can place, and the driver implementing each.
 *
 * The module path is RELATIVE, resolved against `/init/` then `/boot/` the same
 * way a device-manager rule's RUN+= target is: a driver present only on the ESP
 * would never be found by an initfs-relative path, and one present only in initfs
 * is found before the disk is up.
 *
 * `needs_source` says the filesystem has a device. Those drivers accept
 * `id=<canonical block id>`, so naming the volume is possible; requiring it is a
 * decision, not a limitation -- see handle_mount_req. */
typedef struct {
    const char* type;
    const char* module;
    int32_t needs_source;
} fsmgr_fstype_t;

static const fsmgr_fstype_t g_fstypes[] = {
    {"tmpfs", "system/drivers/fs_tmpfs.wap", 0},
    {"fat", "system/drivers/fs_fat.wap", 1},
    {"wfs", "system/drivers/fs_wfs.wap", 1},
};

/* Request ids for the nested spawn calls a mount makes. Separate from any client
 * request id: these travel on fs-manager's own reply endpoint, and reusing a
 * client's id would let a stale client reply satisfy a spawn await. */
static int32_t g_mount_request_id = 1;

/* Longest mount descriptor accepted. The process manager truncates startup
 * arguments at WASMOS_STARTUP_ARGS_MAX (255), and the descriptor's tokens become
 * those arguments, so a longer one could not be delivered anyway. */
#define FSMGR_MOUNT_DESC_MAX 256

/* Deadline for a spawned backend to reach readiness, matching device-manager's.
 * A driver that never registers must not hold the FS service loop. */
#define FSMGR_MOUNT_SPAWN_TIMEOUT_MS 5000

/* Copy the value of `key` out of a whitespace-separated `key=value` descriptor.
 *
 * Matches only at the start of a token, so `source=` cannot be found inside
 * `nosource=`. Returns 1 when the key is present and its value fits. */
static int32_t desc_token(const char* desc, const char* key, char* out, uint32_t out_cap) {
    uint32_t i = 0;
    uint32_t klen = 0;

    if (!desc || !key || !out || out_cap == 0u) {
        return 0;
    }
    out[0] = '\0';
    while (key[klen] != '\0') {
        klen++;
    }
    while (desc[i] != '\0') {
        uint32_t j = 0;
        while (desc[i] == ' ' || desc[i] == '\t') {
            i++;
        }
        if (desc[i] == '\0') {
            break;
        }
        if (strncmp(desc + i, key, klen) == 0) {
            i += klen;
            while (desc[i] != '\0' && desc[i] != ' ' && desc[i] != '\t') {
                if (j + 1u >= out_cap) {
                    /* Refuse rather than truncate: a shortened path or device id
                     * names something other than what the caller asked for. */
                    out[0] = '\0';
                    return 0;
                }
                out[j++] = desc[i++];
            }
            out[j] = '\0';
            return j > 0u ? 1 : 0;
        }
        while (desc[i] != '\0' && desc[i] != ' ' && desc[i] != '\t') {
            i++;
        }
    }
    return 0;
}

static const fsmgr_fstype_t* fsmgr_fstype_find(const char* type) {
    for (uint32_t i = 0; i < sizeof(g_fstypes) / sizeof(g_fstypes[0]); ++i) {
        if (strcmp(g_fstypes[i].type, type) == 0) {
            return &g_fstypes[i];
        }
    }
    return 0;
}

/* A mount request waiting on the driver it asked the process manager to spawn.
 *
 * ONE at a time. A second request while this is occupied is answered with
 * WASMOS_ERR_FS_BUSY, which is what that code means: a transient shortage of a
 * single-slot resource that clears on its own, unlike MOUNT_EXISTS or
 * MOUNT_FSTYPE. The process manager has a single sync-spawn slot of its own, so
 * a deeper queue here would only move the refusal.
 *
 * The path/args buffer is held for the whole exchange rather than released after
 * the send: the process manager reads it inside its own handler, and fs-manager
 * cannot observe when that finishes. */
typedef struct {
    int32_t in_use;
    int32_t client;
    int32_t request_id;
    int32_t buffer_id;
    uint32_t prefix; /* index into fsmgr_module_prefixes */
    const char* module;
    char mount[FSMGR_MOUNT_PATH_MAX];
    char args[FSMGR_MOUNT_DESC_MAX];
} fsmgr_pending_mount_t;

static fsmgr_pending_mount_t g_pending_mount;

/* Where a relative driver module path is looked for, in order — the same two
 * roots a device-manager rule's RUN+= target resolves against. A module present
 * only on the ESP is not in initfs, and one present only in initfs is found
 * before any disk is up, so both have to be tried. */
static const char* const fsmgr_module_prefixes[] = {"/init/", "/boot/"};

/* Ask the process manager to spawn `g_pending_mount.module` under prefix
 * `g_pending_mount.prefix`, WITHOUT waiting for the reply.
 *
 * Not waiting is the whole point. The process manager READS the module blob
 * through fs-manager while handling this request, so an fs-manager that blocked
 * on the reply would be the one service unable to answer, and the spawn would
 * fail with "the filesystem never answered" — a mutual wait, not a slow path. By
 * returning to the main loop, fs-manager serves that read like any other, and the
 * spawn's own reply arrives there too (the reply endpoint is the SERVICE endpoint,
 * not the private one used for nested calls).
 *
 * The SYNC spawn opcode is used precisely because its reply is deferred until the
 * child reports READY: the backend has registered by then, so the mount is in the
 * table when the reply is handled, and a driver that never becomes ready is bounded
 * by the timeout rather than leaving the client waiting forever. CAPS_SYNC rather
 * than PATH_SYNC because it is the only path spawn that carries startup ARGUMENTS;
 * the capability set it also carries is empty.
 *
 * Returns 0 when the request was sent, or a negative packed code. */
static int32_t fsmgr_mount_send_spawn(void) {
    char path[128];
    uint32_t path_len;
    uint32_t args_len;
    int32_t bid;

    path[0] = '\0';
    str_copy(path, sizeof(path), fsmgr_module_prefixes[g_pending_mount.prefix]);
    wasmos_sys_str_append(path, sizeof(path), g_pending_mount.module);
    path_len = (uint32_t)strlen(path);
    args_len = (uint32_t)strlen(g_pending_mount.args);
    if (path_len == 0u || path_len > 0xFFFu) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    bid = wasmos_xfer_buffer_acquire((int32_t)(path_len + args_len + 1u));
    if (bid < 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    /* Path at offset 0, NUL-terminated args immediately after it; arg1 packs the
     * buffer id above the path length. The process manager reads this buffer by
     * OWNERSHIP, so no grant is lent. */
    if (wasmos_xfer_buffer_write(bid, (const uint8_t*)path, (int32_t)path_len, 0) != 0 ||
        wasmos_xfer_buffer_write(bid,
                                 (const uint8_t*)g_pending_mount.args,
                                 (int32_t)(args_len + 1u),
                                 (int32_t)path_len) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_ipc_send(g_proc_endpoint,
                        g_fs_endpoint,
                        PROC_IPC_SPAWN_PATH_CAPS_SYNC,
                        g_mount_request_id++,
                        0,
                        (int32_t)(((uint32_t)bid << 12) | (path_len & 0xFFFu)),
                        0,
                        FSMGR_MOUNT_SPAWN_TIMEOUT_MS) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_FS_BACKEND_IPC;
    }
    g_pending_mount.buffer_id = bid;
    return 0;
}

static void fsmgr_mount_pending_clear(void) {
    if (g_pending_mount.buffer_id > 0) {
        (void)wasmos_xfer_buffer_release(g_pending_mount.buffer_id);
    }
    g_pending_mount.in_use = 0;
    g_pending_mount.buffer_id = -1;
}

static void fsmgr_mount_pending_fail(wasmos_error_code_t code) {
    int32_t client = g_pending_mount.client;
    int32_t request_id = g_pending_mount.request_id;
    fsmgr_mount_pending_clear();
    send_fs_error(client, request_id, code);
}

/* The process manager answered the spawn a pending mount is waiting on.
 *
 * A refusal is not final while another module prefix is untried: a path that does
 * not resolve under `/init/` is the expected case for a driver that ships only on
 * the ESP. Once the prefixes are exhausted the client is told the driver did not
 * come up.
 *
 * On success the child has reported READY, so its backend has registered — but
 * the registration arrives as a class event this loop has not necessarily
 * dispatched yet, so the class is pulled here rather than waited for. A backend
 * pulled twice is registered once (backend_register keys on the endpoint). */
static void fsmgr_mount_handle_spawn_reply(int32_t type, int32_t pid) {
    int32_t client;
    int32_t request_id;

    if (!g_pending_mount.in_use) {
        return;
    }
    if (type != PROC_IPC_RESP) {
        if (g_pending_mount.buffer_id > 0) {
            (void)wasmos_xfer_buffer_release(g_pending_mount.buffer_id);
            g_pending_mount.buffer_id = -1;
        }
        g_pending_mount.prefix++;
        if (g_pending_mount.prefix <
            sizeof(fsmgr_module_prefixes) / sizeof(fsmgr_module_prefixes[0])) {
            int32_t rc = fsmgr_mount_send_spawn();
            if (rc == 0) {
                return;
            }
            fsmgr_mount_pending_fail((wasmos_error_code_t)rc);
            return;
        }
        fsmgr_mount_pending_fail(WASMOS_ERR_FS_NOT_READY);
        return;
    }

    fsmgr_pull_all_backends();
    client = g_pending_mount.client;
    request_id = g_pending_mount.request_id;
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && strcmp(g_backends[i].mount_path, g_pending_mount.mount) == 0) {
            fsmgr_mount_pending_clear();
            (void)reply_to_client(client, FSMGR_IPC_MOUNT_RESP, request_id, 0, pid, 0, 0);
            return;
        }
    }
    /* Ready, but no backend registered at the path it was given: the driver
     * registered nothing, or reported a different mount. Nothing was added, so
     * there is nothing to undo — the process is left running, the same gap
     * unmount has. */
    log_msg("[fs-manager] mount: backend started but registered no mount\n");
    fsmgr_mount_pending_fail(WASMOS_ERR_FS_NOT_READY);
}

/* FSMGR_IPC_MOUNT_REQ: establish a mount.
 *
 * fs-manager implements no filesystem. It validates the placement, asks the
 * process manager to spawn the driver for the requested type, and has it handed
 * `mount=` (plus `id=<source>` for a disk-backed one) as startup arguments — the
 * same contract a device-manager rule's ENV{MOUNT} uses, so placement is one
 * mechanism whether a mount comes from a boot rule or from a request.
 *
 * The descriptor is `key=value` text in a client-owned buffer rather than packed
 * arguments, because what a filesystem needs in order to be placed differs per
 * type and the set grows. Bare argument words would have to be reinterpreted per
 * type, which is the shape that makes an opcode outgrow itself.
 *
 * `source` is REQUIRED for a type that has a device, even though the drivers can
 * self-select a volume when given none: a mount that picks its own device is not
 * the mount the caller asked for, and the caller has no way to discover which one
 * it got. Conversely a source given for a memory-backed type is refused rather
 * than ignored, because silently dropping it would answer a different request.
 *
 * The reply is DEFERRED — see fsmgr_mount_send_spawn for why it must be, and
 * fsmgr_mount_handle_spawn_reply for what completes it. The mount POINT needs no
 * work: fsmgr_ensure_mount_points runs on every registration and creates it in
 * whichever filesystem covers it.
 *
 * TODO: a driver that is spawned, reports ready, and then wedges without
 * registering leaves nothing behind here, but the process stays. A driver that
 * never reports ready is bounded only by FSMGR_MOUNT_SPAWN_TIMEOUT_MS in the
 * process manager; fs-manager has no clock of its own to bound anything with. */
static void handle_mount_req(fs_client_state_t* state, int32_t source, int32_t request_id,
                             int32_t desc_len, int32_t buffer_id) {
    uint8_t desc[FSMGR_MOUNT_DESC_MAX];
    char type[16];
    char requested[FSMGR_CWD_MAX];
    char target[FSMGR_CWD_MAX];
    char mount[FSMGR_MOUNT_PATH_MAX];
    char source_id[BLOCK_DESCRIPTOR_ID_MAX];
    const fsmgr_fstype_t* fstype;
    int32_t has_source;
    int32_t rc;

    if (g_pending_mount.in_use) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BUSY);
        return;
    }
    if (buffer_id <= 0 || desc_len <= 0 || desc_len >= (int32_t)sizeof(desc) ||
        desc_len >= wasmos_xfer_buffer_size()) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BAD_ARGS);
        return;
    }
    if (wasmos_xfer_buffer_read(buffer_id, desc, desc_len, 0) != 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BUFFER);
        return;
    }
    desc[desc_len] = '\0';

    if (!desc_token((const char*)desc, "type=", type, (uint32_t)sizeof(type))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_MOUNT_FSTYPE);
        return;
    }
    fstype = fsmgr_fstype_find(type);
    if (!fstype) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_MOUNT_FSTYPE);
        return;
    }
    has_source = desc_token((const char*)desc, "source=", source_id, (uint32_t)sizeof(source_id));
    if (has_source != fstype->needs_source) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_MOUNT_FSTYPE);
        return;
    }
    if (!desc_token((const char*)desc, "mount=", requested, (uint32_t)sizeof(requested))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BAD_ARGS);
        return;
    }
    /* Resolved against the client's working directory, so `mount=scratch` means
     * the same thing here as every other path a client names. */
    if (!fsmgr_cwd_join(state->cwd, requested, target, (int32_t)sizeof(target))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_PATH_TOO_LONG);
        return;
    }
    if (!fsmgr_mount_path_from_reported(target, mount, (uint32_t)sizeof(mount))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_ABSOLUTE);
        return;
    }
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && strcmp(g_backends[i].mount_path, mount) == 0) {
            send_fs_error(source, request_id, WASMOS_ERR_FS_MOUNT_EXISTS);
            return;
        }
    }

    g_pending_mount.in_use = 1;
    g_pending_mount.client = source;
    g_pending_mount.request_id = request_id;
    g_pending_mount.buffer_id = -1;
    g_pending_mount.prefix = 0;
    g_pending_mount.module = fstype->module;
    str_copy(g_pending_mount.mount, sizeof(g_pending_mount.mount), mount);
    /* Built in the order the drivers document: `id=` then `mount=`. */
    g_pending_mount.args[0] = '\0';
    if (has_source) {
        str_copy(g_pending_mount.args, sizeof(g_pending_mount.args), "id=");
        wasmos_sys_str_append(g_pending_mount.args, sizeof(g_pending_mount.args), source_id);
        wasmos_sys_str_append(g_pending_mount.args, sizeof(g_pending_mount.args), " ");
    }
    wasmos_sys_str_append(g_pending_mount.args, sizeof(g_pending_mount.args), "mount=");
    wasmos_sys_str_append(g_pending_mount.args, sizeof(g_pending_mount.args), mount);

    rc = fsmgr_mount_send_spawn();
    if (rc != 0) {
        fsmgr_mount_pending_fail((wasmos_error_code_t)rc);
    }
}

/* FSMGR_IPC_UNMOUNT_REQ: remove one mount from the namespace.
 *
 * The mount is named by PATH, not by backend endpoint or class instance: a
 * client knows where a filesystem is, not which process serves it, and the path
 * is what `mount` reports. The path arrives in the client's buffer (arg2, arg0 =
 * its length) because a mount path can grow past what an argument word carries;
 * it is resolved against the client's working directory and canonicalized
 * exactly as a registration path is, so "/WFS", "/wfs/" and "/wfs" all name the
 * same mount.
 *
 * The refusal cases are the whole point of the operation:
 *
 *  - nothing mounted at that path is WASMOS_ERR_FS_NO_BACKEND. A path that
 *    merely EXISTS inside some other mount is not a mount and is refused here,
 *    which is why the comparison is against the mount path rather than a route.
 *  - anything still standing in the mount is WASMOS_ERR_FS_MOUNT_BUSY. The
 *    requesting client counts: `umount` of the directory you are standing in
 *    fails, and "/" is normally unremovable because every client starts there.
 *
 * The backend is quiesced before it is dropped -- WASMOS_IPC_SHUTDOWN_REQ with
 * WASMOS_SHUTDOWN_REASON_UNMOUNT, the same sequence machine shutdown uses -- so
 * a filesystem with dirty state writes it while it still has a block device.
 * A backend that fails to answer is dropped anyway: the mount is going regardless
 * and leaving it in the table would make an unresponsive backend permanent.
 *
 * The mount POINT is left in place. It is a directory in the covering
 * filesystem, created when the mount was established, and it belongs to that
 * filesystem rather than to the mount -- removing it would delete a directory
 * fs-manager does not own, and would break re-mounting at the same path. What
 * reappears once the mount is gone is whatever the covering filesystem holds
 * there, which is the other half of shadowing.
 *
 * TODO: the backend PROCESS stays resident after its mount is dropped. It no
 * longer serves anything (fs-manager holds no reference and class discovery only
 * re-adds on an ADD event) but it still occupies a process slot, so repeated
 * mount/unmount cycles leak slots. Exiting needs a process-exit path a driver
 * can call after answering DONE, which no driver has today. */
static void handle_unmount_req(fs_client_state_t* state, int32_t source, int32_t request_id,
                               int32_t path_len, int32_t buffer_id) {
    uint8_t requested[FSMGR_CWD_MAX];
    char target[FSMGR_CWD_MAX];
    char mount[FSMGR_MOUNT_PATH_MAX];
    fs_backend_t* victim = 0;
    wasmos_error_code_t busy;
    int32_t endpoint;
    int32_t rr_t = FS_IPC_ERROR, rr0 = 0, rr1 = 0, rr2 = 0, rr3 = 0;

    if (buffer_id <= 0 || path_len <= 0 || path_len >= (int32_t)sizeof(requested) ||
        path_len >= wasmos_xfer_buffer_size()) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_PATH_TOO_LONG);
        return;
    }
    if (wasmos_xfer_buffer_read(buffer_id, requested, path_len, 0) != 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BUFFER);
        return;
    }
    requested[path_len] = '\0';
    if (!fsmgr_cwd_join(state->cwd, (const char*)requested, target, (int32_t)sizeof(target))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_PATH_TOO_LONG);
        return;
    }
    if (!fsmgr_mount_path_from_reported(target, mount, (uint32_t)sizeof(mount))) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_ABSOLUTE);
        return;
    }
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && strcmp(g_backends[i].mount_path, mount) == 0) {
            victim = &g_backends[i];
            break;
        }
    }
    if (!victim) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NO_BACKEND);
        return;
    }
    busy = fsmgr_mount_busy_reason(victim);
    if (busy != WASMOS_ERR_NONE) {
        send_fs_error(source, request_id, busy);
        return;
    }

    endpoint = victim->endpoint;
    /* The table entry goes first. The quiesce below blocks on a reply, and while
     * it does, this loop is not serving anyone -- but a backend that answers by
     * issuing work of its own must not find its own mount still routable. */
    victim->in_use = 0;
    fsmgr_forget_backend_in_clients(endpoint);

    if (forward_request(endpoint,
                        WASMOS_IPC_SHUTDOWN_REQ,
                        request_id,
                        WASMOS_SHUTDOWN_REASON_UNMOUNT,
                        0,
                        0,
                        0,
                        source,
                        &rr_t,
                        &rr0,
                        &rr1,
                        &rr2,
                        &rr3) != 0 ||
        rr_t != (int32_t)WASMOS_IPC_SHUTDOWN_DONE) {
        /* Reported, not returned: the mount IS gone, so answering the client
         * with an error would describe a failure that did not happen. What the
         * backend failed to flush is the backend's loss to record. */
        log_msg("[fs-manager] unmount: backend did not quiesce\n");
    }
    (void)reply_to_client(source, FSMGR_IPC_UNMOUNT_RESP, request_id, 0, 0, 0, 0);
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
    g_cwd_bid = wasmos_xfer_buffer_acquire(FSMGR_CWD_MAX);
    log_msg("[fs-manager] init start\n");
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0 || g_fs_endpoint < 0 || g_cwd_bid < 0) {
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

        /* The process manager's answer to a mount's spawn. It arrives on the
         * SERVICE endpoint rather than the private reply endpoint precisely
         * because fs-manager must not be blocked waiting for it. */
        if (type == PROC_IPC_RESP || type == PROC_IPC_ERROR) {
            fsmgr_mount_handle_spawn_reply(type, arg0);
            continue;
        }

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

        /* UNMOUNT resolves its path against the client's cwd and refuses on the
         * client's own position, so it needs client state -- unlike
         * QUERY_MOUNTS, which reads the table and needs none. */
        if (type == FSMGR_IPC_UNMOUNT_REQ) {
            handle_unmount_req(state, source, request_id, arg0, arg2f);
            continue;
        }
        if (type == FSMGR_IPC_MOUNT_REQ) {
            handle_mount_req(state, source, request_id, arg0, arg2f);
            continue;
        }

        if (type == FS_IPC_READ_PATH_REQ) {
            (void)handle_read_path_req(state, source, request_id, arg0, arg1f, arg2f, arg3f);
            continue;
        }

        /* READDIR names no path, so the backend lists whichever directory it
         * currently holds -- and it holds one per fs-manager connection, not one
         * per client. Re-assert this client's directory first, or a listing is
         * whatever the last client to chdir left behind. */
        if (type == FS_IPC_READDIR_REQ) {
            char tail[FSMGR_CWD_MAX];
            int32_t tail_len = 0;
            int32_t dir_backend = -1;
            if (!route_absolute_path(
                    state->cwd, tail, (int32_t)sizeof(tail), &tail_len, &dir_backend) ||
                backend_sync_cwd(dir_backend, request_id, tail, source) != 0) {
                send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_FOUND);
                continue;
            }
        }

        /* CHDIR is resolved entirely here: fs-manager owns the working
         * directory, so nothing about it is decided from a backend's reply. */
        if (type == FS_IPC_CHDIR_REQ) {
            (void)handle_chdir_mount(state, source, request_id, arg0, arg2f);
            continue;
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
        if (type == FS_IPC_RENAME_REQ) {
            if (route_rename_request(state, arg2f, &req_arg0, &arg1f, &backend) < 0) {
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
        (void)reply_to_client(source, resp_type, request_id, r0, r1, r2, r3);
    }
}
