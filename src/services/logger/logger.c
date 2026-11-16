/* logger - append the kernel log to /var/log/messages.
 *
 * The kernel keeps the newest 64 KiB of kernel log in its kmsg ring (dmesg
 * prints it). This service makes the log outlive the ring: it pages the ring
 * with kmsg_read from the first retained byte and appends everything to
 * LOGGER_PATH, so the file starts with the boot that happened before the
 * service existed and follows every kernel-log line after it.
 *
 * It sleeps between batches. kmsg_subscribe makes the kernel send
 * WASMOS_IPC_KMSG_NOTIFY to the service's reply endpoint once per batch of
 * appends; the event loop turns that into a wake of the root task, which drains
 * the ring to its end and parks again. Writes go through fs-manager as futures
 * (wasmos_sys_wasm_fs_*_async), never as blocking calls, and so does the
 * ready notification the process manager waits for before it answers the
 * spawn.
 *
 * Bytes the ring overwrote before they were read are not skipped silently: a
 * "[logger] N bytes ... lost" line takes their place in the file. The file is
 * capped at LOGGER_FILE_MAX bytes written by this instance; on reaching it the
 * file is truncated and restarts with a line saying so. The default location is
 * the tmpfs root, so the file lasts until reboot.
 *
 * TODO: rotation keeps no history because the async FS client has no rename;
 * with wasmos_sys_wasm_fs_rename_async the cap could move the file to
 * messages.1 instead of truncating it.
 *
 * The service's own output is kernel log too (it has no controlling tty), so it
 * prints only when it gives up, and exits then rather than retry: a write that
 * failed would fail again on the notify its own message raises.
 */
#include <stdint.h>

#include "fcntl.h"
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/service_runtime_wasm.h"
#include "wasmos/startup.h"

#include "logger_stage.h"

#define LOGGER_PATH "/var/log/messages"
#define LOGGER_FILE_MAX (1024u * 1024u)

typedef enum {
    LOGGER_START = 0,
    LOGGER_READYING,
    LOGGER_OPEN,
    LOGGER_OPENING,
    LOGGER_DRAIN,
    LOGGER_WRITING,
    LOGGER_CLOSING,
    LOGGER_IDLE,
    LOGGER_WAITING,
} logger_state_t;

typedef struct {
    logger_state_t state;
    int32_t fs_endpoint;
    int32_t fd;
    uint32_t file_bytes; /* bytes this instance wrote to the open file */
    uint8_t truncate;    /* the next open restarts the file */
    uint8_t notified;    /* a KMSG_NOTIFY arrived since the last drain began */
    uint8_t wake_armed;  /* the root task is parked on `wake` */
    wasmos_sys_wasm_ipc_future_t ready;
    wasmos_sys_wasm_fs_operation_t op;
    wasmos_future_t* pending;
    wasmos_future_t wake;
    wasmos_promise_t wake_promise;
    logger_stage_t stage; /* the ring position and the next write (logger_stage.h) */
} logger_t;

static logger_t g_logger;

static void logger_on_notify(void* user, const wasmos_ipc_message_t* msg) {
    logger_t* lg = (logger_t*)user;
    (void)msg;
    lg->notified = 1;
    if (lg->wake_armed) {
        lg->wake_armed = 0;
        (void)wasmos_promise_resolve(&lg->wake_promise, 0);
    }
}

/* Prints why the service stops and returns the status that ends the root task. */
static int32_t logger_fail(const char* what, int32_t rc) {
    printf("[logger] %s (%d); %s is no longer written\n", what, (int)rc, LOGGER_PATH);
    return rc < 0 ? rc : -1;
}

static int32_t logger_read_ring(void* user, uint8_t* dst, int32_t len, uint64_t* seq) {
    (void)user;
    return wasmos_kmsg_read(dst, len, seq);
}

static int32_t logger_resume(void* user, uintptr_t* out_value) {
    logger_t* lg = (logger_t*)user;
    wasmos_sys_event_loop_t* loop = wasmos_sys_wasm_async_event_loop();
    int32_t reply_ep = wasmos_sys_wasm_async_reply_endpoint();
    uintptr_t value = 0;
    int32_t rc = 0;
    int32_t written = 0;
    (void)out_value;

    for (;;) {
        switch (lg->state) {
        case LOGGER_START:
            lg->fs_endpoint = wasmos_fs_endpoint();
            if (lg->fs_endpoint < 0) {
                return logger_fail("no filesystem service", lg->fs_endpoint);
            }
            if (wasmos_sys_event_register(loop, WASMOS_IPC_KMSG_NOTIFY, logger_on_notify, lg) !=
                0) {
                return logger_fail("cannot handle kernel-log notifies", -1);
            }
            /* Subscribe before the first drain, so an append that lands while the
             * boot backlog is written still raises a notify. */
            rc = wasmos_kmsg_subscribe(reply_ep);
            if (rc != 0) {
                return logger_fail("kmsg_subscribe failed", rc);
            }
            /* The process manager answers the parent's spawn only once a service
             * reports ready; the ack arrives on the loop like any other reply. */
            wasmos_sys_wasm_ipc_future_init(&lg->ready, 0, 0);
            lg->pending = wasmos_sys_wasm_ipc_future_send(loop,
                                                          &lg->ready,
                                                          wasmos_startup_proc_endpoint(),
                                                          reply_ep,
                                                          PROC_IPC_NOTIFY_READY,
                                                          0,
                                                          0,
                                                          0,
                                                          0,
                                                          0);
            if (!lg->pending) {
                return logger_fail("cannot report ready", -1);
            }
            lg->state = LOGGER_READYING;
            break;
        case LOGGER_READYING:
            rc = wasmos_future_await(lg->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            if (rc != 0) {
                return logger_fail("the process manager refused ready", rc);
            }
            lg->state = LOGGER_OPEN;
            break;
        case LOGGER_OPEN:
            lg->pending = wasmos_sys_wasm_fs_open_async(loop,
                                                        &lg->op,
                                                        lg->fs_endpoint,
                                                        reply_ep,
                                                        LOGGER_PATH,
                                                        O_WRONLY | O_CREAT |
                                                            (lg->truncate ? O_TRUNC : O_APPEND),
                                                        0);
            if (!lg->pending) {
                return logger_fail("cannot start the open", -1);
            }
            lg->state = LOGGER_OPENING;
            break;
        case LOGGER_OPENING:
            rc = wasmos_future_await(lg->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            lg->fd = wasmos_sys_wasm_fs_operation_finish(&lg->op, 0, 0, 0);
            if (rc != 0 || lg->fd < 0) {
                return logger_fail("cannot open " LOGGER_PATH, rc != 0 ? rc : lg->fd);
            }
            if (lg->truncate) {
                logger_stage_truncated(&lg->stage, LOGGER_PATH, LOGGER_FILE_MAX);
            }
            lg->truncate = 0;
            lg->file_bytes = 0;
            lg->state = LOGGER_DRAIN;
            break;
        case LOGGER_DRAIN:
            if (lg->file_bytes >= LOGGER_FILE_MAX) {
                lg->pending = wasmos_sys_wasm_fs_close_async(
                    loop, &lg->op, lg->fs_endpoint, reply_ep, lg->fd, 0);
                if (!lg->pending) {
                    return logger_fail("cannot start the close", -1);
                }
                lg->state = LOGGER_CLOSING;
                break;
            }
            lg->notified = 0;
            rc = logger_stage_next(&lg->stage, logger_read_ring, 0);
            if (rc < 0) {
                return logger_fail("kmsg_read failed", rc);
            }
            if (rc == 0) {
                lg->state = LOGGER_IDLE;
                break;
            }
            lg->pending = wasmos_sys_wasm_fs_write_async(loop,
                                                         &lg->op,
                                                         lg->fs_endpoint,
                                                         reply_ep,
                                                         lg->fd,
                                                         lg->stage.out,
                                                         (int32_t)lg->stage.out_len,
                                                         0);
            if (!lg->pending) {
                return logger_fail("cannot start a write", -1);
            }
            lg->state = LOGGER_WRITING;
            break;
        case LOGGER_WRITING:
            rc = wasmos_future_await(lg->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            written = wasmos_sys_wasm_fs_operation_finish(&lg->op, 0, 0, 0);
            if (rc != 0) {
                return logger_fail("write failed", rc);
            }
            if (!logger_stage_written(lg->stage.out_len, written)) {
                return logger_fail("short or failed write (filesystem full?)", written);
            }
            lg->file_bytes += lg->stage.out_len;
            lg->state = LOGGER_DRAIN;
            break;
        case LOGGER_CLOSING:
            rc = wasmos_future_await(lg->pending, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            (void)wasmos_sys_wasm_fs_operation_finish(&lg->op, 0, 0, 0);
            lg->truncate = 1;
            lg->state = LOGGER_OPEN;
            break;
        case LOGGER_IDLE:
            if (lg->notified) {
                lg->state = LOGGER_DRAIN;
                break;
            }
            wasmos_future_init(&lg->wake, &lg->wake_promise);
            lg->wake_armed = 1;
            lg->state = LOGGER_WAITING;
            break;
        case LOGGER_WAITING:
            rc = wasmos_future_await(&lg->wake, &value);
            if (rc == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            lg->state = LOGGER_DRAIN;
            break;
        }
    }
}

wasmos_sys_wasm_async_config_t wasmos_async_service = {
    .resume = logger_resume,
    .user = &g_logger,
};
