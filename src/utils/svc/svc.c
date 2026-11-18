/* svc - hand units to the supervisor from the shell.
 *
 *   svc submit <unit> <path> [restart=...] [class=...] [tty=<slot>] [-- args]
 *
 * The spec after `submit` is passed through unchanged; its grammar is
 * SUPERVISOR_IPC_SUBMIT_REQ's (abi/opcodes.yaml). Prints "svc: <unit>
 * accepted" and exits 0 once the supervisor has recorded the unit, or
 * "svc: <reason>" and exits 1.
 *
 * Runs on an event loop like a service: the supervisor is found by a name
 * watch and the submission is an intent, so nothing here waits inside a
 * request. */
#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define SVC_FIND_TIMEOUT_MS 5000
#define SVC_TICK_MS 50
#define SVC_LOOP_REQ_BASE 0x5300

static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_watches;
static wasmos_sys_svc_watch_t g_watch;
static wasmos_supervisor_submit_desc_t g_desc;
static int32_t g_ep = -1;
static int32_t g_bid = -1;
static int g_done;
static int32_t g_status; /* 0, or the code that ended the run */

static const char* svc_reason(int32_t code) {
    switch (code) {
    case WASMOS_ERR_SUPERVISOR_BAD_DESC:
        return "the supervisor could not read the submission";
    case WASMOS_ERR_SUPERVISOR_BAD_SPEC:
        return "bad unit spec";
    case WASMOS_ERR_SUPERVISOR_EXISTS:
        return "a unit of that name exists";
    case WASMOS_ERR_SUPERVISOR_NO_SPACE:
        return "the supervisor's unit table is full";
    case WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT:
        return "no supervisor is running";
    default:
        return "submission failed";
    }
}

static void svc_finish(int32_t status) {
    if (g_bid >= 0) {
        (void)wasmos_xfer_buffer_release(g_bid);
        g_bid = -1;
    }
    g_status = status;
    g_done = 1;
}

static void svc_on_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type == SUPERVISOR_IPC_RESP) {
        svc_finish(0);
    } else {
        svc_finish(msg->type == SUPERVISOR_IPC_ERROR ? msg->arg0 : -1);
    }
}

/* The descriptor travels in a buffer svc owns and lends READ to the
 * supervisor; releasing it after the reply revokes the loan. */
static void svc_on_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    wasmos_sys_svc_watch_stop(watch);
    g_bid = wasmos_xfer_stage(&g_desc, (int32_t)sizeof(g_desc));
    if (g_bid < 0) {
        g_bid = -1;
        svc_finish(-1);
        return;
    }
    if (wasmos_xfer_buffer_borrow(endpoint, g_bid, WASMOS_BUFFER_GRANT_READ) < 0 ||
        wasmos_sys_intent_send(&g_loop,
                               endpoint,
                               g_ep,
                               SUPERVISOR_IPC_SUBMIT_REQ,
                               g_bid,
                               0,
                               (int32_t)sizeof(g_desc),
                               0,
                               svc_on_reply,
                               0,
                               0) != 0) {
        svc_finish(-1);
    }
}

static void svc_on_missing(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)user;
    (void)watch;
    svc_finish(status);
}

static void svc_on_tick(void* user) {
    (void)user;
    wasmos_sys_svc_watch_set_expire(&g_watches);
}

int main(void) {
    char args[SUPERVISOR_SPEC_MAX + 16];
    char unit[SUPERVISOR_UNIT_NAME_MAX + 1];
    const char* spec;
    uint32_t len;
    uint32_t n = 0;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    if (strncmp(args, "submit ", 7) != 0) {
        puts("usage: svc submit <unit> <path> [restart=never|on-failure|always]");
        puts("                  [class=optional|required|critical] [tty=<slot>] [-- args]");
        return 2;
    }
    spec = args + 7;
    while (*spec == ' ') {
        spec++;
    }
    len = (uint32_t)strlen(spec);
    if (len == 0 || len > SUPERVISOR_SPEC_MAX) {
        printf("svc: %s\n", svc_reason(WASMOS_ERR_SUPERVISOR_BAD_SPEC));
        return 1;
    }
    memset(&g_desc, 0, sizeof(g_desc));
    g_desc.version = SUPERVISOR_SUBMIT_DESC_VERSION;
    g_desc.spec_len = len;
    memcpy(g_desc.spec, spec, len + 1u);

    g_ep = wasmos_ipc_create_endpoint();
    if (g_ep < 0) {
        puts("svc: cannot create an endpoint");
        return 1;
    }
    wasmos_sys_event_loop_init(&g_loop, g_ep, SVC_LOOP_REQ_BASE);
    g_loop.on_timeout = svc_on_tick;
    g_loop.poll_timeout_ms = SVC_TICK_MS;
    if (wasmos_sys_svc_watch_set_init(&g_watches, &g_loop, wasmos_startup_proc_endpoint()) != 0 ||
        wasmos_sys_svc_watch_start(&g_watches,
                                   &g_watch,
                                   "supervisor",
                                   SVC_FIND_TIMEOUT_MS,
                                   svc_on_bound,
                                   svc_on_missing,
                                   0) != 0) {
        puts("svc: cannot look for the supervisor");
        return 1;
    }
    while (!g_done) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
        wasmos_sys_svc_watch_set_expire(&g_watches);
    }
    if (g_status != 0) {
        printf("svc: %s\n", svc_reason(g_status));
        return 1;
    }
    while (n < SUPERVISOR_UNIT_NAME_MAX && spec[n] != '\0' && spec[n] != ' ' && spec[n] != '\t') {
        unit[n] = spec[n];
        n++;
    }
    unit[n] = '\0';
    printf("svc: %s accepted\n", unit);
    return 0;
}
