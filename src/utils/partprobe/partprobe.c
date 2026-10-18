/* partprobe -- tell the partition manager a disk's table has changed.
 *
 * Usage: partprobe <canonical-id>
 *
 *   canonical-id  the DISK's id as blkinfo prints it, e.g. `block:ata:0`. A
 *                 partition's id is not accepted and would not work: the
 *                 partition manager keys a rescan on the backing disk, which is
 *                 the thing that holds a table.
 *
 * A partition table is read once, when its disk arrives under the `block`
 * class, and nothing re-reads it. A table rewritten from inside the guest
 * therefore leaves every partition device exactly as it was at boot -- the old
 * extents, the old slots, and devices for partitions that no longer exist. This
 * is the tool that closes that gap, and `fdisk` will send the same request once
 * its write has landed.
 *
 * What comes back is a reconciliation, not a republish. A partition whose
 * extent did not change is left strictly alone, because a filesystem may be
 * mounted on it; one whose extent moved keeps its identity and has only its
 * descriptor restated; one the table no longer holds is retired from the device
 * manager and the `block` class. A retirement is refused while a filesystem
 * holds the partition -- the volume manager's CLAIM, which is the only live
 * record of what is mounted -- and the refusal is reported here rather than
 * failing the rescan, because the rest of the disk was reconciled correctly and
 * the refusal is the useful half of the answer.
 *
 * The request is sent over the system IPC-future bridge rather than a blocking
 * call. The reply is DEFERRED by several round trips -- a table read, then two
 * per retirement -- and a blocking wrapper would be a new synchronous
 * request/reply, which this tree does not add (AGENTS.md).
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_cast.h"
#include "wasmos_driver_abi.h"

#define PARTPROBE_MAX_PROVIDERS 8

static wasmos_wasm_runtime_t g_runtime;
static wasmos_sys_event_loop_t g_loop;
static int32_t g_proc_endpoint = -1;
static int32_t g_reply_endpoint = -1;
static int32_t g_request_id = 1;

/* Await `future`, resuming this task at `next_pc`.
 *
 * A stackless task is re-entered from the top, so every suspension point is a
 * program counter the switch below dispatches on; the local state a stack would
 * hold lives in the context instead. Same shape as the WFS tasks, which is
 * where this form comes from. */
#define PARTPROBE_AWAIT(ctx, future, next_pc)                                                      \
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

enum {
    PARTPROBE_PC_START = 0,
    PARTPROBE_PC_ANSWERED,
};

typedef struct {
    uint32_t pc;
    wasmos_sys_wasm_ipc_future_t op;
    int32_t endpoint;
    uint32_t instance;
    /* The answer, valid only once the task has joined cleanly. */
    uint32_t partitions;
    uint32_t refused;
} probe_ctx_t;

/* Send the rescan and keep what came back.
 *
 * Any reply is accepted into the record, because both outcomes are replies the
 * caller must see: PARTITION_IPC_RESP carries the counts, PARTITION_IPC_ERROR a
 * packed code naming why nothing was done. A predicate rejecting the error form
 * would turn a diagnosable refusal into a bare failure. */
static int32_t probe_task(void* user, uintptr_t* out_value) {
    probe_ctx_t* c = (probe_ctx_t*)user;
    const wasmos_ipc_message_t* reply;

    if (out_value) {
        *out_value = 0;
    }
    switch (c->pc) {
    case PARTPROBE_PC_START:
        wasmos_sys_wasm_ipc_future_init(&c->op, 0, 0);
        PARTPROBE_AWAIT(c,
                        wasmos_sys_wasm_ipc_future_send(&g_loop,
                                                        &c->op,
                                                        c->endpoint,
                                                        g_reply_endpoint,
                                                        PARTITION_IPC_RESCAN_REQ,
                                                        (int32_t)c->instance,
                                                        0,
                                                        0,
                                                        0,
                                                        0),
                        PARTPROBE_PC_ANSWERED);
        /* fallthrough */
    case PARTPROBE_PC_ANSWERED:
    default:
        break;
    }

    reply = wasmos_sys_wasm_ipc_future_reply(&c->op);
    if (!reply) {
        return WASMOS_ERR_BLOCK_DEV_NOT_READY;
    }
    if (reply->type == PARTITION_IPC_ERROR) {
        return (wasmos_error_code_t)reply->arg0;
    }
    if (reply->type != PARTITION_IPC_RESP || reply->arg0 != 0) {
        return WASMOS_ERR_BLOCK_DEV_UNSUPPORTED_REQUEST;
    }
    c->partitions = (uint32_t)reply->arg1;
    c->refused = (uint32_t)reply->arg2;
    return 0;
}

/* Run one task to completion. The poll parks when nothing is queued, so a
 * rescan waiting on a disk costs no CPU. */
static int32_t run(wasmos_wasm_task_resume_fn fn, void* ctx) {
    wasmos_wasm_coroutine_t task;
    uint8_t* p = (uint8_t*)&task;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(task); ++i) {
        p[i] = 0u;
    }
    if (!wasmos_async_start(&g_runtime, &task, fn, ctx)) {
        return WASMOS_ERR_BLOCK_DEV_NOT_READY;
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

/* The partition manager's service endpoint, or -1.
 *
 * Resolved by NAME rather than by the `block` class it provides. Every
 * partition it publishes registers under that class, so a class lookup would
 * return one entry per partition and no way to tell the manager from the
 * devices it serves. */
static int32_t find_partition_manager(void) {
    return wasmos_svc_lookup(g_proc_endpoint, g_reply_endpoint, "partition", g_request_id++);
}

/* Copy the first whitespace-delimited token of `args` into `out`. Returns its
 * length, or 0 when there is none. */
static uint32_t first_token(const char* args, char* out, uint32_t cap) {
    uint32_t n = 0;
    while (*args == ' ' || *args == '\t') {
        ++args;
    }
    while (*args != '\0' && *args != ' ' && *args != '\t' && n + 1u < cap) {
        out[n++] = *args++;
    }
    out[n] = '\0';
    return n;
}

int main(void) {
    char args[96];
    char id[BLOCK_DESCRIPTOR_ID_MAX];
    probe_ctx_t ctx;
    int32_t status;
    uint32_t i;
    uint8_t* raw = (uint8_t*)&ctx;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    if (first_token(args, id, sizeof(id)) == 0u) {
        (void)printf("usage: partprobe <canonical-id>   "
                     "(the DISK, as blkinfo prints it, e.g. block:ata:0)\n");
        return 2;
    }

    g_proc_endpoint = wasmos_startup_proc_endpoint();
    g_reply_endpoint = wasmos_ipc_create_endpoint();
    if (g_proc_endpoint < 0 || g_reply_endpoint < 0) {
        (void)printf("[partprobe] no process-manager channel\n");
        return 1;
    }
    wasmos_wasm_runtime_init(&g_runtime);
    wasmos_sys_event_loop_init(&g_loop, g_reply_endpoint, 0x5200);

    for (i = 0; i < (uint32_t)sizeof(ctx); ++i) {
        raw[i] = 0u;
    }
    ctx.endpoint = find_partition_manager();
    if (ctx.endpoint < 0) {
        (void)printf("[partprobe] no partition manager is running\n");
        return 1;
    }
    /* The class instance is a fingerprint of the canonical id, so a mistyped or
     * truncated id resolves to no device rather than to the wrong one -- which
     * the manager reports as NO_SUCH_UNIT below. */
    ctx.instance = wasmos_block_fingerprint(id);
    if (ctx.instance == 0u) {
        (void)printf("[partprobe] '%s' is not a usable device id\n", id);
        return 2;
    }

    status = run(probe_task, &ctx);
    if (status != 0) {
        (void)printf("[partprobe] %s was not rescanned: %s\n",
                     id,
                     wasmos_error_code_name((wasmos_error_code_t)status));
        return 1;
    }

    (void)printf("[partprobe] %s rescanned; %u partitions\n", id, (unsigned)ctx.partitions);
    if (ctx.refused > 0u) {
        /* The usual cause is a filesystem still holding the partition, so say
         * what to do about it rather than reprinting the count. */
        (void)printf("[partprobe] %u partitions could not be retired; "
                     "unmount what is still using them and rescan again\n",
                     (unsigned)ctx.refused);
        return 1;
    }
    return 0;
}
