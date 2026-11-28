/* keyring - the system's trusted package-signing keys.
 *
 * The keys live as PEM "PUBLIC KEY" files in KEYRING_DIR: the build installs
 * the system key (system.pem) and its app key (apps.pem) there, and a publisher's
 * key is trusted by placing its PEM file beside them. At start the service lists
 * the directory, reads every *.pem file, and registers each Ed25519 or RSA key
 * with the kernel (keyring_register_key, which needs keyring.register; the
 * kernel accepts RSA moduli of 2048 to 4096 bits): a package signed
 * with one then starts as a publisher's (docs/architecture/10, "Signed
 * Packages"). It then reads the allowlist, KEYRING_DIR/allow.lst, and registers
 * each package digest on it (keyring_allow_package): a package whose signature
 * no trusted key verifies starts anyway when its digest is listed, from bytecode
 * with ipc.basic only. Only after that does it register its service name and
 * report ready, so `wait-svc keyring` in sysinit means the keys are in force.
 *
 * Once ready it serves KEYRING_IPC_ALLOW_REQ and KEYRING_IPC_ADD_KEY_REQ from a
 * sender holding keyring.admin (the trust utility). A request is authorized, its
 * payload copied and the change registered with the kernel as it arrives; the
 * answer is deferred until the change is written to KEYRING_DIR (allow.lst, or
 * the key's own PEM file), so an answered request survives a restart.
 *
 * Every filesystem and process-manager exchange is a future on the async
 * runtime's loop, never a blocking call. The directory listing is the one
 * exception to a plain future: fs-manager streams it as FS_IPC_STREAM frames
 * that carry the request's id and ends it with an FS_IPC_RESP, and a future
 * settles on the first message with its id. So READDIR is sent as a plain
 * message under KEYRING_LIST_REQ_ID, outside the loop's request ids, and the
 * frames and the final reply reach type handlers that collect them. */
#include <stdint.h>

#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/service_runtime_wasm.h"
#include "wasmos/startup.h"

#include "keyring_pem.h"

#define KEYRING_DIR "/boot/system/keys"
#define KEYRING_LISTING_MAX 2048u
#define KEYRING_PEM_MAX 4096
#define KEYRING_DER_MAX 1024u
#define KEYRING_NAME_MAX 64u
#define KEYRING_ALLOW_FILE KEYRING_DIR "/allow.lst"
/* The allowlist read at boot: as many lines as the kernel holds digests. */
#define KEYRING_ALLOW_MAX (64u * KEYRING_ALLOW_LINE_MAX)
/* One FS read of the allowlist. */
#define KEYRING_READ_CHUNK 4096u
/* Requests taken but not yet answered: a request beyond these is answered BUSY. */
#define KEYRING_QUEUE 4u
/* The listing's request id: above anything the loop hands its futures. */
#define KEYRING_LIST_REQ_ID 0x4B520001

typedef enum {
    KEYRING_START = 0,
    KEYRING_CHDIRING,
    KEYRING_LISTING,
    KEYRING_NEXT,
    KEYRING_OPENING,
    KEYRING_READING,
    KEYRING_CLOSING,
    KEYRING_ALLOW_OPENING,
    KEYRING_ALLOW_READING,
    KEYRING_ALLOW_CLOSING,
    KEYRING_REGISTERING,
    KEYRING_READYING,
    KEYRING_IDLE,
    KEYRING_SAVE_OPENING,
    KEYRING_SAVE_WRITING,
    KEYRING_SAVE_CLOSING,
} keyring_state_t;

/* A request whose change is in force and is still to be written down, then
 * answered: `data` is written to `path` (appended, or replacing the file). */
typedef struct {
    int32_t source;
    int32_t request_id;
    int32_t flags;
    int32_t len;
    char path[sizeof(KEYRING_DIR) + 16u];
    char data[KEYRING_PEM_MAX];
} keyring_job_t;

typedef struct {
    keyring_state_t state;
    int32_t fs_endpoint;
    int32_t fd;
    int32_t staged; /* transfer buffer of the CHDIR path or the service descriptor */
    uint32_t pos;   /* position in the listing */
    uint32_t listing_len;
    uint32_t trusted;
    int32_t pem_len;
    char name[KEYRING_NAME_MAX];
    char path[sizeof(KEYRING_DIR) + KEYRING_NAME_MAX];
    char listing[KEYRING_LISTING_MAX];
    char pem[KEYRING_PEM_MAX];
    uint8_t der[KEYRING_DER_MAX];
    char allow[KEYRING_ALLOW_MAX];
    uint32_t allow_len;
    keyring_job_t jobs[KEYRING_QUEUE];
    uint32_t job_head;
    uint32_t job_count;
    int32_t written;    /* bytes of the job at the head written so far */
    int32_t job_status; /* the head job's answer once written: 0 or PERSIST */
    int idle_armed;
    wasmos_sys_wasm_fs_request_t chdir;
    wasmos_sys_wasm_fs_operation_t op;
    wasmos_sys_wasm_ipc_future_t ipc;
    wasmos_future_t* pending;
    wasmos_future_t listed;
    wasmos_promise_t listed_promise;
    wasmos_future_t idle;
    wasmos_promise_t idle_promise;
} keyring_t;

static keyring_t g_keyring;

static int32_t keyring_fail(const char* what, int32_t rc) {
    printf("[keyring] %s (%d); no publisher key is trusted\n", what, (int)rc);
    return rc < 0 ? rc : -1;
}

/* One streamed listing frame: four bytes in arg0..arg3, a zero ends it. */
static void keyring_on_stream(void* user, const wasmos_ipc_message_t* msg) {
    keyring_t* kr = (keyring_t*)user;
    if (msg->request_id != KEYRING_LIST_REQ_ID) {
        return;
    }
    const int32_t args[4] = {msg->arg0, msg->arg1, msg->arg2, msg->arg3};
    for (uint32_t i = 0; i < 4u; ++i) {
        char c = (char)(args[i] & 0xFF);
        if (c == '\0') {
            return;
        }
        if (kr->listing_len < KEYRING_LISTING_MAX) {
            kr->listing[kr->listing_len++] = c;
        }
    }
}

/* The listing's final reply; any other FS_IPC_RESP belongs to a future and never
 * gets here (the loop settles futures first). */
static void keyring_on_resp(void* user, const wasmos_ipc_message_t* msg) {
    keyring_t* kr = (keyring_t*)user;
    if (msg->request_id == KEYRING_LIST_REQ_ID) {
        (void)wasmos_promise_resolve(&kr->listed_promise, (uintptr_t)(uint32_t)msg->arg0);
    }
}

static void keyring_hex(const uint8_t* bytes, uint32_t len, char* out) {
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < len; ++i) {
        out[2u * i] = digits[bytes[i] >> 4];
        out[2u * i + 1u] = digits[bytes[i] & 15u];
    }
    out[2u * len] = '\0';
}

/* Register the key just read, or say why not. The file's name ends each line:
 * console output is test input, and a utility's own errors start with
 * "<name>:". */
static void keyring_take_key(keyring_t* kr) {
    int32_t der_len = keyring_pem_to_der(kr->pem, (uint32_t)kr->pem_len, kr->der, KEYRING_DER_MAX);
    if (der_len < 0) {
        printf("[keyring] skipped, not a PEM public key: %s\n", kr->name);
        return;
    }
    int kind = keyring_der_kind(kr->der, (uint32_t)der_len);
    if (kind == KEYRING_KEY_UNKNOWN) {
        printf("[keyring] skipped, neither an Ed25519 nor an RSA key: %s\n", kr->name);
        return;
    }
    const char* alg = kind == KEYRING_KEY_RSA ? "RSA" : "Ed25519";
    uint8_t id[KEYRING_KEY_ID_LEN];
    char hex[2u * KEYRING_KEY_ID_LEN + 1u];
    keyring_key_id(kr->der, (uint32_t)der_len, id);
    keyring_hex(id, KEYRING_KEY_ID_LEN, hex);
    int32_t rc = wasmos_keyring_register_key((int32_t)(uintptr_t)kr->der, der_len);
    if (rc != 0) {
        printf("[keyring] the kernel refused %s key %s (%d): %s\n", alg, hex, (int)rc, kr->name);
        return;
    }
    kr->trusted++;
    printf("[keyring] trusted %s key %s: %s\n", alg, hex, kr->name);
}

/* Register every digest in the allowlist read at boot. */
static void keyring_take_allowlist(keyring_t* kr) {
    uint8_t digest[KEYRING_DIGEST_LEN];
    uint32_t pos = 0;
    uint32_t allowed = 0;
    int rc;
    while ((rc = keyring_allow_next(kr->allow, kr->allow_len, &pos, digest)) != 0) {
        if (rc < 0) {
            printf("[keyring] skipped a line that holds no digest: allow.lst\n");
            continue;
        }
        int32_t status =
            wasmos_keyring_allow_package((int32_t)(uintptr_t)digest, (int32_t)KEYRING_DIGEST_LEN);
        if (status != 0) {
            printf("[keyring] the kernel refused a digest (%d): allow.lst\n", (int)status);
            continue;
        }
        allowed++;
    }
    if (allowed > 0u) {
        printf("[keyring] %u package(s) allowed to start anyway: allow.lst\n", (unsigned)allowed);
    }
}

static void keyring_reply(int32_t destination, int32_t request_id, int32_t status) {
    (void)wasmos_ipc_send(destination,
                          wasmos_sys_wasm_async_reply_endpoint(),
                          status == 0 ? KEYRING_IPC_RESP : KEYRING_IPC_ERROR,
                          request_id,
                          status,
                          0,
                          0,
                          0);
}

/* KEYRING_IPC_ALLOW_REQ: register the digest and record its line in `job`. */
static int32_t keyring_take_allow(const wasmos_ipc_message_t* msg, keyring_job_t* job) {
    char payload[KEYRING_DIGEST_LEN + KEYRING_ALLOW_PATH_MAX];
    if (msg->arg2 < (int32_t)KEYRING_DIGEST_LEN || msg->arg2 > (int32_t)sizeof(payload) ||
        msg->arg1 < 0 || wasmos_xfer_buffer_read(msg->arg0, payload, msg->arg2, msg->arg1) != 0) {
        return WASMOS_ERR_KEYRING_BAD_REQUEST;
    }
    const uint8_t* digest = (const uint8_t*)payload;
    uint32_t path_len = (uint32_t)msg->arg2 - KEYRING_DIGEST_LEN;
    job->len = keyring_allow_line(
        digest, payload + KEYRING_DIGEST_LEN, path_len, job->data, (uint32_t)sizeof(job->data));
    if (job->len < 0) {
        return WASMOS_ERR_KEYRING_BAD_REQUEST;
    }
    int32_t rc =
        wasmos_keyring_allow_package((int32_t)(uintptr_t)digest, (int32_t)KEYRING_DIGEST_LEN);
    if (rc != 0) {
        return rc;
    }
    memcpy(job->path, KEYRING_ALLOW_FILE, sizeof(KEYRING_ALLOW_FILE));
    job->flags = O_WRONLY | O_CREAT | O_APPEND;
    /* The line as written: the digest's first bytes and the path, control
     * characters replaced. */
    char shown[KEYRING_ALLOW_PATH_MAX + 1u];
    uint32_t n = 0;
    for (int32_t i = 2 * (int32_t)KEYRING_DIGEST_LEN + 1; i < job->len - 1; ++i) {
        shown[n++] = job->data[i];
    }
    shown[n] = '\0';
    char hex[2u * KEYRING_KEY_ID_LEN + 1u];
    keyring_hex(digest, KEYRING_KEY_ID_LEN, hex);
    printf("[keyring] allowed package %s to start anyway: %s\n", hex, n ? shown : "?");
    return 0;
}

/* KEYRING_IPC_ADD_KEY_REQ: register the key and record its PEM file in `job`. */
static int32_t keyring_take_add_key(keyring_t* kr, const wasmos_ipc_message_t* msg,
                                    keyring_job_t* job) {
    if (msg->arg2 <= 0 || msg->arg2 > (int32_t)sizeof(job->data) || msg->arg1 < 0 ||
        wasmos_xfer_buffer_read(msg->arg0, job->data, msg->arg2, msg->arg1) != 0) {
        return WASMOS_ERR_KEYRING_BAD_REQUEST;
    }
    job->len = msg->arg2;
    int32_t der_len =
        keyring_pem_to_der(job->data, (uint32_t)job->len, kr->der, (uint32_t)sizeof(kr->der));
    int kind = der_len > 0 ? keyring_der_kind(kr->der, (uint32_t)der_len) : KEYRING_KEY_UNKNOWN;
    if (kind == KEYRING_KEY_UNKNOWN) {
        return WASMOS_ERR_KEYRING_NOT_A_KEY;
    }
    int32_t rc = wasmos_keyring_register_key((int32_t)(uintptr_t)kr->der, der_len);
    if (rc != 0) {
        return rc;
    }
    uint8_t id[KEYRING_KEY_ID_LEN];
    char name[13];
    char hex[2u * KEYRING_KEY_ID_LEN + 1u];
    keyring_key_id(kr->der, (uint32_t)der_len, id);
    keyring_key_file_name(id, name);
    keyring_hex(id, KEYRING_KEY_ID_LEN, hex);
    snprintf(job->path, sizeof(job->path), "%s/%s", KEYRING_DIR, name);
    job->flags = O_WRONLY | O_CREAT | O_TRUNC;
    kr->trusted++;
    printf("[keyring] trusted %s key %s: %s\n",
           kind == KEYRING_KEY_RSA ? "RSA" : "Ed25519",
           hex,
           name);
    return 0;
}

/* A change request: answered at once when it is refused; otherwise queued to be
 * written down and answered by the task, which is woken. */
static void keyring_on_request(void* user, const wasmos_ipc_message_t* msg) {
    keyring_t* kr = (keyring_t*)user;
    int32_t rc;
    if (kr->job_count == KEYRING_QUEUE) {
        rc = WASMOS_ERR_KEYRING_BUSY;
    } else if (wasmos_ipc_endpoint_has_cap(msg->source, WASMOS_CAP_KEYRING_ADMIN) != 1) {
        rc = WASMOS_ERR_KEYRING_NOT_AUTHORIZED;
    } else {
        keyring_job_t* job = &kr->jobs[(kr->job_head + kr->job_count) % KEYRING_QUEUE];
        rc = msg->type == KEYRING_IPC_ALLOW_REQ ? keyring_take_allow(msg, job)
                                                : keyring_take_add_key(kr, msg, job);
        if (rc == 0) {
            job->source = msg->source;
            job->request_id = msg->request_id;
            kr->job_count++;
            if (kr->idle_armed) {
                (void)wasmos_promise_resolve(&kr->idle_promise, 0);
            }
            return;
        }
    }
    keyring_reply(msg->source, msg->request_id, rc);
}

/* Stage `len` bytes for `endpoint` to read and write; the buffer id, or -1. */
static int32_t keyring_stage(const void* data, int32_t len, int32_t endpoint, int32_t* borrow) {
    int32_t bid = wasmos_xfer_buffer_acquire(len);
    if (bid < 0) {
        return -1;
    }
    if (wasmos_xfer_buffer_write(bid, data, len, 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    *borrow = wasmos_xfer_buffer_borrow(endpoint, bid, 3 /* READ|WRITE */);
    if (*borrow < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return -1;
    }
    return bid;
}

static int32_t keyring_resume(void* user, uintptr_t* out_value) {
    keyring_t* kr = (keyring_t*)user;
    wasmos_sys_event_loop_t* loop = wasmos_sys_wasm_async_event_loop();
    int32_t reply_ep = wasmos_sys_wasm_async_reply_endpoint();
    uintptr_t value = 0;
    int32_t rc = 0;
    int32_t got = 0; /* bytes an FS read or write reported */
    (void)out_value;

    for (;;) {
        switch (kr->state) {
        case KEYRING_START: {
            kr->fs_endpoint = wasmos_fs_endpoint();
            if (kr->fs_endpoint < 0) {
                return keyring_fail("no filesystem service", kr->fs_endpoint);
            }
            if (wasmos_sys_event_register(loop, FS_IPC_STREAM, keyring_on_stream, kr) != 0 ||
                wasmos_sys_event_register(loop, FS_IPC_RESP, keyring_on_resp, kr) != 0 ||
                wasmos_sys_event_register(loop, KEYRING_IPC_ALLOW_REQ, keyring_on_request, kr) !=
                    0 ||
                wasmos_sys_event_register(loop, KEYRING_IPC_ADD_KEY_REQ, keyring_on_request, kr) !=
                    0) {
                return keyring_fail("cannot handle the directory listing", -1);
            }
            /* fs-manager lists the client's working directory. */
            static const char dir[] = KEYRING_DIR;
            int32_t borrow = 0;
            kr->staged = keyring_stage(dir, (int32_t)sizeof(dir), kr->fs_endpoint, &borrow);
            if (kr->staged < 0) {
                return keyring_fail("cannot stage the directory path", -1);
            }
            wasmos_sys_wasm_fs_request_init(&kr->chdir);
            kr->pending = wasmos_sys_wasm_fs_request_send(loop,
                                                          &kr->chdir,
                                                          kr->fs_endpoint,
                                                          reply_ep,
                                                          FS_IPC_CHDIR_REQ,
                                                          (int32_t)(sizeof(dir) - 1u),
                                                          0,
                                                          kr->staged,
                                                          borrow,
                                                          0);
            if (!kr->pending) {
                (void)wasmos_xfer_buffer_release(kr->staged);
                return keyring_fail("cannot start the chdir", -1);
            }
            kr->state = KEYRING_CHDIRING;
            break;
        }
        case KEYRING_CHDIRING:
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            (void)wasmos_xfer_buffer_release(kr->staged);
            if (rc != 0 || wasmos_sys_wasm_fs_request_reply(&kr->chdir)->arg0 < 0) {
                return keyring_fail("no " KEYRING_DIR " directory", rc);
            }
            kr->listing_len = 0;
            wasmos_future_init(&kr->listed, &kr->listed_promise);
            if (wasmos_ipc_send(kr->fs_endpoint,
                                reply_ep,
                                FS_IPC_READDIR_REQ,
                                KEYRING_LIST_REQ_ID,
                                0,
                                0,
                                0,
                                0) != 0) {
                return keyring_fail("cannot list " KEYRING_DIR, -1);
            }
            kr->state = KEYRING_LISTING;
            break;
        case KEYRING_LISTING:
            rc = wasmos_future_await(&kr->listed, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            if (rc != 0 || (int32_t)value != 0) {
                return keyring_fail("cannot list " KEYRING_DIR, rc != 0 ? rc : (int32_t)value);
            }
            kr->pos = 0;
            kr->state = KEYRING_NEXT;
            break;
        case KEYRING_NEXT:
            if (!keyring_listing_next(
                    kr->listing, kr->listing_len, &kr->pos, kr->name, KEYRING_NAME_MAX)) {
                /* An absent allowlist is the usual case: nothing is allowed. */
                kr->allow_len = 0;
                kr->pending = wasmos_sys_wasm_fs_open_async(
                    loop, &kr->op, kr->fs_endpoint, reply_ep, KEYRING_ALLOW_FILE, O_RDONLY, 0);
                kr->state = kr->pending ? KEYRING_ALLOW_OPENING : KEYRING_REGISTERING;
                break;
            }
            snprintf(kr->path, sizeof(kr->path), "%s/%s", KEYRING_DIR, kr->name);
            kr->pending = wasmos_sys_wasm_fs_open_async(
                loop, &kr->op, kr->fs_endpoint, reply_ep, kr->path, O_RDONLY, 0);
            if (!kr->pending) {
                printf("[keyring] skipped, cannot open: %s\n", kr->name);
                break;
            }
            kr->state = KEYRING_OPENING;
            break;
        case KEYRING_OPENING:
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            kr->fd = wasmos_sys_wasm_fs_operation_finish(&kr->op, 0, 0, 0);
            if (rc != 0 || kr->fd < 0) {
                printf("[keyring] skipped, cannot open: %s\n", kr->name);
                kr->state = KEYRING_NEXT;
                break;
            }
            kr->pending = wasmos_sys_wasm_fs_read_async(
                loop, &kr->op, kr->fs_endpoint, reply_ep, kr->fd, kr->pem, KEYRING_PEM_MAX, 0);
            kr->state = kr->pending ? KEYRING_READING : KEYRING_CLOSING;
            kr->pem_len = -1;
            break;
        case KEYRING_READING:
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            kr->pem_len = wasmos_sys_wasm_fs_operation_finish(&kr->op, kr->pem, KEYRING_PEM_MAX, 0);
            if (rc != 0) {
                kr->pem_len = -1;
            }
            kr->pending =
                wasmos_sys_wasm_fs_close_async(loop, &kr->op, kr->fs_endpoint, reply_ep, kr->fd, 0);
            kr->state = KEYRING_CLOSING;
            break;
        case KEYRING_CLOSING:
            if (kr->pending) {
                rc = wasmos_future_await(kr->pending, &value);
                if (rc == WASMOS_WASM_AWAIT_PENDING) {
                    return WASMOS_WASM_TASK_YIELDED;
                }
                (void)wasmos_sys_wasm_fs_operation_finish(&kr->op, 0, 0, 0);
            }
            if (kr->pem_len > 0) {
                keyring_take_key(kr);
            } else {
                printf("[keyring] skipped, cannot read: %s\n", kr->name);
            }
            kr->state = KEYRING_NEXT;
            break;
        case KEYRING_ALLOW_OPENING:
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            kr->fd = wasmos_sys_wasm_fs_operation_finish(&kr->op, 0, 0, 0);
            kr->pending = 0;
            if (rc != 0 || kr->fd < 0) {
                kr->state = KEYRING_REGISTERING;
                break;
            }
            kr->state = KEYRING_ALLOW_READING;
            break;
        case KEYRING_ALLOW_READING:
            if (!kr->pending) {
                uint32_t room = KEYRING_ALLOW_MAX - kr->allow_len;
                if (room > KEYRING_READ_CHUNK) {
                    room = KEYRING_READ_CHUNK;
                }
                kr->pending = room == 0u ? 0
                                         : wasmos_sys_wasm_fs_read_async(loop,
                                                                         &kr->op,
                                                                         kr->fs_endpoint,
                                                                         reply_ep,
                                                                         kr->fd,
                                                                         0,
                                                                         (int32_t)room,
                                                                         0);
                if (!kr->pending) {
                    kr->state = KEYRING_ALLOW_CLOSING;
                }
                break;
            }
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            got = wasmos_sys_wasm_fs_operation_finish(&kr->op,
                                                      kr->allow + kr->allow_len,
                                                      (int32_t)(KEYRING_ALLOW_MAX - kr->allow_len),
                                                      0);
            kr->pending = 0;
            if (rc != 0 || got <= 0) {
                kr->state = KEYRING_ALLOW_CLOSING;
            } else {
                kr->allow_len += (uint32_t)got;
            }
            break;
        case KEYRING_ALLOW_CLOSING:
            if (!kr->pending) {
                kr->pending = wasmos_sys_wasm_fs_close_async(
                    loop, &kr->op, kr->fs_endpoint, reply_ep, kr->fd, 0);
                if (!kr->pending) {
                    keyring_take_allowlist(kr);
                    kr->state = KEYRING_REGISTERING;
                }
                break;
            }
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            (void)wasmos_sys_wasm_fs_operation_finish(&kr->op, 0, 0, 0);
            kr->pending = 0;
            keyring_take_allowlist(kr);
            kr->state = KEYRING_REGISTERING;
            break;
        case KEYRING_REGISTERING:
            if (!kr->pending) {
                /* The keys are in force: announce the service, as sysinit's
                 * `wait-svc keyring` waits for. */
                svc_register_desc_t desc;
                memset(&desc, 0, sizeof(desc));
                desc.version = WASMOS_SVC_REGISTER_DESC_VERSION;
                desc.service_endpoint = (uint32_t)reply_ep;
                memcpy(desc.name, "keyring", sizeof("keyring"));
                kr->staged = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
                if (kr->staged < 0) {
                    return keyring_fail("cannot stage the service descriptor", -1);
                }
                wasmos_sys_wasm_ipc_future_init(&kr->ipc, 0, 0);
                kr->pending = wasmos_sys_wasm_ipc_future_send(loop,
                                                              &kr->ipc,
                                                              wasmos_startup_proc_endpoint(),
                                                              reply_ep,
                                                              SVC_IPC_REGISTER_DESC_REQ,
                                                              0,
                                                              (int32_t)sizeof(desc),
                                                              kr->staged,
                                                              0,
                                                              0);
                if (!kr->pending) {
                    (void)wasmos_xfer_buffer_release(kr->staged);
                    return keyring_fail("cannot register the service", -1);
                }
                break;
            }
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            (void)wasmos_xfer_buffer_release(kr->staged);
            if (rc != 0 ||
                wasmos_sys_wasm_ipc_future_reply(&kr->ipc)->type != SVC_IPC_REGISTER_RESP) {
                return keyring_fail("the process manager refused the service name", rc);
            }
            printf("[keyring] ready, %u key(s) registered\n", (unsigned)kr->trusted);
            wasmos_sys_wasm_ipc_future_init(&kr->ipc, 0, 0);
            kr->pending = wasmos_sys_wasm_ipc_future_send(loop,
                                                          &kr->ipc,
                                                          wasmos_startup_proc_endpoint(),
                                                          reply_ep,
                                                          PROC_IPC_NOTIFY_READY,
                                                          0,
                                                          0,
                                                          0,
                                                          0,
                                                          0);
            if (!kr->pending) {
                return keyring_fail("cannot report ready", -1);
            }
            kr->state = KEYRING_READYING;
            break;
        case KEYRING_READYING:
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            kr->state = KEYRING_IDLE;
            break;
        case KEYRING_IDLE:
            /* The keys stay registered for as long as the process lives; it
             * parks until a request is queued. */
            if (kr->job_count > 0u) {
                kr->written = 0;
                kr->job_status = 0;
                kr->state = KEYRING_SAVE_OPENING;
                kr->pending = 0;
                break;
            }
            if (!kr->idle_armed) {
                wasmos_future_init(&kr->idle, &kr->idle_promise);
                kr->idle_armed = 1;
            }
            rc = wasmos_future_await(&kr->idle, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            kr->idle_armed = 0;
            break;
        case KEYRING_SAVE_OPENING: {
            keyring_job_t* job = &kr->jobs[kr->job_head];
            if (!kr->pending) {
                kr->pending = wasmos_sys_wasm_fs_open_async(
                    loop, &kr->op, kr->fs_endpoint, reply_ep, job->path, job->flags, 0);
                if (!kr->pending) {
                    kr->fd = -1;
                    kr->job_status = WASMOS_ERR_KEYRING_PERSIST;
                    kr->state = KEYRING_SAVE_CLOSING;
                }
                break;
            }
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            kr->fd = wasmos_sys_wasm_fs_operation_finish(&kr->op, 0, 0, 0);
            kr->pending = 0;
            if (rc != 0 || kr->fd < 0) {
                kr->fd = -1;
                kr->job_status = WASMOS_ERR_KEYRING_PERSIST;
                kr->state = KEYRING_SAVE_CLOSING;
                break;
            }
            kr->state = KEYRING_SAVE_WRITING;
            break;
        }
        case KEYRING_SAVE_WRITING: {
            keyring_job_t* job = &kr->jobs[kr->job_head];
            if (!kr->pending) {
                if (kr->written >= job->len) {
                    kr->state = KEYRING_SAVE_CLOSING;
                    break;
                }
                kr->pending = wasmos_sys_wasm_fs_write_async(loop,
                                                             &kr->op,
                                                             kr->fs_endpoint,
                                                             reply_ep,
                                                             kr->fd,
                                                             job->data + kr->written,
                                                             job->len - kr->written,
                                                             0);
                if (!kr->pending) {
                    kr->job_status = WASMOS_ERR_KEYRING_PERSIST;
                    kr->state = KEYRING_SAVE_CLOSING;
                }
                break;
            }
            rc = wasmos_future_await(kr->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            got = wasmos_sys_wasm_fs_operation_finish(&kr->op, 0, 0, 0);
            kr->pending = 0;
            if (rc != 0 || got <= 0) {
                kr->job_status = WASMOS_ERR_KEYRING_PERSIST;
                kr->state = KEYRING_SAVE_CLOSING;
                break;
            }
            kr->written += got;
            break;
        }
        case KEYRING_SAVE_CLOSING: {
            keyring_job_t* job = &kr->jobs[kr->job_head];
            if (!kr->pending && kr->fd >= 0) {
                kr->pending = wasmos_sys_wasm_fs_close_async(
                    loop, &kr->op, kr->fs_endpoint, reply_ep, kr->fd, 0);
                kr->fd = -1;
                if (kr->pending) {
                    break;
                }
                kr->job_status = WASMOS_ERR_KEYRING_PERSIST;
            } else if (kr->pending) {
                rc = wasmos_future_await(kr->pending, &value);
                if (rc == WASMOS_WASM_AWAIT_PENDING) {
                    return WASMOS_WASM_TASK_YIELDED;
                }
                if (rc != 0 || wasmos_sys_wasm_fs_operation_finish(&kr->op, 0, 0, 0) < 0) {
                    kr->job_status = WASMOS_ERR_KEYRING_PERSIST;
                }
                kr->pending = 0;
            }
            if (kr->job_status != 0) {
                printf("[keyring] in force until restart, could not be written: %s\n", job->path);
            }
            keyring_reply(job->source, job->request_id, kr->job_status);
            kr->job_head = (kr->job_head + 1u) % KEYRING_QUEUE;
            kr->job_count--;
            kr->state = KEYRING_IDLE;
            break;
        }
        }
    }
}

wasmos_sys_wasm_async_config_t wasmos_async_service = {
    .resume = keyring_resume,
    .user = &g_keyring,
};
