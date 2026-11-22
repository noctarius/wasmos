/* fs_manager.c - VFS multiplexer service ("fs.vfs"): routes file operations to
 * the FS backend drivers (fs-fat, fs-init) by mount-name prefix, with
 * per-context client state tracked in a custom bump+chunk heap.  Backends are
 * found by subscribing to the FSMGR_BACKEND_CLASS service class and pulling each
 * provider's FSMGR_IPC_BACKEND_INFO, so the set is rebuilt on (re)start. */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "sys/stat.h" /* S_IFMT / S_IFDIR, for validating a chdir target */
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"
#include "fs_manager_types.h"
#include "fs_manager_path.h"
#include "fs_manager_backends.h"
#include "fs_manager_clients.h"
#include "fs_manager_pending.h"
#include "fs_manager_units.h"

/* Retries a reply gets before this service gives up on a client whose endpoint
 * stays full. Generous on purpose: the failure it guards against is permanent
 * (a stranded client hangs forever) while the condition it waits out is
 * transient (a client mid-output-loop drains within a few scheduling rounds). */
#define FSMGR_REPLY_SEND_RETRIES 8192
/* Closes for ended clients that may be in flight at once; the rest wait in their
 * releasing slots. Half the IPC queue depth (32): a backend replying to a full
 * g_release_endpoint retries instead of serving the next request, and leaves room
 * in its own queue for the requests fs-manager relays to it. */
#define FSMGR_RELEASE_CLOSES_MAX 16u
/* How long the main loop parks before retrying a close that could not be sent
 * or that a backend refused as busy, when nothing else would wake it. */
#define FSMGR_RELEASE_RETRY_MS 20

#define FSMGR_PATH_SCRATCH_SIZE 256

static int32_t g_proc_endpoint = -1;
static int32_t g_fs_endpoint = -1;
/* Source of every request fs-manager sends a backend, and where the answers
 * land. Nothing waits on it: the main loop drains it (fsmgr_reply_drain) and
 * matches each answer to its record in g_pending by request id. The process
 * manager's answers to the class subscription and lookup at startup also land
 * here, awaited before the loop runs and before any backend request is sent. */
static int32_t g_reply_endpoint = -1;
/* Source of the closes fs-manager sends on behalf of an ended client, and where
 * their replies land. Separate from g_reply_endpoint because those replies are
 * matched against the client table's close records, not against g_pending; the
 * main loop drains it (fsmgr_release_drain). */
static int32_t g_release_endpoint = -1;
/* g_fs_endpoint, g_reply_endpoint and g_release_endpoint, so the main loop parks
 * on all three. Required: a service that cannot wait for a backend's answer
 * alongside its clients' requests cannot defer, so fs-manager parks for good
 * when the set cannot be created. */
static int32_t g_main_select = -1;
/* Requests in flight to backends (fs_manager_pending.h). */
static fsmgr_pending_table_t g_pending;
/* Request id of the last close fsmgr_release_pump sent. Positive and unique
 * among outstanding closes; a reply is matched to its file by it. */
static int32_t g_release_request_id = 0;
static fs_backend_t g_backends[FS_BACKEND_CAP];
extern uint8_t __heap_base;

static fs_client_table_t g_clients;
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

void* fsmgr_clients_alloc_chunk(uint32_t size) {
    return fsmgr_heap_alloc(size, 8u);
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

/* Find or create per-context state for context_id. Returns NULL only if the
 * heap is exhausted. */
static fs_client_state_t* client_state(int32_t context_id) {
    return fsmgr_client_get(&g_clients, context_id);
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
    slot->source_id[0] = '\0';
    return slot;
}

/* Reply to a client, treating a full client queue as backpressure rather than
 * failure: retry with a yield between tries.
 *
 * Every reply this service sends is one a client is blocked waiting for, with no
 * timeout behind it. Dropping one because the client's 32-slot endpoint is
 * momentarily full -- which a READDIR makes likely, since the stream frames just
 * filled it and the terminating reply follows immediately -- hangs that client
 * forever, and behind it everything waiting on that client. The STREAM relay in
 * fsmgr_reply_drain retries for the same reason.
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
    /* Sized for every mount a default boot carries plus the runtime ones, each
     * now also naming its device. The loop stops appending when the next line
     * would not fit, so a buffer that is too small silently drops the mounts at
     * the end -- the ones a reader is most likely to have just created. */
    char mounts[768];
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
        /* The device the filesystem sits on, when it has one. Named by canonical
         * id because that is the same string `blkinfo` prints and `mount -t` and
         * `fsck.wfs` take, so a reader can carry it between them without
         * translating. A memory-backed filesystem prints nothing here rather than
         * a placeholder: it is not that its device is unknown, it is that there
         * is none. */
        if (n > 0 && (uint32_t)n < sizeof(mounts) - pos && g_backends[i].source_id[0] != '\0') {
            int m = snprintf(mounts + pos + (uint32_t)n,
                             sizeof(mounts) - (pos + (uint32_t)n),
                             " on %s",
                             g_backends[i].source_id);
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

/* `reason` is a packed WASMOS_ERR_FS_* code, or a code relayed unchanged from a
 * backend reply. */
static void send_fs_error(int32_t source, int32_t request_id, wasmos_error_code_t reason) {
    /* Retry with yield for the same reason as reply_to_client: dropping the
     * error notification because the client's endpoint is transiently full hangs
     * that client forever in select_one. */
    (void)wasmos_sys_ipc_send_retry(
        source, g_fs_endpoint, FS_IPC_ERROR, request_id, reason, 0, 0, 0, FSMGR_REPLY_SEND_RETRIES);
}

/* Send `type` to the record's backend under the record's id, from the reply
 * endpoint, where the answer lands for fsmgr_reply_drain. Never waits for it.
 * Returns 0, or WASMOS_ERR_FS_BACKEND_IPC when the request could not be
 * delivered. */
static int32_t pending_send(const fsmgr_pending_t* entry, int32_t type, int32_t a0, int32_t a1,
                            int32_t a2, int32_t a3) {
    if (entry->backend < 0 ||
        wasmos_ipc_send(entry->backend, g_reply_endpoint, type, entry->id, a0, a1, a2, a3) != 0) {
        return WASMOS_ERR_FS_BACKEND_IPC;
    }
    return 0;
}

/* End a record: take back the grant fs-manager lent for it, release the buffer it
 * owns for it, and free it. Done BEFORE the client is answered, so a client that
 * releases its buffer on the answer finds no grant of fs-manager's beneath it. */
static void pending_end(fsmgr_pending_t* entry) {
    if (entry->borrow >= 0) {
        (void)wasmos_xfer_buffer_unborrow(entry->borrow);
    }
    if (entry->buffer >= 0) {
        (void)wasmos_xfer_buffer_release(entry->buffer);
    }
    fsmgr_pending_free(entry);
}

/* End a record and answer its client, if it still has one, with `code`. */
static void pending_abort(fsmgr_pending_t* entry, wasmos_error_code_t code) {
    int32_t client = entry->client;
    int32_t request_id = entry->client_request_id;
    pending_end(entry);
    if (client >= 0) {
        send_fs_error(client, request_id, code);
    }
}

/* Claim a record answering request `request_id` of `source`, whose state is
 * keyed by `context`. A full table refuses the request with WASMOS_ERR_FS_BUSY,
 * which is retryable, and returns NULL. */
static fsmgr_pending_t* pending_for_client(fsmgr_step_t step, int32_t backend, int32_t source,
                                           int32_t request_id, int32_t context) {
    fsmgr_pending_t* entry = fsmgr_pending_alloc(&g_pending, step, backend);
    if (!entry) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BUSY);
        return 0;
    }
    entry->client = source;
    entry->client_request_id = request_id;
    entry->context = context;
    return entry;
}

/* Give `entry` a buffer of its own holding `path` and lend its backend READ on
 * it: the path of a request fs-manager supplies itself (READDIR's directory, a
 * CHDIR target, a mount point), which no client buffer carries. A buffer per
 * record, because several such requests may be in flight at once; released when
 * the record ends, which revokes the grant with it.
 *
 * Returns the grant, or a negative packed code. */
static int32_t pending_lend_path(fsmgr_pending_t* entry, const char* path, int32_t* out_len) {
    int32_t len;
    int32_t borrow;

    if (!path || path[0] != '/') {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    len = (int32_t)strlen(path);
    if (len >= wasmos_xfer_buffer_size()) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }
    entry->buffer = wasmos_xfer_buffer_acquire(len);
    if (entry->buffer < 0) {
        entry->buffer = -1;
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_xfer_buffer_write(entry->buffer, (const uint8_t*)path, len, 0) != 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    borrow = wasmos_xfer_buffer_borrow(entry->backend, entry->buffer, WASMOS_BUFFER_GRANT_READ);
    if (borrow < 0) {
        return WASMOS_ERR_FS_REBORROW;
    }
    *out_len = len;
    return borrow;
}

/* A close of `backend_fd` that no client asked for: the file of an OPEN whose
 * client ended, or had no fd left, before the answer arrived. A close lost to a
 * full table leaves the file open on its backend, which says so. */
static void pending_discard_close(int32_t backend, int32_t backend_fd) {
    fsmgr_pending_t* entry = fsmgr_pending_alloc(&g_pending, FSMGR_STEP_DISCARD_CLOSE, backend);
    if (!entry) {
        printf("[fs-manager] no record free to close backend fd %d\n", (int)backend_fd);
        return;
    }
    if (pending_send(entry, FS_IPC_CLOSE_REQ, backend_fd, 0, 0, 0) != 0) {
        pending_end(entry);
    }
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
 * with BACKEND_IPC as the first command of a session -- a request cannot be sent
 * to a negative endpoint -- and it was invisible while "/" was not a mount, because
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
                                     const char* reported_mount, const char* source_id) {
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
    registered->fs_type = (uint32_t)fs_type;
    str_copy(registered->mount_path, sizeof(registered->mount_path), mount);
    str_copy(registered->source_id, sizeof(registered->source_id), source_id ? source_id : "");
}

/* Create one directory in `backend`, under the path that backend sees.
 *
 * Not waited for, and nobody is answered: an EXISTS reply is the expected case,
 * since this runs on every registration, and a refusal costs the mount point's
 * entry in a listing, not the mount (fsmgr_ensure_mount_points). */
static void fsmgr_backend_mkdir(int32_t backend, const char* path) {
    fsmgr_pending_t* entry;
    int32_t len = 0;
    int32_t borrow;

    if (backend < 0 || !path || path[0] != '/' || path[1] == '\0') {
        return;
    }
    entry = fsmgr_pending_alloc(&g_pending, FSMGR_STEP_MKDIR, backend);
    if (!entry) {
        printf("[fs-manager] no record free to create mount point %s\n", path);
        return;
    }
    borrow = pending_lend_path(entry, path, &len);
    if (borrow < 0 || pending_send(entry, FS_IPC_MKDIR_REQ, len, 0, entry->buffer, borrow) != 0) {
        pending_end(entry);
    }
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
 * routing one.
 *
 * The MKDIRs are not waited for, and need not be for their order to hold. A
 * backend serves the requests from one sender in the order they were sent, and
 * every request that could observe a mount point -- an ancestor's MKDIR, a
 * listing of its parent -- goes to the backend that covers that parent, which is
 * the backend the MKDIR went to, and from the same reply endpoint. So `/mnt` is
 * made before `/mnt/usb`, and a client told a mount exists lists its point. */
static void fsmgr_ensure_mount_points(void) {
    uint32_t i;

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
                fsmgr_backend_mkdir(backend, target);
            }
            if (mount[seg_end] == '\0') {
                break;
            }
            seg_end++; /* step over the separator onto the next segment */
        }
    }
}

/* ------------------------------------------------------------ mount waits */

/* FSMGR_IPC_MOUNT_WAIT_REQ callers whose mount does not exist yet. Kernel init
 * is the one boot caller; the table is small because a wait is a milestone, not
 * a per-request resource. */
#define FSMGR_MOUNT_WAITS_CAP 4u

_Static_assert(FSMGR_MOUNT_WAIT_PATH_MAX + 1u == FSMGR_MOUNT_PATH_MAX,
               "a mount wait names a path the mount table can hold");

typedef struct {
    uint8_t in_use;
    int32_t source; /* the endpoint the deferred reply goes to */
    int32_t request_id;
    char path[FSMGR_MOUNT_PATH_MAX];
} fsmgr_mount_wait_t;

static fsmgr_mount_wait_t g_mount_waits[FSMGR_MOUNT_WAITS_CAP];

static void fsmgr_mount_pending_complete(void);

static int fsmgr_mount_exists(const char* path) {
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && strcmp(g_backends[i].mount_path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Answers every wait whose mount now exists. Run after each registration. */
static void fsmgr_mount_waits_fire(void) {
    for (uint32_t i = 0; i < FSMGR_MOUNT_WAITS_CAP; ++i) {
        fsmgr_mount_wait_t* w = &g_mount_waits[i];
        if (w->in_use && fsmgr_mount_exists(w->path)) {
            w->in_use = 0;
            (void)reply_to_client(w->source, FSMGR_IPC_MOUNT_WAIT_RESP, w->request_id, 0, 0, 0, 0);
        }
    }
}

/* A waiter's process ended: nobody is left to answer. */
static void fsmgr_mount_waits_drop(int32_t dead_endpoint) {
    for (uint32_t i = 0; i < FSMGR_MOUNT_WAITS_CAP; ++i) {
        if (g_mount_waits[i].in_use && g_mount_waits[i].source == dead_endpoint) {
            g_mount_waits[i].in_use = 0;
        }
    }
}

/* FSMGR_IPC_MOUNT_WAIT_REQ: arg0=buffer_id arg1=offset arg2=size of a
 * wasmos_fsmgr_mount_wait_desc_t the caller lent READ. The path is copied out
 * here; the reply is deferred until the mount exists (fsmgr_mount_waits_fire). */
static void handle_mount_wait_req(int32_t source, int32_t request_id, int32_t buffer_id,
                                  int32_t offset, int32_t size) {
    wasmos_fsmgr_mount_wait_desc_t desc;
    fsmgr_mount_wait_t* slot = 0;

    if (size < (int32_t)sizeof(desc) || offset < 0 ||
        wasmos_xfer_buffer_read(buffer_id, &desc, (int32_t)sizeof(desc), offset) != 0) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BUFFER);
        return;
    }
    if (desc.version != FSMGR_MOUNT_WAIT_DESC_VERSION || desc.path_len == 0u ||
        desc.path_len > FSMGR_MOUNT_WAIT_PATH_MAX) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_BAD_ARGS);
        return;
    }
    desc.path[desc.path_len] = '\0';
    if (desc.path[0] != '/') {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NOT_ABSOLUTE);
        return;
    }
    if (fsmgr_mount_exists(desc.path)) {
        (void)reply_to_client(source, FSMGR_IPC_MOUNT_WAIT_RESP, request_id, 0, 0, 0, 0);
        return;
    }
    for (uint32_t i = 0; i < FSMGR_MOUNT_WAITS_CAP; ++i) {
        if (!g_mount_waits[i].in_use) {
            slot = &g_mount_waits[i];
            break;
        }
    }
    if (!slot) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NO_CLIENT_SLOT);
        return;
    }
    slot->in_use = 1;
    slot->source = source;
    slot->request_id = request_id;
    memcpy(slot->path, desc.path, desc.path_len + 1u);
}

/* Ask a discovered backend for its identity over FSMGR_IPC_BACKEND_INFO; the
 * answer registers it (fsmgr_backend_info_done). Not waited for.
 *
 * fs-manager is the CLIENT of this exchange and therefore owns the buffer the
 * backend writes its mount name into: acquired here, borrowed to the backend
 * WRITE, released when the record ends on every path including the failures.
 * `release` cascade-revokes the grant, so there is nothing else to clean up
 * (docs/architecture/12-dma-transfers.md). A backend lending its own buffer
 * instead is refused ALREADY_BORROWED on the second pull, and each backend is
 * pulled more than once -- the class lookup and the ADD event both land here;
 * each pull has a buffer of its own. */
static void fsmgr_pull_backend(int32_t backend_endpoint, int32_t pid) {
    /* Both strings ride one buffer: the mount at offset 0 and the device id
     * immediately after it, lengths in arg2 and arg3. Neither is terminated in
     * the buffer -- the lengths delimit them -- so the capacity is exactly the
     * two fields. */
    const int32_t info_cap = (int32_t)(FSMGR_MOUNT_PATH_MAX + BLOCK_DESCRIPTOR_ID_MAX);
    fsmgr_pending_t* entry;

    if (backend_endpoint < 0) {
        return;
    }
    entry = fsmgr_pending_alloc(&g_pending, FSMGR_STEP_BACKEND_INFO, backend_endpoint);
    if (!entry) {
        printf("[fs-manager] no record free to pull backend %d; not registered\n",
               (int)backend_endpoint);
        return;
    }
    entry->pid = pid;
    entry->buffer = wasmos_xfer_buffer_acquire(info_cap);
    if (entry->buffer < 0) {
        entry->buffer = -1;
        pending_end(entry);
        return;
    }
    if (wasmos_xfer_buffer_borrow(backend_endpoint, entry->buffer, WASMOS_BUFFER_GRANT_WRITE) < 0 ||
        pending_send(entry, FSMGR_IPC_BACKEND_INFO_REQ, entry->buffer, 0, 0, 0) != 0) {
        pending_end(entry);
    }
}

/* The answer to fsmgr_pull_backend: arg0 = kind, arg1 = FS_TYPE_*, arg2 and arg3
 * the lengths of the mount path and device id in the record's buffer.
 *
 * A backend that names no mount is not registered: nothing else knows where it
 * belongs, so there is no default to fall back to. */
static void fsmgr_backend_info_done(fsmgr_pending_t* entry, int32_t type, int32_t kind,
                                    int32_t fs_type, int32_t name_len, int32_t source_len) {
    char reported_mount[FSMGR_MOUNT_PATH_MAX];
    char reported_source[BLOCK_DESCRIPTOR_ID_MAX];
    int32_t backend_endpoint = entry->backend;
    int32_t pid = entry->pid;

    if (type != FSMGR_IPC_BACKEND_INFO_RESP) {
        pending_end(entry);
        return;
    }
    if (name_len <= 0 || name_len >= (int32_t)sizeof(reported_mount) ||
        wasmos_sys_buffer_read(entry->buffer, reported_mount, name_len, 0) != 0) {
        printf("[fs-manager] backend named no mount; not registered\n");
        pending_end(entry);
        return;
    }
    reported_mount[name_len] = '\0';
    /* A device id is OPTIONAL -- a memory-backed filesystem has none -- so an
     * absent or unusable one leaves it empty and the mount registers anyway. It
     * is diagnostic: losing it costs a column in `mount`, not a mount. */
    reported_source[0] = '\0';
    if (source_len > 0 && source_len < (int32_t)sizeof(reported_source) &&
        wasmos_sys_buffer_read(entry->buffer, reported_source, source_len, name_len) == 0) {
        reported_source[source_len] = '\0';
    }
    pending_end(entry);
    fsmgr_apply_backend_info(backend_endpoint, kind, fs_type, reported_mount, reported_source);
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && g_backends[i].endpoint == backend_endpoint) {
            g_backends[i].pid = pid;
        }
    }
    /* The table changed, so the mount points may be incomplete: this backend's
     * own point did not exist before now, and an earlier backend's point could
     * not be created until the filesystem covering it registered. */
    fsmgr_ensure_mount_points();
    fsmgr_mount_waits_fire();
    fsmgr_mount_pending_complete();
}

/* Send closes for the files ended clients left open, as many as the cap allows.
 *
 * Fire and forget from g_release_endpoint: fs-manager must not block on a
 * backend for a client that no longer exists. A backend keys open files by the
 * sender's CONTEXT, so a close from this endpoint finds a file opened through
 * g_reply_endpoint. Each file stays recorded until fsmgr_release_drain sees the
 * reply confirming its close (fsmgr_client_orphan_closed).
 *
 * Returns 1 when a file is left that could not be sent because the backend's
 * queue was full, so the caller retries on a timer rather than waiting for
 * traffic that may never come. A send that fails for any other reason means the
 * backend is gone, and its file went with it. */
static int fsmgr_release_pump(void) {
    if (g_release_endpoint < 0 || g_main_select < 0) {
        return 0;
    }
    while (fsmgr_client_orphans_in_flight(&g_clients) < FSMGR_RELEASE_CLOSES_MAX) {
        fsmgr_client_fd_t* fd = fsmgr_client_next_orphan(&g_clients);
        if (!fd) {
            return 0;
        }
        if (++g_release_request_id <= 0) {
            g_release_request_id = 1;
        }
        int32_t rc = wasmos_ipc_send(fd->backend_endpoint,
                                     g_release_endpoint,
                                     FS_IPC_CLOSE_REQ,
                                     g_release_request_id,
                                     fd->backend_fd,
                                     0,
                                     0,
                                     0);
        if (rc == WASMOS_FULL) {
            return 1;
        }
        if (rc != 0) {
            fd->in_use = 0;
            continue;
        }
        fsmgr_client_orphan_sent(fd, g_release_request_id);
    }
    return 0;
}

/* Take every reply to fsmgr_release_pump's closes off g_release_endpoint.
 *
 * Only a reply naming an outstanding close counts; anything else that lands
 * here -- a hangup from a backend that ended, which carries no request id -- is
 * not one. A close refused as FS_BUSY is retried; any other refusal is reported
 * and the record dropped, since the backend will not close that fd. */
static void fsmgr_release_drain(void) {
    if (g_release_endpoint < 0) {
        return;
    }
    while (wasmos_ipc_drain(g_release_endpoint) > 0) {
        int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t status = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int busy = type == FS_IPC_ERROR && status == WASMOS_ERR_FS_BUSY;
        if (!fsmgr_client_orphan_closed(
                &g_clients, wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID), busy)) {
            continue;
        }
        if (type != FS_IPC_RESP && !busy) {
            printf("[fs-manager] backend endpoint %d refused an ended client's close: %s\n",
                   (int)wasmos_ipc_last_field(WASMOS_IPC_FIELD_SOURCE),
                   wasmos_strerror((wasmos_error_code_t)status));
        }
    }
}

/* Release the state held for `context_id`, whose process has ended.
 *
 * Driven by WASMOS_IPC_HANGUP, which the kernel delivers when a process that had
 * sent to this endpoint -- or that the process manager watched for it, because
 * fs-manager holds a copied working directory for it -- has ended. Scoped by
 * construction: this service hears about its own clients and nothing else, and
 * there is nothing to subscribe to.
 *
 * Its working directory is dropped at once. Its open files are closed on their
 * backends by fsmgr_release_pump, and its slot is reused once they are. */
static void fsmgr_release_client(int32_t context_id) {
    fsmgr_client_release(&g_clients, context_id);
    (void)fsmgr_release_pump();
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
    fs_client_chunk_t* chunk = g_clients.chunks;

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
 * Three things count as standing in a mount, and all three are the namespace
 * saying someone is still there rather than a shortage to retry:
 *
 *  - a DEEPER mount inside it. Removing the outer one first would leave the
 *    inner one addressable only through a path whose prefix no longer routes.
 *    This is also what normally makes "/" unremovable, since every other mount
 *    is inside it.
 *  - an OPEN FILE on it. The fd names a backend that would stop existing, and
 *    the client cannot re-derive it.
 *  - a client whose WORKING DIRECTORY is under it, which is the same rule Linux
 *    applies. The requesting client counts: `umount .` refuses, and so does
 *    unmounting the directory the shell is standing in.
 *
 * The working-directory rule is sound only because every entry this walks
 * belongs to a live process: WASMOS_IPC_HANGUP releases a client's state when its
 * process ends (fsmgr_release_client). A directory left behind by an exited
 * process would refuse the unmount forever -- one `cat` run inside a mount would
 * make it permanently unremovable. That covers a client that never sent
 * fs-manager anything too: a child whose directory the process manager copied
 * (FSMGR_IPC_CLONE_CWD) is released through the watch the process manager
 * registers for it (ipc_context_watch).
 */
static wasmos_error_code_t fsmgr_mount_busy_reason(const fs_backend_t* mount) {
    fs_client_chunk_t* chunk = g_clients.chunks;

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
            /* A releasing slot stands in no directory; its fds below still count,
             * because those files are still open on the backend. */
            if (!state->releasing && fsmgr_path_is_within(mount->mount_path, state->cwd)) {
                return WASMOS_ERR_FS_MOUNT_BUSY;
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

/* Defined with the rest of the deferred-reply handling, below the request
 * handlers whose records it completes. */
static void fsmgr_pending_fail_backend(int32_t backend);

/* Drop a backend that left its class (provider died / unregistered), and fail
 * the requests it held: nothing will answer them. */
static void fsmgr_backend_remove(int32_t backend_endpoint) {
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && g_backends[i].endpoint == backend_endpoint) {
            g_backends[i].in_use = 0;
        }
    }
    fsmgr_forget_backend_in_clients(backend_endpoint);
    fsmgr_pending_fail_backend(backend_endpoint);
}

/* Discover the current FS backends by class and pull each. Subscribe first so a
 * backend registering between here and the lookup still fires an event; the
 * lookup then captures the current set (and rebuilds it after an fs-manager
 * restart). backend_register is idempotent, so an overlap is harmless. */
/* Enumerate FSMGR_BACKEND_CLASS and pull every provider's identity. Run once,
 * at start. Pulling a provider already registered is harmless --
 * backend_register keys on the endpoint and updates the entry in place -- so a
 * backend seen both here and by its class event is registered once.
 *
 * TODO: this is a synchronous call to the process manager, which reads modules
 * THROUGH fs-manager; it is safe only because nothing spawns by path before
 * fs-manager has started. It should be the class event's work alone. */
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
        fsmgr_pull_backend((int32_t)backends[i].endpoint, 0);
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
 * and unborrows that reborrow before replying so the client's release()
 * succeeds. `capacity` bounds the read to the client buffer size (0 = full
 * transfer size). No relay copy.
 *
 * This is how the process manager reads every module it spawns, a restarted
 * driver's included, so it must never wait behind a backend: the open is sent
 * here, and each answer sends the next step (fsmgr_read_path_step). */
static int handle_read_path_req(fs_client_state_t* state, int32_t source, int32_t request_id,
                                int32_t path_len, int32_t capacity, int32_t buffer_id,
                                int32_t client_borrow) {
    int32_t backend = state ? state->backend_endpoint : -1;
    fsmgr_pending_t* entry;
    int32_t fs_buf_size = wasmos_xfer_buffer_size();
    int32_t read_cap = (capacity > 0 && capacity < fs_buf_size) ? capacity : fs_buf_size;
    uint8_t path_scratch[FSMGR_PATH_SCRATCH_SIZE];
    char abs_path[FSMGR_CWD_MAX + FSMGR_PATH_SCRATCH_SIZE];
    int32_t open_path_len = 0;

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
    entry = pending_for_client(
        FSMGR_STEP_READ_PATH_OPEN, backend, source, request_id, state ? state->context_id : -1);
    if (!entry) {
        return 1;
    }
    entry->client_buffer = buffer_id;
    entry->read_cap = read_cap;
    /* Reborrow to the backend (R|W: reads the path, writes the blob). */
    entry->borrow = wasmos_xfer_buffer_reborrow(
        backend, client_borrow, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (entry->borrow < 0) {
        entry->borrow = -1;
        pending_abort(entry, WASMOS_ERR_FS_REBORROW);
        return 1;
    }
    if (pending_send(entry, FS_IPC_OPEN_REQ, open_path_len, 0, buffer_id, entry->borrow) != 0) {
        pending_abort(entry, WASMOS_ERR_FS_BACKEND_IPC);
    }
    return 1;
}

/* Answer a READ_PATH with the outcome its record carries: the bytes read, or the
 * packed reason it failed. */
static void fsmgr_read_path_finish(fsmgr_pending_t* entry) {
    int32_t client = entry->client;
    int32_t request_id = entry->client_request_id;
    int32_t result = entry->result;

    /* The reborrow is dropped before replying; the client's own grant stays until
     * the client releases the buffer. */
    pending_end(entry);
    if (client < 0) {
        return;
    }
    if (result > 0) {
        (void)reply_to_client(client, FS_IPC_RESP, request_id, result, 0, 0, 0);
    } else {
        send_fs_error(client, request_id, (wasmos_error_code_t)result);
    }
}

/* Close the file a READ_PATH opened, whatever its read came to: the outcome is
 * already in the record and is answered once the close is. A close that cannot be
 * sent is answered at once. */
static void fsmgr_read_path_close(fsmgr_pending_t* entry) {
    entry->step = FSMGR_STEP_READ_PATH_CLOSE;
    (void)fsmgr_pending_renew(&g_pending, entry);
    if (pending_send(entry, FS_IPC_CLOSE_REQ, entry->fd, 0, 0, 0) != 0) {
        fsmgr_read_path_finish(entry);
    }
}

/* Advance a READ_PATH on the answer to its current step. */
static void fsmgr_read_path_step(fsmgr_pending_t* entry, int32_t type, int32_t r0) {
    if (entry->step == FSMGR_STEP_READ_PATH_OPEN) {
        if (type != FS_IPC_RESP || r0 < 0) {
            /* Relay the backend's own reason when it sent one -- it names the
             * failure better than anything this relay could substitute. */
            pending_abort(entry, r0 < 0 ? (wasmos_error_code_t)r0 : WASMOS_ERR_FS_BACKEND_IPC);
            return;
        }
        entry->fd = r0;
        entry->step = FSMGR_STEP_READ_PATH_READ;
        (void)fsmgr_pending_renew(&g_pending, entry);
        if (pending_send(entry,
                         FS_IPC_READ_REQ,
                         entry->fd,
                         entry->read_cap,
                         entry->client_buffer,
                         entry->borrow) != 0) {
            entry->result = WASMOS_ERR_FS_BACKEND_IPC;
            fsmgr_read_path_close(entry);
        }
        return;
    }
    if (entry->step == FSMGR_STEP_READ_PATH_READ) {
        if (type != FS_IPC_RESP || r0 < 0) {
            entry->result = r0 < 0 ? r0 : WASMOS_ERR_FS_BACKEND_IPC;
        } else if (r0 == 0 || r0 > entry->read_cap) {
            /* Backend reported a length outside the requested capacity. */
            entry->result = WASMOS_ERR_FS_RANGE;
        } else {
            entry->result = r0;
        }
        fsmgr_read_path_close(entry);
        return;
    }
    /* The close's own answer changes nothing: the file was read, or was not. */
    fsmgr_read_path_finish(entry);
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
 * The backend is asked with FS_IPC_STAT_REQ, which is the whole of what a chdir
 * needs to know and moves nothing; the target is committed when the answer
 * arrives (fsmgr_chdir_done), not here.
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
    int32_t borrow;
    fsmgr_pending_t* entry;

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
    entry =
        pending_for_client(FSMGR_STEP_CHDIR_STAT, backend, source, request_id, state->context_id);
    if (!entry) {
        return 1;
    }
    entry->client_buffer = buffer_id;
    str_copy(entry->path, sizeof(entry->path), target);
    borrow = pending_lend_path(entry, tail, &tail_len);
    if (borrow < 0) {
        pending_abort(entry, (wasmos_error_code_t)borrow);
        return 1;
    }
    if (pending_send(entry, FS_IPC_STAT_REQ, tail_len, 0, entry->buffer, borrow) != 0) {
        pending_abort(entry, WASMOS_ERR_FS_BACKEND_IPC);
    }
    return 1;
}

/* The STAT a CHDIR sent was answered: commit the target if it is a directory.
 * A client that ended meanwhile has no state left to commit it to. */
static void fsmgr_chdir_done(fsmgr_pending_t* entry, int32_t type, int32_t r0, int32_t mode) {
    fs_client_state_t* state;
    int32_t client = entry->client;
    int32_t request_id = entry->client_request_id;
    int32_t reported;

    if (type != FS_IPC_RESP) {
        /* The backend's reason, not a substituted one: NOT_FOUND reads very
         * differently from a backend that never answered. */
        pending_abort(entry, r0 < 0 ? (wasmos_error_code_t)r0 : WASMOS_ERR_FS_BACKEND_IPC);
        return;
    }
    /* The mode's type field is what distinguishes a directory from a file that
     * happens to exist at that path; NOT_DIR for a file the client tried to
     * stand in is the answer, and NOT_FOUND would be wrong. */
    if (((uint32_t)mode & S_IFMT) != S_IFDIR) {
        pending_abort(entry, WASMOS_ERR_FS_NOT_DIR);
        return;
    }
    state = entry->context >= 0 ? fsmgr_client_lookup(&g_clients, entry->context) : 0;
    if (!state || client < 0) {
        pending_end(entry);
        return;
    }
    /* Committed only now: a refused chdir leaves the client exactly where it
     * was, rather than somewhere the backend never confirmed. */
    str_copy(state->cwd, sizeof(state->cwd), entry->path);
    state->mount = FS_MOUNT_BACKEND;
    state->backend_endpoint = entry->backend;
    reported = chdir_report_cwd(state, entry->client_buffer);
    pending_end(entry);
    (void)reply_to_client(client, FS_IPC_RESP, request_id, 0, reported, 0, 0);
}

/* Filesystem types fs-manager can place, and the driver implementing each.
 *
 * The module path is ABSOLUTE and names the root the module actually ships
 * under, because the two roots do not hold the same set: fs_tmpfs is in initfs
 * only, fs_wfs on the ESP only, and fs_fat in both (initfs is preferred for it,
 * since it is always present and needs no disk).
 *
 * Naming the root rather than probing for it is deliberate. Probing meant
 * spawning under the first root and treating ANY failure as "wrong root", which
 * misreported every other cause: a spawn that failed for want of a process slot
 * came back as "/boot/system/drivers/fs_tmpfs.wap does not exist" -- a file that
 * is not supposed to exist, about a mount that failed for an unrelated reason.
 * It also cost a failed spawn and an alarming log line on every wfs mount. A
 * module that is not where this table says is a packaging error, and now says
 * so.
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
    {"tmpfs", "/init/system/drivers/fs_tmpfs.wap", 0},
    {"fat", "/init/system/drivers/fs_fat.wap", 1},
    {"wfs", "/boot/system/drivers/fs_wfs.wap", 1},
};

/* Request ids of what fs-manager asks the supervisor and the process manager
 * about mounts: the supervisor's lookup and each unit's SUBMIT and RETRACT. They
 * are sent from the service endpoint, so they share no id space with the records
 * in g_pending, whose answers land on the reply endpoint. */
#define FSMGR_SV_LOOKUP_REQUEST_ID 0x5F00
static int32_t g_sv_request_id = FSMGR_SV_LOOKUP_REQUEST_ID + 1;

/* Longest mount descriptor accepted. The process manager truncates startup
 * arguments at WASMOS_STARTUP_ARGS_MAX (255), and the descriptor's tokens become
 * those arguments, so a longer one could not be delivered anyway. */
#define FSMGR_MOUNT_DESC_MAX 256

/* How long a mount waits for its backend to register once the supervisor has
 * accepted its unit. It spans the supervisor's first restarts of a backend that
 * fails to start; a mount not up by then is refused and its unit retracted. */
#define FSMGR_MOUNT_TIMEOUT_MS 15000

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

/* A mount request waiting on its backend.
 *
 * ONE at a time. A second request while this is occupied is answered with
 * WASMOS_ERR_FS_BUSY, which is what that code means: a transient shortage of a
 * single-slot resource that clears on its own, unlike MOUNT_EXISTS or
 * MOUNT_FSTYPE. */
typedef struct {
    int32_t in_use;
    int32_t client;
    int32_t request_id;
    const char* module;
    char mount[FSMGR_MOUNT_PATH_MAX];
    char args[FSMGR_MOUNT_DESC_MAX];
    /* The unit submitted for it; NULL until the supervisor is known. */
    fsmgr_unit_t* unit;
    /* The supervisor accepted the unit; the reply waits for its backend to
     * register, until deadline_ms. */
    int32_t awaiting_backend;
    uint32_t deadline_ms;
} fsmgr_pending_mount_t;

static fsmgr_pending_mount_t g_pending_mount;
/* The units fs-manager submitted (fs_manager_units.h). */
static fsmgr_unit_t g_units[FSMGR_UNIT_CAP];
/* The supervisor's endpoint, from the one lookup sent at startup; -1 until it is
 * answered, and for good when it fails. */
static int32_t g_supervisor_endpoint = -1;
static int32_t g_supervisor_lookup_done = 0;

static uint32_t fsmgr_now_ms(void) {
    return (uint32_t)wasmos_sched_ticks() * (1000u / WASMOS_SCHED_TICK_HZ);
}

/* Look the supervisor up WITHOUT waiting; the answer reaches the main loop
 * (fsmgr_on_supervisor_lookup). Sent once: the supervisor registers its name
 * before it spawns fs-manager and is never restarted, so the one answer stands. */
static void fsmgr_lookup_supervisor(void) {
    int32_t name[4];
    wasmos_ipc_pack_name16("supervisor", name);
    if (wasmos_ipc_send(g_proc_endpoint,
                        g_fs_endpoint,
                        SVC_IPC_LOOKUP_REQ,
                        FSMGR_SV_LOOKUP_REQUEST_ID,
                        name[0],
                        name[1],
                        name[2],
                        name[3]) != 0) {
        log_msg("[fs-manager] supervisor lookup could not be sent; mounts are refused\n");
        g_supervisor_lookup_done = 1;
    }
}

/* Send `type` about `unit` to the supervisor from the service endpoint, with
 * `desc` staged in a buffer fs-manager owns and lends the supervisor READ. The
 * request id and the buffer are recorded on the unit; the answer reaches the
 * main loop (fsmgr_on_supervisor_reply), which releases the buffer. Returns 0,
 * or a negative packed code with nothing outstanding. */
static int32_t fsmgr_supervisor_send(fsmgr_unit_t* unit, int32_t type, const void* desc,
                                     int32_t size) {
    int32_t bid;
    if (g_supervisor_endpoint < 0) {
        return WASMOS_ERR_FS_NOT_READY;
    }
    bid = wasmos_xfer_stage(desc, size);
    if (bid < 0) {
        return WASMOS_ERR_FS_BUFFER;
    }
    if (wasmos_xfer_buffer_borrow(g_supervisor_endpoint, bid, WASMOS_BUFFER_GRANT_READ) < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_FS_BUFFER;
    }
    unit->request_id = g_sv_request_id++;
    if (wasmos_ipc_send(
            g_supervisor_endpoint, g_fs_endpoint, type, unit->request_id, bid, 0, size, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return WASMOS_ERR_FS_BACKEND_IPC;
    }
    unit->buffer_id = bid;
    return 0;
}

/* Answer the unmount waiting on `unit`, if one is. */
static void fsmgr_unit_answer_unmount(fsmgr_unit_t* unit) {
    if (unit->client >= 0) {
        (void)reply_to_client(
            unit->client, FSMGR_IPC_UNMOUNT_RESP, unit->client_request_id, 0, 0, 0, 0);
    }
}

/* Withdraw `unit` from the supervisor (SUPERVISOR_IPC_RETRACT_REQ). Its answer
 * decides the unmount waiting on it (fsmgr_on_supervisor_reply). A retraction
 * that cannot be sent is tried again shortly: the supervisor still holds the
 * unit, and with it the name and the path. */
static void fsmgr_unit_retract(fsmgr_unit_t* unit) {
    static wasmos_supervisor_unit_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = SUPERVISOR_UNIT_DESC_VERSION;
    desc.name_len = (uint32_t)strlen(unit->name);
    str_copy(desc.name, sizeof(desc.name), unit->name);
    unit->state = FSMGR_UNIT_RETRACTING;
    if (fsmgr_supervisor_send(unit, SUPERVISOR_IPC_RETRACT_REQ, &desc, (int32_t)sizeof(desc)) !=
        0) {
        fsmgr_unit_retract_unsent(unit, fsmgr_now_ms());
    }
}

/* Take over the answer of an unmount whose quiesce is still unanswered, when
 * its deadline passes: the record is ended, so a late DONE completes nothing,
 * and the unit answers the client once it is retracted. */
static void fsmgr_unit_take_unmount(fsmgr_unit_t* unit) {
    for (uint32_t i = 0; i < FSMGR_PENDING_CAP; ++i) {
        fsmgr_pending_t* entry = &g_pending.slots[i];
        if (entry->in_use && entry->step == FSMGR_STEP_UNMOUNT && entry->backend == unit->backend) {
            unit->client = entry->client;
            unit->client_request_id = entry->client_request_id;
            pending_end(entry);
            break;
        }
    }
    unit->quiesced = 1;
}

/* Retract every unmounted unit whose backend has ended, or whose deadline has
 * passed.
 *
 * Waiting for the end is what frees the unit's name for the next mount at that
 * path: the supervisor keeps a retracted unit's name until its instance has
 * ended. It always has by the time fs-manager sees the endpoint ownerless,
 * because the kernel posts the supervisor's exit notice before it releases the
 * process's endpoints, and the supervisor reads that notice and this request
 * from one queue, in order. The backend ends itself after an unmount's quiesce;
 * one that does not is killed by the retraction once the deadline passes. */
static void fsmgr_units_retire(void) {
    uint32_t now = fsmgr_now_ms();
    for (uint32_t i = 0; i < FSMGR_UNIT_CAP; ++i) {
        fsmgr_unit_t* unit = &g_units[i];
        int32_t alive;
        if (unit->state != FSMGR_UNIT_RETIRING && unit->state != FSMGR_UNIT_ENDING) {
            continue;
        }
        alive = wasmos_ipc_endpoint_owner(unit->backend) >= 0;
        switch (fsmgr_unit_due(unit, now, alive)) {
        case FSMGR_UNIT_DO_RETRACT:
            if (!unit->quiesced) {
                printf("[fs-manager] unmount: %s did not quiesce in time\n", unit->name);
                fsmgr_unit_take_unmount(unit);
            }
            fsmgr_unit_retract(unit);
            break;
        case FSMGR_UNIT_DO_ANSWER:
            fsmgr_unit_answer_unmount(unit);
            fsmgr_unit_release(unit);
            break;
        default:
            break;
        }
    }
}

static void fsmgr_mount_pending_clear(void) {
    g_pending_mount.in_use = 0;
    g_pending_mount.unit = 0;
    g_pending_mount.awaiting_backend = 0;
}

static void fsmgr_mount_pending_fail(wasmos_error_code_t code) {
    int32_t client = g_pending_mount.client;
    int32_t request_id = g_pending_mount.request_id;
    fsmgr_mount_pending_clear();
    send_fs_error(client, request_id, code);
}

/* Answers the pending mount once its backend is in the table, with the
 * backend's pid; called when the supervisor accepts the unit and after every
 * registration. */
static void fsmgr_mount_pending_complete(void) {
    int32_t client = g_pending_mount.client;
    int32_t request_id = g_pending_mount.request_id;
    int32_t pid = 0;
    if (!g_pending_mount.in_use || !g_pending_mount.awaiting_backend ||
        !fsmgr_mount_exists(g_pending_mount.mount)) {
        return;
    }
    for (uint32_t i = 0; i < FS_BACKEND_CAP; ++i) {
        if (g_backends[i].in_use && strcmp(g_backends[i].mount_path, g_pending_mount.mount) == 0) {
            pid = g_backends[i].pid;
        }
    }
    fsmgr_mount_pending_clear();
    (void)reply_to_client(client, FSMGR_IPC_MOUNT_RESP, request_id, 0, pid, 0, 0);
}

/* A pending mount whose backend did not register in time is refused, and its
 * unit retracted so that nothing keeps restarting a backend nobody waits for. */
static void fsmgr_mount_pending_expire(void) {
    fsmgr_unit_t* unit = g_pending_mount.unit;
    if (!g_pending_mount.in_use || !g_pending_mount.awaiting_backend ||
        !fsmgr_deadline_passed(fsmgr_now_ms(), g_pending_mount.deadline_ms)) {
        return;
    }
    printf("[fs-manager] mount %s: its backend did not register\n", g_pending_mount.mount);
    fsmgr_mount_pending_fail(WASMOS_ERR_FS_NOT_READY);
    if (unit && unit->state == FSMGR_UNIT_HELD) {
        unit->client = -1;
        fsmgr_unit_retract(unit);
    }
}

/* Milliseconds until the nearest deadline a mount or an unmount waits on, or -1
 * when none does. The main loop bounds its wait with it, since nothing else
 * wakes it when a backend simply never arrives or never ends. */
static int32_t fsmgr_next_deadline_ms(void) {
    uint32_t now = fsmgr_now_ms();
    int32_t best = -1;
    if (g_pending_mount.in_use && g_pending_mount.awaiting_backend) {
        int32_t left = (int32_t)(g_pending_mount.deadline_ms - now);
        best = left > 0 ? left : 0;
    }
    for (uint32_t i = 0; i < FSMGR_UNIT_CAP; ++i) {
        if (g_units[i].state == FSMGR_UNIT_RETIRING || g_units[i].state == FSMGR_UNIT_ENDING) {
            int32_t left = (int32_t)(g_units[i].deadline_ms - now);
            left = left > 0 ? left : 0;
            if (best < 0 || left < best) {
                best = left;
            }
        }
    }
    return best;
}

/* Submit the pending mount's backend to the supervisor as a unit
 * (SUPERVISOR_IPC_SUBMIT_REQ), named for the mount and restart=on-failure, with
 * the mount's arguments as its startup arguments. The answer reaches the main
 * loop (fsmgr_on_supervisor_reply).
 *
 * Not waiting is the whole point. The supervisor spawns the backend through the
 * process manager, which READS the module through fs-manager, so an fs-manager
 * that blocked on any part of this would be the one service unable to answer.
 *
 * A unit already recorded for the path refuses the mount as BUSY: its backend is
 * restarting, or the previous mount there is still being retracted. */
static void fsmgr_mount_submit(void) {
    static wasmos_supervisor_submit_desc_t desc;
    char name[SUPERVISOR_UNIT_NAME_MAX + 1];
    fsmgr_unit_t* unit;
    int32_t rc;

    if (g_supervisor_endpoint < 0) {
        fsmgr_mount_pending_fail(WASMOS_ERR_FS_NOT_READY);
        return;
    }
    rc = fsmgr_unit_name(g_pending_mount.module, g_pending_mount.mount, name, sizeof(name));
    if (rc < 0) {
        fsmgr_mount_pending_fail((wasmos_error_code_t)rc);
        return;
    }
    memset(&desc, 0, sizeof(desc));
    desc.version = SUPERVISOR_SUBMIT_DESC_VERSION;
    rc = fsmgr_unit_spec(
        name, g_pending_mount.module, g_pending_mount.args, desc.spec, sizeof(desc.spec));
    if (rc < 0) {
        fsmgr_mount_pending_fail((wasmos_error_code_t)rc);
        return;
    }
    desc.spec_len = (uint32_t)rc;
    unit = fsmgr_unit_claim(g_units, FSMGR_UNIT_CAP, name, g_pending_mount.mount);
    if (!unit) {
        fsmgr_mount_pending_fail(WASMOS_ERR_FS_BUSY);
        return;
    }
    rc = fsmgr_supervisor_send(unit, SUPERVISOR_IPC_SUBMIT_REQ, &desc, (int32_t)sizeof(desc));
    if (rc != 0) {
        fsmgr_unit_release(unit);
        fsmgr_mount_pending_fail((wasmos_error_code_t)rc);
        return;
    }
    g_pending_mount.unit = unit;
}

/* The process manager answered the supervisor's lookup. A mount that arrived
 * before it is submitted now. */
static void fsmgr_on_supervisor_lookup(int32_t endpoint) {
    g_supervisor_lookup_done = 1;
    if ((uint32_t)endpoint == 0xFFFFFFFFu || endpoint < 0) {
        log_msg("[fs-manager] no supervisor registered; mounts are refused\n");
    } else {
        g_supervisor_endpoint = endpoint;
    }
    if (g_pending_mount.in_use && !g_pending_mount.unit) {
        fsmgr_mount_submit();
    }
}

/* The supervisor answered a unit's SUBMIT or RETRACT; `code` is its packed code
 * on a refusal. The supervisor has copied the request before it answers, so the
 * buffer is released here, which also revokes its loan.
 *
 * An accepted SUBMIT holds the unit, and the pending mount then waits for the
 * backend to register; a refused one fails the mount with the supervisor's code.
 * A RETRACT's answer decides the unmount waiting on it (fsmgr_unit_retracted):
 * it is answered at once when no instance was left to kill or the unit was
 * already gone, after the killed instance has ended otherwise, and with the
 * supervisor's refusal -- keeping the unit, which the supervisor still holds --
 * when it was refused. */
static void fsmgr_on_supervisor_reply(int32_t type, int32_t request_id, int32_t code,
                                      int32_t pid_killed) {
    fsmgr_unit_t* unit = fsmgr_unit_by_request(g_units, FSMGR_UNIT_CAP, request_id);
    if (!unit) {
        return;
    }
    if (unit->buffer_id >= 0) {
        (void)wasmos_xfer_buffer_release(unit->buffer_id);
        unit->buffer_id = -1;
    }
    if (unit->state == FSMGR_UNIT_SUBMITTING) {
        int32_t pending = g_pending_mount.in_use && g_pending_mount.unit == unit;
        if (type != SUPERVISOR_IPC_RESP) {
            printf("[fs-manager] the supervisor refused %s\n", unit->name);
            fsmgr_unit_release(unit);
            if (pending) {
                fsmgr_mount_pending_fail((wasmos_error_code_t)code);
            }
            return;
        }
        unit->state = FSMGR_UNIT_HELD;
        if (pending) {
            g_pending_mount.awaiting_backend = 1;
            g_pending_mount.deadline_ms = fsmgr_now_ms() + FSMGR_MOUNT_TIMEOUT_MS;
            fsmgr_mount_pending_complete();
        }
        return;
    }
    switch (
        fsmgr_unit_retracted(unit, type == SUPERVISOR_IPC_RESP, code, pid_killed, fsmgr_now_ms())) {
    case FSMGR_UNIT_DO_ANSWER:
        fsmgr_unit_answer_unmount(unit);
        fsmgr_unit_release(unit);
        break;
    case FSMGR_UNIT_DO_REFUSE:
        printf("[fs-manager] the supervisor refused to retract %s\n", unit->name);
        if (unit->client >= 0) {
            send_fs_error(unit->client, unit->client_request_id, (wasmos_error_code_t)unit->result);
            unit->client = -1;
        }
        break;
    default:
        break;
    }
}

/* FS_IPC_READDIR_REQ: list the client's working directory.
 *
 * The two legs of this opcode carry different things, and deliberately so. From
 * a CLIENT it names no path: fs-manager owns the working directory, so a client
 * that supplied one could name a directory it is not standing in, and would need
 * to know a path fs-manager is the authority on. To the BACKEND it carries the
 * mount-relative path in fs-manager's own buffer, exactly like every other path
 * op.
 *
 * That is what keeps a backend STATELESS about position. It previously listed
 * "whichever directory it currently holds", so fs-manager had to send a CHDIR
 * first to make that the right one -- a second round trip per listing, and a
 * correctness argument that only held because this service handles one request
 * at a time. A path in the request needs neither.
 *
 * Entries come back as FS_IPC_STREAM frames, which fsmgr_reply_drain relays to
 * the client as they arrive; the terminating FS_IPC_RESP completes the record
 * (fsmgr_relay_done). */
static void handle_readdir_req(fs_client_state_t* state, int32_t source, int32_t request_id) {
    char tail[FSMGR_CWD_MAX];
    int32_t tail_len = 0;
    int32_t backend = -1;
    int32_t borrow;
    fsmgr_pending_t* entry;

    if (!route_absolute_path(state->cwd, tail, (int32_t)sizeof(tail), &tail_len, &backend)) {
        send_fs_error(source, request_id, WASMOS_ERR_FS_NO_BACKEND);
        return;
    }
    entry = pending_for_client(FSMGR_STEP_READDIR, backend, source, request_id, state->context_id);
    if (!entry) {
        return;
    }
    borrow = pending_lend_path(entry, tail, &tail_len);
    if (borrow < 0) {
        pending_abort(entry, (wasmos_error_code_t)borrow);
        return;
    }
    if (pending_send(entry, FS_IPC_READDIR_REQ, tail_len, 0, entry->buffer, borrow) != 0) {
        pending_abort(entry, WASMOS_ERR_FS_BACKEND_IPC);
    }
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
 * The backend is the supervisor's to run: fs-manager submits it as a unit
 * (fsmgr_mount_submit), so a backend that dies is restarted and its mount comes
 * back when the successor registers. The reply is DEFERRED until the backend has
 * registered (fsmgr_mount_pending_complete), and bounded by
 * FSMGR_MOUNT_TIMEOUT_MS (fsmgr_mount_pending_expire). The mount POINT needs no
 * work: fsmgr_ensure_mount_points runs on every registration and creates it in
 * whichever filesystem covers it. */
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
    g_pending_mount.unit = 0;
    g_pending_mount.awaiting_backend = 0;
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

    /* A mount that arrives before the supervisor's lookup is answered is
     * submitted by that answer (fsmgr_on_supervisor_lookup). */
    if (g_supervisor_lookup_done) {
        fsmgr_mount_submit();
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
 * A mount fs-manager placed has a supervised unit (fsmgr_mount_submit). Its
 * backend ends itself after the quiesce, and the client is answered only once
 * that end has been seen and the unit retracted (fsmgr_units_retire), so the
 * path can be mounted again as soon as the answer arrives. */
static void fsmgr_unmount_done(fsmgr_pending_t* entry, int32_t type);

static void handle_unmount_req(fs_client_state_t* state, int32_t source, int32_t request_id,
                               int32_t path_len, int32_t buffer_id) {
    uint8_t requested[FSMGR_CWD_MAX];
    char target[FSMGR_CWD_MAX];
    char mount[FSMGR_MOUNT_PATH_MAX];
    fs_backend_t* victim = 0;
    wasmos_error_code_t busy;
    fsmgr_pending_t* entry;
    fsmgr_unit_t* unit;

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

    /* Claimed before anything changes, so a full table refuses the unmount
     * whole rather than dropping a mount it cannot quiesce. */
    entry = pending_for_client(
        FSMGR_STEP_UNMOUNT, victim->endpoint, source, request_id, state->context_id);
    if (!entry) {
        return;
    }
    /* The table entry goes first: no request routed after this one may reach
     * the backend being quiesced. The client is answered once the backend
     * confirms (fsmgr_unmount_done), and, for a supervised backend, once its
     * unit is retracted. */
    unit = fsmgr_unit_by_mount(g_units, FSMGR_UNIT_CAP, victim->mount_path);
    if (unit && unit->state == FSMGR_UNIT_HELD) {
        unit->state = FSMGR_UNIT_RETIRING;
        unit->backend = victim->endpoint;
        unit->quiesced = 0;
        unit->deadline_ms = fsmgr_now_ms() + FSMGR_RETIRE_TIMEOUT_MS;
    }
    victim->in_use = 0;
    fsmgr_forget_backend_in_clients(entry->backend);
    if (pending_send(entry, WASMOS_IPC_SHUTDOWN_REQ, WASMOS_SHUTDOWN_REASON_UNMOUNT, 0, 0, 0) !=
        0) {
        fsmgr_unmount_done(entry, FS_IPC_ERROR);
    }
}

/* The quiesce an unmount sent was answered, or its backend ended first.
 *
 * Either way the client is told the mount is gone, because it is: answering with
 * an error would describe a failure that did not happen. What the backend failed
 * to flush is the backend's loss to record, so it is reported, not returned. */
static void fsmgr_unmount_done(fsmgr_pending_t* entry, int32_t type) {
    int32_t client = entry->client;
    int32_t request_id = entry->client_request_id;
    int32_t backend = entry->backend;

    if (type != (int32_t)WASMOS_IPC_SHUTDOWN_DONE) {
        log_msg("[fs-manager] unmount: backend did not quiesce\n");
    }
    pending_end(entry);
    for (uint32_t i = 0; i < FSMGR_UNIT_CAP; ++i) {
        fsmgr_unit_t* unit = &g_units[i];
        if (unit->state == FSMGR_UNIT_RETIRING && !unit->quiesced && unit->backend == backend) {
            unit->quiesced = 1;
            unit->client = client;
            unit->client_request_id = request_id;
            unit->deadline_ms = fsmgr_now_ms() + FSMGR_RETIRE_TIMEOUT_MS;
            fsmgr_units_retire();
            return;
        }
    }
    if (client >= 0) {
        (void)reply_to_client(client, FSMGR_IPC_UNMOUNT_RESP, request_id, 0, 0, 0, 0);
    }
}

/* ---------------------------------------------------------- deferred replies */

/* The answer to a relayed request: a FORWARD, or a READDIR whose entries went to
 * the client as FS_IPC_STREAM frames ahead of it. An OPEN's answer allocates the
 * client fd, a CLOSE's releases it. */
static void fsmgr_relay_done(fsmgr_pending_t* entry, int32_t type, int32_t r0, int32_t r1,
                             int32_t r2, int32_t r3) {
    int32_t client = entry->client;
    int32_t request_id = entry->client_request_id;
    fs_client_state_t* state =
        entry->context >= 0 ? fsmgr_client_lookup(&g_clients, entry->context) : 0;

    if (entry->step == FSMGR_STEP_READDIR && type != FS_IPC_RESP) {
        pending_abort(entry, r0 < 0 ? (wasmos_error_code_t)r0 : WASMOS_ERR_FS_BACKEND_IPC);
        return;
    }
    if (entry->type == FS_IPC_OPEN_REQ && type == FS_IPC_RESP && r0 >= 0) {
        int32_t backend = entry->backend;
        int32_t backend_fd = r0;
        /* A client that ended meanwhile, or has no fd left, cannot hold the
         * file, so it is closed again on the backend. */
        if (!state || client < 0 || fsmgr_fd_alloc(state, backend, backend_fd, &r0) != 0) {
            pending_abort(entry, WASMOS_ERR_FS_NO_FD);
            pending_discard_close(backend, backend_fd);
            return;
        }
    }
    if (entry->type == FS_IPC_CLOSE_REQ && type == FS_IPC_RESP && r0 == 0 &&
        entry->client_fd >= 0 && state) {
        fsmgr_fd_release(state, entry->client_fd);
    }
    pending_end(entry);
    if (client >= 0) {
        (void)reply_to_client(client, type, request_id, r0, r1, r2, r3);
    }
}

/* A record whose backend will never answer it: the backend ended, or left its
 * class. Ends the record on every path, which is what lets
 * fsmgr_pending_fail_backend walk the table by repeated lookup. */
static void fsmgr_pending_fail(fsmgr_pending_t* entry, wasmos_error_code_t code) {
    switch ((fsmgr_step_t)entry->step) {
    case FSMGR_STEP_READ_PATH_CLOSE:
        /* The file was read, or was not, before the close went out. */
        fsmgr_read_path_finish(entry);
        return;
    case FSMGR_STEP_UNMOUNT:
        fsmgr_unmount_done(entry, FS_IPC_ERROR);
        return;
    default:
        pending_abort(entry, code);
        return;
    }
}

/* `backend` ended or left its class: fail every request it held. */
static void fsmgr_pending_fail_backend(int32_t backend) {
    fsmgr_pending_t* entry;
    if (backend < 0) {
        return;
    }
    while ((entry = fsmgr_pending_next_for_backend(&g_pending, backend)) != 0) {
        fsmgr_pending_fail(entry, WASMOS_ERR_FS_BACKEND_IPC);
    }
}

static void fsmgr_pending_complete(fsmgr_pending_t* entry, int32_t type, int32_t r0, int32_t r1,
                                   int32_t r2, int32_t r3) {
    switch ((fsmgr_step_t)entry->step) {
    case FSMGR_STEP_FORWARD:
    case FSMGR_STEP_READDIR:
        fsmgr_relay_done(entry, type, r0, r1, r2, r3);
        return;
    case FSMGR_STEP_READ_PATH_OPEN:
    case FSMGR_STEP_READ_PATH_READ:
    case FSMGR_STEP_READ_PATH_CLOSE:
        fsmgr_read_path_step(entry, type, r0);
        return;
    case FSMGR_STEP_CHDIR_STAT:
        fsmgr_chdir_done(entry, type, r0, r1);
        return;
    case FSMGR_STEP_BACKEND_INFO:
        fsmgr_backend_info_done(entry, type, r0, r1, r2, r3);
        return;
    case FSMGR_STEP_UNMOUNT:
        fsmgr_unmount_done(entry, type);
        return;
    case FSMGR_STEP_DISCARD_CLOSE:
    case FSMGR_STEP_MKDIR:
    default:
        pending_end(entry);
        return;
    }
}

/* Take every answer off the reply endpoint and complete the record it belongs to.
 *
 * FS_IPC_STREAM frames are relayed to the record's client as they arrive and
 * leave the record in flight; the terminating answer completes it. An answer no
 * record claims belongs to a request already failed -- its backend left its
 * class first -- and is reported, never expected. */
static void fsmgr_reply_drain(void) {
    while (wasmos_ipc_drain(g_reply_endpoint) > 0) {
        int32_t type = wasmos_ipc_last_field(WASMOS_IPC_FIELD_TYPE);
        int32_t id = wasmos_ipc_last_field(WASMOS_IPC_FIELD_REQUEST_ID);
        int32_t a0 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG0);
        int32_t a1 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG1);
        int32_t a2 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG2);
        int32_t a3 = wasmos_ipc_last_field(WASMOS_IPC_FIELD_ARG3);
        fsmgr_pending_t* entry;

        /* A backend ended. The kernel tells every endpoint it had sent to, and
         * it sent its answers here; arg0 is the endpoint that ended. */
        if (type == WASMOS_IPC_HANGUP) {
            fsmgr_pending_fail_backend(a0);
            continue;
        }
        entry = fsmgr_pending_find(&g_pending, id);
        if (!entry) {
            printf("[fs-manager] dropped an answer nothing awaits: type=0x%x id=%d\n",
                   (unsigned)type,
                   (int)id);
            continue;
        }
        if (type == FS_IPC_STREAM) {
            /* Retried for the same reason as reply_to_client. A client still
             * full after every retry is stranded, and the rest of its answer
             * goes nowhere. */
            if (entry->client >= 0 && wasmos_sys_ipc_send_retry(entry->client,
                                                                g_fs_endpoint,
                                                                type,
                                                                entry->client_request_id,
                                                                a0,
                                                                a1,
                                                                a2,
                                                                a3,
                                                                FSMGR_REPLY_SEND_RETRIES) != 0) {
                entry->client = -1;
                entry->context = -1;
            }
            continue;
        }
        fsmgr_pending_complete(entry, type, a0, a1, a2, a3);
    }
}

/* Service entry point.  Brings up the bump heap and the endpoints (a service
 * endpoint published as "fs.vfs", a reply endpoint every request to a backend is
 * sent from, and the release endpoint), registers the service name, signals
 * readiness, then discovers the FS backends and serves requests forever.
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
 * wasmos_ipc_select_wait rather than spinning.  Client identity is the owning
 * context of the sending endpoint, not the endpoint itself, so all of a
 * process's endpoints share one cwd.
 *
 * The loop never waits for a backend. A request that needs one is recorded in
 * g_pending and sent, and the answer, drained from the reply endpoint, completes
 * it; a backend that holds a request holds only that request. fs-manager is below
 * everything that needs a file -- the process manager reads every module it
 * spawns through it -- so a wait here is a wait for whoever needs it to answer
 * (docs/architecture/18-filesystem-stack.md, "Deferred Replies").
 *
 * Does not return: a failure to create an endpoint or the select set, or to
 * register the name, parks the process in wasmos_sys_ipc_recv_loop instead of exiting, so the
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
    g_release_endpoint = wasmos_ipc_create_endpoint();
    log_msg("[fs-manager] init start\n");
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0 || g_fs_endpoint < 0 ||
        g_release_endpoint < 0) {
        log_msg("[fs-manager] endpoint init failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    g_main_select = wasmos_ipc_select_create();
    if (g_main_select <= 0 || wasmos_ipc_select_add(g_main_select, g_fs_endpoint) != 0 ||
        wasmos_ipc_select_add(g_main_select, g_reply_endpoint) != 0 ||
        wasmos_ipc_select_add(g_main_select, g_release_endpoint) != 0) {
        log_msg("[fs-manager] select set init failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    if (wasmos_svc_register(g_proc_endpoint, g_fs_endpoint, "fs.vfs", 1) != 0) {
        log_msg("[fs-manager] register fs.vfs failed\n");
        wasmos_sys_ipc_recv_loop();
    }
    log_msg("[fs-manager] services registered\n");
    wasmos_sys_notify_ready(g_proc_endpoint, g_fs_endpoint);
    fsmgr_discover_backends();
    fsmgr_lookup_supervisor();

    for (;;) {
        /* Answers first: draining them is what completes the records in flight
         * and lets the pump send the next closes, and each drain reads into the
         * same last-message slot the request below is read from. */
        fsmgr_release_drain();
        fsmgr_reply_drain();
        int release_retry = fsmgr_release_pump();
        /* Every turn, because what they wait for -- a backend's end, a deadline
         * -- is observed rather than announced to them. */
        fsmgr_units_retire();
        fsmgr_mount_pending_expire();
        /* Park until any endpoint has something; a message that arrived since
         * the drains wakes the wait at once. A close still to be resent, and a
         * mount or unmount deadline, bound the wait, since nothing would wake
         * it for either. */
        if (wasmos_ipc_drain(g_fs_endpoint) <= 0) {
            int32_t wait_ms = fsmgr_next_deadline_ms();
            if (release_retry && (wait_ms < 0 || wait_ms > FSMGR_RELEASE_RETRY_MS)) {
                wait_ms = FSMGR_RELEASE_RETRY_MS;
            }
            if (wait_ms >= 0) {
                (void)wasmos_ipc_select_wait_timeout(g_main_select, wait_ms > 0 ? wait_ms : 1);
            } else {
                (void)wasmos_ipc_select_wait(g_main_select);
            }
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

        /* Answers to what fs-manager asked from this endpoint: the supervisor's
         * lookup, and a unit's SUBMIT or RETRACT. */
        if (type == SVC_IPC_LOOKUP_RESP && source == g_proc_endpoint &&
            request_id == FSMGR_SV_LOOKUP_REQUEST_ID) {
            fsmgr_on_supervisor_lookup(arg0);
            continue;
        }
        if ((type == SUPERVISOR_IPC_RESP || type == SUPERVISOR_IPC_ERROR) &&
            g_supervisor_endpoint >= 0 && source == g_supervisor_endpoint) {
            fsmgr_on_supervisor_reply(type, request_id, arg0, arg1f);
            continue;
        }

        /* A client that had sent to this endpoint is gone: release what was
         * held for it. arg0 is the dead endpoint, arg1 the context that owned it,
         * and the context is what client state is keyed by.
         *
         * No subscription: the kernel delivers this to the endpoints the dead one
         * had actually sent to, so the client's own requests are what registered
         * it for notification. */
        if (type == WASMOS_IPC_HANGUP) {
            fsmgr_mount_waits_drop(arg0);
            (void)fsmgr_pending_orphan_client(&g_pending, arg0);
            fsmgr_release_client(arg1f);
            continue;
        }

        if (type == SVC_IPC_CLASS_EVENT) {
            /* Existence event for FSMGR_BACKEND_CLASS: arg0=event, arg1=instance,
             * arg2=provider endpoint, arg3=pid. */
            if (arg0 == (int32_t)SVC_CLASS_EVENT_ADD) {
                fsmgr_pull_backend(arg2f, arg3f);
            } else if (arg0 == (int32_t)SVC_CLASS_EVENT_REMOVE) {
                fsmgr_backend_remove(arg2f);
            }
            continue;
        }

        if (type == FSMGR_IPC_CLONE_CWD_REQ) {
            (void)handle_clone_cwd_req(source, source_owner, request_id, arg0, arg1f);
            continue;
        }
        if (type == FSMGR_IPC_MOUNT_WAIT_REQ) {
            handle_mount_wait_req(source, request_id, arg0, arg1f, arg2f);
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

        if (type == FS_IPC_READDIR_REQ) {
            handle_readdir_req(state, source, request_id);
            continue;
        }

        /* CHDIR is resolved entirely here: fs-manager owns the working
         * directory, so nothing about it is decided from a backend's reply. */
        if (type == FS_IPC_CHDIR_REQ) {
            (void)handle_chdir_mount(state, source, request_id, arg0, arg2f);
            continue;
        }

        int32_t req_arg0 = arg0;
        int32_t backend = resolve_backend_for_state(state);
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
        int32_t fwd_arg3 = arg3f;
        fsmgr_pending_t* entry;
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
        /* Relayed, not waited for: the answer completes the record
         * (fsmgr_relay_done), and this loop goes on serving meanwhile. */
        entry =
            pending_for_client(FSMGR_STEP_FORWARD, backend, source, request_id, state->context_id);
        if (!entry) {
            continue;
        }
        entry->type = type;
        entry->client_fd = client_fd;
        if (uses_buf) {
            entry->borrow = wasmos_xfer_buffer_reborrow(
                backend, client_borrow, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
            if (entry->borrow < 0) {
                entry->borrow = -1;
                pending_abort(entry, WASMOS_ERR_FS_REBORROW);
                continue;
            }
            fwd_arg3 = entry->borrow;
        }
        if (pending_send(entry, type, req_arg0, arg1f, arg2f, fwd_arg3) != 0) {
            pending_abort(entry, WASMOS_ERR_FS_BACKEND_IPC);
        }
    }
}
