/* svc - talk to the supervisor from the shell.
 *
 *   svc submit <unit> <path> [restart=...] [class=...] [tty=<slot>] [-- args]
 *   svc list
 *   svc status <unit>
 *
 * submit passes the spec through unchanged; its grammar is
 * SUPERVISOR_IPC_SUBMIT_REQ's (abi/opcodes.yaml). It prints "svc: <unit>
 * accepted" once the supervisor has recorded the unit. list prints one line
 * per unit; status prints one unit's policy and last end. Each exits 0, or
 * prints "svc: <reason>" and exits 1.
 *
 * Runs on an event loop like a service: the supervisor is found by a name
 * watch and the request is an intent, so nothing here waits inside a request.
 * Every request travels in a buffer svc owns and lends to the supervisor. */
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
#define SVC_LIST_REGION                                                                            \
    ((int32_t)(sizeof(wasmos_supervisor_list_desc_t) +                                             \
               SUPERVISOR_UNITS_MAX * sizeof(wasmos_supervisor_unit_record_t)))

typedef enum { SVC_SUBMIT, SVC_LIST, SVC_STATUS } svc_cmd_t;

static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_watches;
static wasmos_sys_svc_watch_t g_watch;
static svc_cmd_t g_cmd;
static wasmos_supervisor_submit_desc_t g_desc;
static wasmos_supervisor_list_desc_t g_list_desc;
static wasmos_supervisor_unit_record_t g_records[SUPERVISOR_UNITS_MAX];
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
    case WASMOS_ERR_SUPERVISOR_DMA_CAPS:
        return "a DMA unit cannot be restarted safely";
    case WASMOS_ERR_PROC_PM_SVC_AWAIT_TIMEOUT:
        return "no supervisor is running";
    default:
        return "request failed";
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

/* A list reply is read back out of the buffer before svc_finish releases it. */
static void svc_on_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (msg->type != SUPERVISOR_IPC_RESP) {
        svc_finish(msg->type == SUPERVISOR_IPC_ERROR ? msg->arg0 : -1);
        return;
    }
    if (g_cmd != SVC_SUBMIT) {
        if (wasmos_xfer_buffer_read(g_bid, &g_list_desc, (int32_t)sizeof(g_list_desc), 0) != 0 ||
            g_list_desc.count > SUPERVISOR_UNITS_MAX ||
            (g_list_desc.count > 0 &&
             wasmos_xfer_buffer_read(
                 g_bid,
                 g_records,
                 (int32_t)(g_list_desc.count * sizeof(wasmos_supervisor_unit_record_t)),
                 (int32_t)sizeof(g_list_desc)) != 0)) {
            svc_finish(-1);
            return;
        }
    }
    svc_finish(0);
}

/* submit lends its descriptor READ; list and status lend the descriptor and
 * the room for the records READ|WRITE. Releasing the buffer after the reply
 * revokes the loan. */
static void svc_on_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    int32_t region;
    int32_t rights;
    int32_t op;
    (void)user;
    wasmos_sys_svc_watch_stop(watch);
    if (g_cmd == SVC_SUBMIT) {
        region = (int32_t)sizeof(g_desc);
        rights = WASMOS_BUFFER_GRANT_READ;
        op = SUPERVISOR_IPC_SUBMIT_REQ;
        g_bid = wasmos_xfer_stage(&g_desc, region);
    } else {
        region = SVC_LIST_REGION;
        rights = WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE;
        op = SUPERVISOR_IPC_LIST_REQ;
        g_bid = wasmos_xfer_buffer_acquire(region);
        if (g_bid >= 0 &&
            wasmos_xfer_buffer_write(g_bid, &g_list_desc, (int32_t)sizeof(g_list_desc), 0) != 0) {
            (void)wasmos_xfer_buffer_release(g_bid);
            g_bid = -1;
        }
    }
    if (g_bid < 0) {
        g_bid = -1;
        svc_finish(-1);
        return;
    }
    if (wasmos_xfer_buffer_borrow(endpoint, g_bid, rights) < 0 ||
        wasmos_sys_intent_send(
            &g_loop, endpoint, g_ep, op, g_bid, 0, region, 0, svc_on_reply, 0, 0) != 0) {
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

static const char* svc_state_name(uint32_t state) {
    switch (state) {
    case SUPERVISOR_UNIT_STATE_SPAWN_DUE:
        return "spawn-due";
    case SUPERVISOR_UNIT_STATE_SPAWNING:
        return "spawning";
    case SUPERVISOR_UNIT_STATE_STARTING:
        return "starting";
    case SUPERVISOR_UNIT_STATE_RUNNING:
        return "running";
    case SUPERVISOR_UNIT_STATE_STABLE:
        return "stable";
    case SUPERVISOR_UNIT_STATE_STOPPING:
        return "stopping";
    case SUPERVISOR_UNIT_STATE_STOPPED:
        return "stopped";
    case SUPERVISOR_UNIT_STATE_FAILED:
        return "failed";
    default:
        return "?";
    }
}

static const char* svc_restart_name(uint32_t restart) {
    switch (restart) {
    case SUPERVISOR_RESTART_NEVER:
        return "never";
    case SUPERVISOR_RESTART_ON_FAILURE:
        return "on-failure";
    case SUPERVISOR_RESTART_ALWAYS:
        return "always";
    default:
        return "?";
    }
}

static const char* svc_class_name(uint32_t cls) {
    switch (cls) {
    case SUPERVISOR_CLASS_OPTIONAL:
        return "optional";
    case SUPERVISOR_CLASS_REQUIRED:
        return "required";
    case SUPERVISOR_CLASS_CRITICAL:
        return "critical";
    default:
        return "?";
    }
}

/* "-" before the unit's first end. */
static const char* svc_end_name(uint32_t reason) {
    switch (reason) {
    case 0:
        return "-";
    case PROC_EXIT_REASON_EXITED:
        return "exited";
    case PROC_EXIT_REASON_TRAPPED:
        return "trapped";
    case PROC_EXIT_REASON_FAULTED:
        return "faulted";
    case PROC_EXIT_REASON_KILLED:
        return "killed";
    case PROC_EXIT_REASON_START_FAILED:
        return "start failed";
    default:
        return "unknown";
    }
}

/* Appends `s` to line[*at..], then spaces up to column `col` (at least one).
 * The libc printf pads on the left only. */
static void svc_column(char* line, uint32_t size, uint32_t* at, const char* s, uint32_t col) {
    while (*s != '\0' && *at + 1u < size) {
        line[(*at)++] = *s++;
    }
    do {
        if (*at + 1u < size) {
            line[(*at)++] = ' ';
        }
    } while (*at < col && *at + 1u < size);
    line[*at] = '\0';
}

static void svc_print_list(void) {
    char line[128];
    char num[16];
    uint32_t at = 0;
    svc_column(line, sizeof(line), &at, "UNIT", 20);
    svc_column(line, sizeof(line), &at, "STATE", 31);
    svc_column(line, sizeof(line), &at, "PID", 37);
    svc_column(line, sizeof(line), &at, "RESTARTS", 47);
    printf("%sLAST\n", line);
    for (uint32_t i = 0; i < g_list_desc.count; ++i) {
        const wasmos_supervisor_unit_record_t* r = &g_records[i];
        at = 0;
        svc_column(line, sizeof(line), &at, r->name, 20);
        svc_column(line, sizeof(line), &at, svc_state_name(r->state), 31);
        (void)snprintf(num, sizeof(num), "%u", r->pid);
        svc_column(line, sizeof(line), &at, num, 37);
        (void)snprintf(num, sizeof(num), "%u", r->restart_count);
        svc_column(line, sizeof(line), &at, num, 47);
        if (r->last_reason == 0) {
            printf("%s-\n", line);
        } else {
            printf("%s%s (status %d)\n", line, svc_end_name(r->last_reason), (int)r->last_status);
        }
    }
    if (g_list_desc.count < g_list_desc.total) {
        printf("svc: %u more unit(s) not shown\n", g_list_desc.total - g_list_desc.count);
    }
}

/* Returns 0, or 1 when no unit has that name. */
static int svc_print_status(const char* name) {
    for (uint32_t i = 0; i < g_list_desc.count; ++i) {
        const wasmos_supervisor_unit_record_t* r = &g_records[i];
        if (strcmp(r->name, name) != 0) {
            continue;
        }
        printf("%s: %s, pid %u\n", r->name, svc_state_name(r->state), r->pid);
        printf("  restart=%s class=%s", svc_restart_name(r->restart), svc_class_name(r->cls));
        if (r->tty != 0) {
            printf(" tty=%u", r->tty - 1u);
        }
        printf("\n  restarts: %u\n", r->restart_count);
        if (r->probe_interval_ms == 0) {
            printf("  liveness: not probed\n");
        } else {
            printf("  liveness: every %u ms, %u answered, %u missed in a row\n",
                   r->probe_interval_ms,
                   r->probes_answered,
                   r->probe_misses);
        }
        if (r->last_reason == 0) {
            printf("  last end: -\n");
        } else {
            printf(
                "  last end: %s (status %d)\n", svc_end_name(r->last_reason), (int)r->last_status);
        }
        return 0;
    }
    printf("svc: no unit named %s\n", name);
    return 1;
}

static void svc_usage(void) {
    puts("usage: svc submit <unit> <path> [restart=never|on-failure|always]");
    puts("                  [class=optional|required|critical] [tty=<slot>]");
    puts("                  [probe=<seconds>] [-- args]");
    puts("       svc list");
    puts("       svc status <unit>");
}

static const char* svc_skip_spaces(const char* s) {
    while (*s == ' ') {
        s++;
    }
    return s;
}

int main(void) {
    char args[SUPERVISOR_SPEC_MAX + 16];
    char unit[SUPERVISOR_UNIT_NAME_MAX + 1];
    const char* spec = "";
    uint32_t len;
    uint32_t n = 0;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    if (strncmp(args, "submit ", 7) == 0) {
        g_cmd = SVC_SUBMIT;
        spec = svc_skip_spaces(args + 7);
        len = (uint32_t)strlen(spec);
        if (len == 0 || len > SUPERVISOR_SPEC_MAX) {
            printf("svc: %s\n", svc_reason(WASMOS_ERR_SUPERVISOR_BAD_SPEC));
            return 1;
        }
        memset(&g_desc, 0, sizeof(g_desc));
        g_desc.version = SUPERVISOR_SUBMIT_DESC_VERSION;
        g_desc.spec_len = len;
        memcpy(g_desc.spec, spec, len + 1u);
    } else if (strcmp(args, "list") == 0 || strncmp(args, "status ", 7) == 0) {
        g_cmd = args[0] == 'l' ? SVC_LIST : SVC_STATUS;
        if (g_cmd == SVC_STATUS) {
            spec = svc_skip_spaces(args + 7);
            if (*spec == '\0') {
                svc_usage();
                return 2;
            }
        }
        memset(&g_list_desc, 0, sizeof(g_list_desc));
        g_list_desc.version = SUPERVISOR_LIST_DESC_VERSION;
        g_list_desc.capacity = SUPERVISOR_UNITS_MAX;
    } else {
        svc_usage();
        return 2;
    }

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
    if (g_cmd == SVC_LIST) {
        svc_print_list();
        return 0;
    }
    if (g_cmd == SVC_STATUS) {
        return svc_print_status(spec);
    }
    while (n < SUPERVISOR_UNIT_NAME_MAX && spec[n] != '\0' && spec[n] != ' ' && spec[n] != '\t') {
        unit[n] = spec[n];
        n++;
    }
    unit[n] = '\0';
    printf("svc: %s accepted\n", unit);
    return 0;
}
