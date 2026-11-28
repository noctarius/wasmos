/* trust -- change what the keyring trusts.
 *
 * Usage: trust allow <package.wap>
 *        trust add-key <key.pem>
 *
 *   allow    Let a package start although its signature is verified by no
 *            trusted key -- it was altered, or its publisher is unknown here --
 *            as macOS and Windows let a user open an app from an unidentified
 *            developer. The package's SHA-512 digest is allowed, so exactly these
 *            bytes start, from bytecode and with ipc.basic only; any change to the
 *            package refuses it again.
 *   add-key  Trust a publisher's key (a PEM "PUBLIC KEY" file, Ed25519 or RSA of
 *            2048 to 4096 bits): every package it signs starts as a publisher's.
 *
 * Both are kept in /boot/system/keys by the keyring service, which answers only
 * once the change is written there, and both need keyring.admin, which the
 * console shell holds (docs/architecture/10, "Signed Packages").
 *
 * Every exchange -- the keyring lookup, the file reads and the request -- is a
 * future on one task: this tree adds no synchronous request/reply (AGENTS.md).
 * The payload travels in a transfer buffer this utility owns and lends READ to
 * the keyring for the one request. */
#include <stdint.h>

#include "fcntl.h"
#include "monocypher-ed25519.h"
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"

#define TRUST_DIGEST_LEN 64u
#define TRUST_PATH_MAX 128u
#define TRUST_PEM_MAX 4096u
#define TRUST_CHUNK 4096

static wasmos_wasm_runtime_t g_runtime;
static wasmos_sys_event_loop_t g_loop;
static int32_t g_reply_endpoint = -1;

enum {
    TRUST_PC_START = 0,
    TRUST_PC_LOOKED_UP,
    TRUST_PC_OPENED,
    TRUST_PC_READ,
    TRUST_PC_CLOSED,
    TRUST_PC_ANSWERED,
};

typedef struct {
    uint32_t pc;
    int add_key; /* add-key rather than allow */
    const char* path;
    uint32_t path_len;
    int32_t fs_endpoint;
    int32_t keyring;
    int32_t fd;
    int32_t buffer;
    uint32_t size; /* bytes read so far */
    int32_t read_status;
    crypto_sha512_ctx sha;
    uint8_t payload[TRUST_DIGEST_LEN + TRUST_PATH_MAX > TRUST_PEM_MAX
                        ? TRUST_DIGEST_LEN + TRUST_PATH_MAX
                        : TRUST_PEM_MAX];
    uint32_t payload_len;
    uint8_t chunk[TRUST_CHUNK];
    wasmos_sys_wasm_ipc_future_t ipc;
    wasmos_sys_wasm_fs_operation_t op;
    const char* failed; /* what failed, for the message */
} trust_ctx_t;

/* Await `future`, resuming this task at `next_pc`; a stackless task re-enters
 * from the top, so its state lives in the context (partprobe has the same
 * form). */
#define TRUST_AWAIT(ctx, future, next_pc)                                                          \
    do {                                                                                           \
        wasmos_future_t* _f = (future);                                                            \
        (ctx)->pc = (next_pc);                                                                     \
        if (_f) {                                                                                  \
            uintptr_t _v = 0;                                                                      \
            if (wasmos_future_await(_f, &_v) == WASMOS_WASM_AWAIT_PENDING) {                       \
                return WASMOS_WASM_TASK_YIELDED;                                                   \
            }                                                                                      \
        }                                                                                          \
    } while (0)

static int32_t trust_task(void* user, uintptr_t* out_value) {
    trust_ctx_t* c = (trust_ctx_t*)user;
    const wasmos_ipc_message_t* reply;
    int32_t n;

    if (out_value) {
        *out_value = 0;
    }
    switch (c->pc) {
    case TRUST_PC_START: {
        int32_t name[4];
        wasmos_ipc_pack_name16("keyring", name);
        wasmos_sys_wasm_ipc_future_init(&c->ipc, 0, 0);
        TRUST_AWAIT(c,
                    wasmos_sys_wasm_ipc_future_send(&g_loop,
                                                    &c->ipc,
                                                    wasmos_startup_proc_endpoint(),
                                                    g_reply_endpoint,
                                                    SVC_IPC_LOOKUP_REQ,
                                                    name[0],
                                                    name[1],
                                                    name[2],
                                                    name[3],
                                                    0),
                    TRUST_PC_LOOKED_UP);
    }
        /* fallthrough */
    case TRUST_PC_LOOKED_UP:
        reply = wasmos_sys_wasm_ipc_future_reply(&c->ipc);
        if (!reply || reply->type != SVC_IPC_LOOKUP_RESP || reply->arg0 < 0) {
            c->failed = "no keyring service is running";
            return WASMOS_ERR_KEYRING_BAD_REQUEST;
        }
        c->keyring = reply->arg0;
        TRUST_AWAIT(c,
                    wasmos_sys_wasm_fs_open_async(
                        &g_loop, &c->op, c->fs_endpoint, g_reply_endpoint, c->path, O_RDONLY, 0),
                    TRUST_PC_OPENED);
        /* fallthrough */
    case TRUST_PC_OPENED:
        c->fd = wasmos_sys_wasm_fs_operation_finish(&c->op, 0, 0, 0);
        if (c->fd < 0) {
            c->failed = "cannot open it";
            return c->fd;
        }
        crypto_sha512_init(&c->sha);
        c->size = 0;
        /* fallthrough */
    case TRUST_PC_READ:
        if (c->pc == TRUST_PC_READ) {
            n = wasmos_sys_wasm_fs_operation_finish(&c->op, c->chunk, TRUST_CHUNK, 0);
            if (n > 0 && c->add_key) {
                if (c->size + (uint32_t)n > TRUST_PEM_MAX) {
                    c->read_status = WASMOS_ERR_KEYRING_NOT_A_KEY;
                    n = 0;
                } else {
                    memcpy(c->payload + c->size, c->chunk, (uint32_t)n);
                }
            } else if (n > 0) {
                crypto_sha512_update(&c->sha, c->chunk, (size_t)n);
            } else if (n < 0) {
                c->read_status = n;
            }
            if (n > 0) {
                c->size += (uint32_t)n;
            }
        } else {
            n = 1; /* nothing read yet */
        }
        if (n > 0) {
            TRUST_AWAIT(
                c,
                wasmos_sys_wasm_fs_read_async(
                    &g_loop, &c->op, c->fs_endpoint, g_reply_endpoint, c->fd, 0, TRUST_CHUNK, 0),
                TRUST_PC_READ);
            return WASMOS_WASM_TASK_YIELDED; /* the future settled at once: read on */
        }
        TRUST_AWAIT(c,
                    wasmos_sys_wasm_fs_close_async(
                        &g_loop, &c->op, c->fs_endpoint, g_reply_endpoint, c->fd, 0),
                    TRUST_PC_CLOSED);
        /* fallthrough */
    case TRUST_PC_CLOSED: {
        (void)wasmos_sys_wasm_fs_operation_finish(&c->op, 0, 0, 0);
        if (c->read_status != 0 || c->size == 0u) {
            c->failed = c->add_key ? "cannot read a key from it" : "cannot read it";
            return c->read_status != 0 ? c->read_status : WASMOS_ERR_KEYRING_BAD_REQUEST;
        }
        if (c->add_key) {
            c->payload_len = c->size;
        } else {
            crypto_sha512_final(&c->sha, c->payload);
            memcpy(c->payload + TRUST_DIGEST_LEN, c->path, c->path_len);
            c->payload_len = TRUST_DIGEST_LEN + c->path_len;
        }
        c->buffer = wasmos_xfer_buffer_acquire((int32_t)c->payload_len);
        if (c->buffer < 0 ||
            wasmos_xfer_buffer_write(c->buffer, c->payload, (int32_t)c->payload_len, 0) != 0 ||
            wasmos_xfer_buffer_borrow(c->keyring, c->buffer, WASMOS_BUFFER_GRANT_READ) < 0) {
            c->failed = "cannot hand the request over";
            return c->buffer < 0 ? c->buffer : WASMOS_ERR_KEYRING_BAD_REQUEST;
        }
        wasmos_sys_wasm_ipc_future_init(&c->ipc, 0, 0);
        TRUST_AWAIT(c,
                    wasmos_sys_wasm_ipc_future_send(&g_loop,
                                                    &c->ipc,
                                                    c->keyring,
                                                    g_reply_endpoint,
                                                    c->add_key ? KEYRING_IPC_ADD_KEY_REQ
                                                               : KEYRING_IPC_ALLOW_REQ,
                                                    c->buffer,
                                                    0,
                                                    (int32_t)c->payload_len,
                                                    0,
                                                    0),
                    TRUST_PC_ANSWERED);
    }
        /* fallthrough */
    case TRUST_PC_ANSWERED:
    default:
        break;
    }

    reply = wasmos_sys_wasm_ipc_future_reply(&c->ipc);
    if (!reply) {
        c->failed = "the keyring did not answer";
        return WASMOS_ERR_KEYRING_BAD_REQUEST;
    }
    if (reply->type != KEYRING_IPC_RESP) {
        c->failed = "the keyring refused it";
        return reply->type == KEYRING_IPC_ERROR && reply->arg0 < 0 ? reply->arg0
                                                                   : WASMOS_ERR_KEYRING_BAD_REQUEST;
    }
    return 0;
}

/* Run the task to completion. The poll parks when nothing is queued. */
static int32_t run(trust_ctx_t* ctx) {
    wasmos_wasm_coroutine_t task;
    memset(&task, 0, sizeof(task));
    if (!wasmos_async_start(&g_runtime, &task, trust_task, ctx)) {
        ctx->failed = "cannot start";
        return WASMOS_ERR_KEYRING_BAD_REQUEST;
    }
    for (;;) {
        int32_t status = 0;
        (void)wasmos_wasm_coroutine_run_budget(&g_runtime, 32u);
        if (task.state == WASMOS_WASM_COROUTINE_DEAD) {
            return wasmos_wasm_coroutine_join(&task, &status);
        }
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
    }
}

/* The next whitespace-delimited token of *args, copied to `out`; its length. */
static uint32_t next_token(const char** args, char* out, uint32_t cap) {
    const char* p = *args;
    uint32_t n = 0;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        if (n + 1u < cap) {
            out[n] = *p;
        }
        ++n;
        ++p;
    }
    out[n < cap ? n : cap - 1u] = '\0';
    *args = p;
    return n;
}

static trust_ctx_t g_ctx;

int main(void) {
    char args[192];
    char verb[16];
    char path[TRUST_PATH_MAX + 1u];
    const char* cursor = args;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    uint32_t verb_len = next_token(&cursor, verb, sizeof(verb));
    uint32_t path_len = next_token(&cursor, path, sizeof(path));
    int add_key = verb_len == 7u && strcmp(verb, "add-key") == 0;
    if ((!add_key && (verb_len != 5u || strcmp(verb, "allow") != 0)) || path_len == 0u) {
        (void)printf("usage: trust allow <package.wap> | trust add-key <key.pem>\n");
        return 2;
    }
    if (path_len > TRUST_PATH_MAX) {
        (void)printf("trust: the path is longer than %u bytes\n", (unsigned)TRUST_PATH_MAX);
        return 2;
    }

    g_reply_endpoint = wasmos_ipc_create_endpoint();
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.fs_endpoint = wasmos_fs_endpoint();
    if (g_reply_endpoint < 0 || g_ctx.fs_endpoint < 0) {
        (void)printf("trust: no channel to the system services\n");
        return 1;
    }
    wasmos_wasm_runtime_init(&g_runtime);
    wasmos_sys_event_loop_init(&g_loop, g_reply_endpoint, 0x5400);
    g_ctx.add_key = add_key;
    g_ctx.path = path;
    g_ctx.path_len = path_len;
    g_ctx.fd = -1;
    g_ctx.buffer = -1;

    int32_t status = run(&g_ctx);
    if (g_ctx.buffer >= 0) {
        (void)wasmos_xfer_buffer_release(g_ctx.buffer);
    }
    if (status != 0) {
        (void)printf("trust: %s was not %s, %s: %s\n",
                     path,
                     add_key ? "trusted" : "allowed",
                     g_ctx.failed ? g_ctx.failed : "failed",
                     wasmos_error_code_name((wasmos_error_code_t)status));
        return 1;
    }
    if (add_key) {
        (void)printf("trust: packages signed with the key in %s now start\n", path);
    } else {
        (void)printf("trust: %s may now start, from bytecode with ipc.basic only\n", path);
    }
    return 0;
}
