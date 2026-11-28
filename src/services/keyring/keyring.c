/* keyring - the system's trusted package-signing keys.
 *
 * The keys live as PEM "PUBLIC KEY" files in KEYRING_DIR: the build installs
 * the system key (system.pem) and its app key (apps.pem) there, and a publisher's
 * key is trusted by placing its PEM file beside them. At start the service lists
 * the directory, reads every *.pem file, and registers each Ed25519 key with the
 * kernel (keyring_register_key, which needs keyring.register): a package signed
 * with one then starts as a publisher's (docs/architecture/10, "Signed
 * Packages"). Only after that does it register its service name and report
 * ready, so `wait-svc keyring` in sysinit means the keys are in force.
 *
 * Every filesystem and process-manager exchange is a future on the async
 * runtime's loop, never a blocking call. The directory listing is the one
 * exception to a plain future: fs-manager streams it as FS_IPC_STREAM frames
 * that carry the request's id and ends it with an FS_IPC_RESP, and a future
 * settles on the first message with its id. So READDIR is sent as a plain
 * message under KEYRING_LIST_REQ_ID, outside the loop's request ids, and the
 * frames and the final reply reach type handlers that collect them.
 *
 * TODO: RSA keys are recognised but not used; the kernel verifies Ed25519 only,
 * and RSA-PSS signatures are to be verified here, with mbedTLS
 * (docs/TASKS.md). */
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
    KEYRING_REGISTERING,
    KEYRING_READYING,
    KEYRING_IDLE,
} keyring_state_t;

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
    if (kind != KEYRING_KEY_ED25519) {
        printf("[keyring] skipped, %s keys are not verified yet: %s\n",
               kind == KEYRING_KEY_RSA ? "RSA" : "unknown",
               kr->name);
        return;
    }
    uint8_t id[KEYRING_KEY_ID_LEN];
    char hex[2u * KEYRING_KEY_ID_LEN + 1u];
    keyring_key_id(kr->der, (uint32_t)der_len, id);
    keyring_hex(id, KEYRING_KEY_ID_LEN, hex);
    int32_t rc = wasmos_keyring_register_key((int32_t)(uintptr_t)kr->der, der_len);
    if (rc != 0) {
        printf("[keyring] the kernel refused Ed25519 key %s (%d): %s\n", hex, (int)rc, kr->name);
        return;
    }
    kr->trusted++;
    printf("[keyring] trusted Ed25519 key %s: %s\n", hex, kr->name);
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
    (void)out_value;

    for (;;) {
        switch (kr->state) {
        case KEYRING_START: {
            kr->fs_endpoint = wasmos_fs_endpoint();
            if (kr->fs_endpoint < 0) {
                return keyring_fail("no filesystem service", kr->fs_endpoint);
            }
            if (wasmos_sys_event_register(loop, FS_IPC_STREAM, keyring_on_stream, kr) != 0 ||
                wasmos_sys_event_register(loop, FS_IPC_RESP, keyring_on_resp, kr) != 0) {
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
                kr->state = KEYRING_REGISTERING;
                kr->pending = 0;
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
            /* Nothing to serve yet: park for good, keeping the keys registered
             * for as long as the process lives. */
            wasmos_future_init(&kr->idle, &kr->idle_promise);
            kr->state = KEYRING_IDLE;
            break;
        case KEYRING_IDLE:
            (void)wasmos_future_await(&kr->idle, &value);
            return WASMOS_WASM_TASK_YIELDED;
        }
    }
}

wasmos_sys_wasm_async_config_t wasmos_async_service = {
    .resume = keyring_resume,
    .user = &g_keyring,
};
