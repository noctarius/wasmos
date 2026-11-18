/* supervisor.c - keeps submitted units running.
 *
 * A unit is handed over by SUPERVISOR_IPC_SUBMIT_REQ (abi/opcodes.yaml) and
 * from then on the supervisor owns "should be running -> is running": it spawns
 * the unit with PROC_IPC_SPAWN_WATCHED, learns of its READY and its end from
 * PROC_IPC_CHILD_READY / PROC_IPC_CHILD_EXITED, and restarts or gives up by the
 * unit's policy (supervisor_unit.c). SUPERVISOR_IPC_LIST_REQ reports the unit
 * table.
 *
 * A critical unit's end takes the system down: a line naming it, a dump of the
 * unit table to the kernel log, then the orderly shutdown sequence and a halt,
 * or a reboot when kenv supervisor.on_critical=reboot. The supervisor is itself
 * critical: kernel init marks it so, and its own end is a kernel panic.
 *
 * kenv keys, read when they are needed:
 *   supervisor.timescale=N    multiply every policy number by N (1-100)
 *   supervisor.on_critical=   halt (default) | reboot
 *   supervisor.fault=trap     trap on the first submission, read at start: the
 *                             supervisor's death on demand, for testing it
 *
 * Everything runs on one event loop on the service endpoint, which is also the
 * spawn's notify endpoint, so the spawn reply precedes every event of the child
 * it names. Nothing here blocks: requests to the process manager are intents
 * answered on the loop, and deadlines are the loop's poll timeout. */
#include <stdint.h>
#include "stdio.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"
#include "supervisor_unit.h"

#define SV_UNITS_MAX SUPERVISOR_UNITS_MAX
#define SV_LOOP_REQ_BASE 0x5100
#define SV_TIMESCALE_KEY "supervisor.timescale"
#define SV_TIMESCALE_MAX 100u
#define SV_ON_CRITICAL_KEY "supervisor.on_critical"
#define SV_FAULT_KEY "supervisor.fault"

_Static_assert(1000u % WASMOS_SCHED_TICK_HZ == 0u, "a tick must be a whole number of ms");

/* The watched-spawn request in the supervisor's own transfer buffer: the
 * descriptor, then the path and the args at the offsets it names. */
#define SV_SPAWN_PATH_OFF ((uint32_t)sizeof(wasmos_spawn_watch_desc_t))
#define SV_SPAWN_ARGS_OFF (SV_SPAWN_PATH_OFF + SV_PATH_MAX)
#define SV_SPAWN_REGION (SV_SPAWN_ARGS_OFF + SV_ARGS_MAX)

/* Default policy numbers, multiplied by the kenv timescale. */
static const sv_timing_t k_base_timing = {
    .ready_timeout_ms = 10000,
    .stable_ms = 10000,
    .backoff_min_ms = 250,
    .backoff_max_ms = 30000,
    .intensity_max = 5,
    .intensity_window_ms = 60000,
};

static wasmos_sys_event_loop_t g_loop;
static int32_t g_ep = -1;
static int32_t g_proc_ep = -1;
static int32_t g_register_bid = -1;
static sv_timing_t g_timing;
static sv_unit_t g_units[SV_UNITS_MAX];
/* One watched spawn is in flight at a time, from this buffer; g_spawn_unit is
 * the unit it is for, -1 when none. */
static int32_t g_spawn_bid = -1;
static int32_t g_spawn_unit = -1;
/* SUPERVISOR_IPC_LIST_REQ records, staged here before they are written out. */
static wasmos_supervisor_unit_record_t g_list[SV_UNITS_MAX];
static int g_fault_trap;

static uint32_t sv_now(void) {
    return (uint32_t)wasmos_sched_ticks() * (1000u / WASMOS_SCHED_TICK_HZ);
}

static void sv_log(const char* line) {
    int32_t len = (int32_t)strlen(line);
    if (len > 0) {
        (void)wasmos_console_write(addr_cast(int32_t, line), len);
    }
}

/* Reads kenv `key` into out[0..size), truncated and NUL-terminated by the host
 * call. Returns its length, or 0 when it is absent or empty. */
static int32_t sv_env(const char* key, char* out, int32_t size) {
    int32_t n = wasmos_env_get(key, (int32_t)strlen(key), out, size);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    return n;
}

/* The kenv timescale stretches every policy number; CI and test configs raise
 * it instead of tuning each timeout. Absent or out of range reads as 1. */
static void sv_load_timing(void) {
    char buf[8];
    uint32_t scale = 1;
    int32_t n = sv_env(SV_TIMESCALE_KEY, buf, (int32_t)sizeof(buf));
    if (n > 0) {
        uint32_t v = 0;
        int32_t i = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') {
            v = v * 10u + (uint32_t)(buf[i] - '0');
            i++;
        }
        if (i == n && v >= 1u && v <= SV_TIMESCALE_MAX) {
            scale = v;
        }
    }
    g_timing = k_base_timing;
    g_timing.ready_timeout_ms *= scale;
    g_timing.stable_ms *= scale;
    g_timing.backoff_min_ms *= scale;
    g_timing.backoff_max_ms *= scale;
    g_timing.intensity_window_ms *= scale;
}

static sv_unit_t* sv_unit_by_pid(uint32_t pid) {
    if (pid == 0) {
        return 0;
    }
    for (int i = 0; i < SV_UNITS_MAX; ++i) {
        if (g_units[i].state != SV_UNIT_FREE && g_units[i].pid == pid) {
            return &g_units[i];
        }
    }
    return 0;
}

static void sv_log_end(const sv_unit_t* u, const char* what) {
    char line[160];
    (void)snprintf(line,
                   sizeof(line),
                   "[supervisor] %s %s (%s, status %d)",
                   u->name,
                   what,
                   sv_reason_name(u->last_reason),
                   (int)u->last_status);
    sv_log(line);
}

/* Consumes a reply that carries nothing the supervisor acts on: the
 * acknowledgement of NOTIFY_READY, and a kill's, since the killed unit's end
 * arrives as PROC_IPC_CHILD_EXITED either way. */
static void sv_on_ignored_reply(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

static int sv_fault_is_trap(void) {
    char fault[8];
    return sv_env(SV_FAULT_KEY, fault, (int32_t)sizeof(fault)) > 0 && strcmp(fault, "trap") == 0;
}

/* The unit table, one line per held unit, for the kernel log. */
static void sv_dump_units(void) {
    char line[160];
    sv_log("[supervisor] unit table:\n");
    for (int32_t i = 0; i < SV_UNITS_MAX; ++i) {
        const sv_unit_t* u = &g_units[i];
        if (u->state == SV_UNIT_FREE) {
            continue;
        }
        (void)snprintf(line,
                       sizeof(line),
                       "[supervisor]   %s %s pid=%u restarts=%u last=%s status=%d\n",
                       u->name,
                       sv_state_name(u->state),
                       u->pid,
                       u->restart_count,
                       u->last_reason == 0 ? "-" : sv_reason_name(u->last_reason),
                       (int)u->last_status);
        sv_log(line);
    }
}

/* A critical unit ended: name it, dump the table, then the orderly shutdown
 * sequence and power-off or reboot. Returns only when the system-control
 * capability is refused, and then the system stays up without the unit rather
 * than in a loop. An unknown supervisor.on_critical halts. */
static void sv_critical(const sv_unit_t* u) {
    char mode[16];
    char line[160];
    int reboot =
        sv_env(SV_ON_CRITICAL_KEY, mode, (int32_t)sizeof(mode)) > 0 && strcmp(mode, "reboot") == 0;
    (void)snprintf(line,
                   sizeof(line),
                   "[supervisor] critical unit %s died (%s, status %d); %s\n",
                   u->name,
                   sv_reason_name(u->last_reason),
                   (int)u->last_status,
                   reboot ? "rebooting" : "halting");
    sv_log(line);
    sv_dump_units();
    if (reboot) {
        /* TODO: nothing bounds a reboot loop: a critical unit that dies on
         * every boot reboots forever. A guard needs a boot counter that
         * survives the reboot (on the ESP or a WFS volume). */
        if (wasmos_system_reboot() != 0) {
            sv_log("[supervisor] reboot refused; continuing without the unit\n");
        }
        return;
    }
    if (wasmos_system_halt() != 0) {
        sv_log("[supervisor] halt refused; continuing without the unit\n");
    }
}

/* Logs a verdict and carries out the parts that are not the next spawn. */
static void sv_act(sv_unit_t* u, sv_verdict_t verdict) {
    char line[160];
    switch (verdict) {
    case SV_VERDICT_RESTART:
        sv_log_end(u, "ended");
        (void)snprintf(line,
                       sizeof(line),
                       "; restart %u in %u ms\n",
                       u->restart_count,
                       u->deadline - sv_now());
        sv_log(line);
        break;
    case SV_VERDICT_STOPPED:
        sv_log_end(u, "stopped");
        sv_log("\n");
        break;
    case SV_VERDICT_FAILED:
        sv_log_end(u, "failed");
        (void)snprintf(line, sizeof(line), " after %u restarts; giving up\n", u->restart_count);
        sv_log(line);
        break;
    case SV_VERDICT_CRITICAL:
        sv_critical(u);
        break;
    case SV_VERDICT_KILL:
        (void)snprintf(line,
                       sizeof(line),
                       "[supervisor] %s not ready after %u ms; killing pid %u\n",
                       u->name,
                       g_timing.ready_timeout_ms,
                       u->pid);
        sv_log(line);
        (void)wasmos_sys_intent_send(&g_loop,
                                     g_proc_ep,
                                     g_ep,
                                     PROC_IPC_KILL,
                                     (int32_t)u->pid,
                                     -1,
                                     0,
                                     0,
                                     sv_on_ignored_reply,
                                     0,
                                     0);
        break;
    default:
        break;
    }
}

/* ----------------------------------------------------------------- spawn */

static void sv_spawn_next(void);

static void sv_on_spawn_reply(void* user, const wasmos_ipc_message_t* msg) {
    sv_unit_t* u = &g_units[(int32_t)(intptr_t)user];
    g_spawn_unit = -1;
    if (msg->type == PROC_IPC_RESP && msg->arg0 > 0) {
        sv_unit_spawned(u, &g_timing, (uint32_t)msg->arg0, sv_now());
    } else {
        int32_t code = msg->type == PROC_IPC_ERROR ? msg->arg1 : WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED;
        sv_act(u, sv_unit_spawn_failed(u, &g_timing, code, sv_now()));
    }
    sv_spawn_next();
}

/* Writes the unit's watched-spawn request into the spawn buffer and sends it.
 * Returns 0, or -1 with nothing in flight. */
static int sv_spawn_send(int32_t index) {
    sv_unit_t* u = &g_units[index];
    wasmos_spawn_watch_desc_t desc;
    uint32_t path_len = (uint32_t)strlen(u->path);
    uint32_t args_len = (uint32_t)strlen(u->args);

    memset(&desc, 0, sizeof(desc));
    desc.version = SPAWN_WATCH_DESC_VERSION;
    desc.notify_endpoint = (uint32_t)g_ep;
    desc.tty = u->tty;
    desc.path_off = SV_SPAWN_PATH_OFF;
    desc.path_len = path_len;
    desc.args_off = SV_SPAWN_ARGS_OFF;
    desc.args_len = args_len;
    if (wasmos_xfer_buffer_write(g_spawn_bid, &desc, (int32_t)sizeof(desc), 0) != 0 ||
        wasmos_xfer_buffer_write(
            g_spawn_bid, u->path, (int32_t)path_len, (int32_t)SV_SPAWN_PATH_OFF) != 0 ||
        (args_len > 0 &&
         wasmos_xfer_buffer_write(
             g_spawn_bid, u->args, (int32_t)args_len, (int32_t)SV_SPAWN_ARGS_OFF) != 0)) {
        return -1;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               g_proc_ep,
                               g_ep,
                               PROC_IPC_SPAWN_WATCHED,
                               g_spawn_bid,
                               0,
                               (int32_t)SV_SPAWN_REGION,
                               0,
                               sv_on_spawn_reply,
                               (void*)(intptr_t)index,
                               0) != 0) {
        return -1;
    }
    sv_unit_spawn_sent(u);
    g_spawn_unit = index;
    return 0;
}

/* Sends the next due spawn, unless one is in flight. A unit whose request
 * cannot be sent stays due and is retried on the next tick. */
static void sv_spawn_next(void) {
    uint32_t now = sv_now();
    if (g_spawn_unit >= 0) {
        return;
    }
    for (int32_t i = 0; i < SV_UNITS_MAX; ++i) {
        if (g_units[i].state == SV_UNIT_SPAWN_DUE &&
            sv_unit_tick(&g_units[i], &g_timing, now) == SV_VERDICT_SPAWN) {
            if (sv_spawn_send(i) == 0) {
                return;
            }
        }
    }
}

/* -------------------------------------------------------------- deadlines */

/* Runs every unit's due deadline, then starts the next spawn and bounds the
 * loop's park by the earliest deadline left. A due spawn waiting behind the
 * one in flight is not a deadline: the reply wakes the loop for it. */
static void sv_tick(void) {
    uint32_t now = sv_now();
    uint32_t next = 0;
    int have_next = 0;

    for (int32_t i = 0; i < SV_UNITS_MAX; ++i) {
        sv_unit_t* u = &g_units[i];
        if (u->state != SV_UNIT_FREE && u->state != SV_UNIT_SPAWN_DUE) {
            sv_act(u, sv_unit_tick(u, &g_timing, now));
        }
    }
    sv_spawn_next();

    now = sv_now();
    for (int32_t i = 0; i < SV_UNITS_MAX; ++i) {
        uint32_t d = 0;
        if (!sv_unit_deadline(&g_units[i], &d)) {
            continue;
        }
        if (g_units[i].state == SV_UNIT_SPAWN_DUE && g_spawn_unit >= 0) {
            continue;
        }
        if (!have_next || (int32_t)(d - next) < 0) {
            next = d;
            have_next = 1;
        }
    }
    if (!have_next) {
        g_loop.poll_timeout_ms = 0;
    } else if ((int32_t)(next - now) <= 0) {
        g_loop.poll_timeout_ms = 1;
    } else {
        g_loop.poll_timeout_ms = (int32_t)(next - now);
    }
}

/* ---------------------------------------------------------------- events */

static void sv_on_child_ready(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    sv_unit_t* u = sv_unit_by_pid((uint32_t)msg->arg0);
    if (u) {
        sv_unit_ready(u, &g_timing, sv_now());
    }
}

static void sv_on_child_exited(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    sv_unit_t* u = sv_unit_by_pid((uint32_t)msg->arg0);
    if (u) {
        sv_act(u, sv_unit_exited(u, &g_timing, msg->arg1, (uint32_t)msg->arg2, sv_now()));
    }
}

static void sv_reply(const wasmos_ipc_message_t* msg, int32_t code) {
    (void)wasmos_ipc_send(msg->source,
                          g_ep,
                          code == 0 ? SUPERVISOR_IPC_RESP : SUPERVISOR_IPC_ERROR,
                          msg->request_id,
                          code,
                          0,
                          0,
                          0);
}

/* SUPERVISOR_IPC_SUBMIT_REQ: arg0=buffer_id arg1=offset arg2=size of a
 * wasmos_supervisor_submit_desc_t the submitter lent READ. The descriptor is
 * copied once; the unit is recorded and answered before it is spawned. */
static void sv_on_submit(void* user, const wasmos_ipc_message_t* msg) {
    wasmos_supervisor_submit_desc_t desc;
    sv_unit_t unit;
    int32_t free_slot = -1;
    int32_t rc;
    (void)user;

    if (g_fault_trap) {
        sv_log("[supervisor] supervisor.fault=trap: trapping\n");
        __builtin_trap();
    }
    if (msg->arg2 < (int32_t)sizeof(desc) || msg->arg1 < 0 ||
        wasmos_xfer_buffer_read(msg->arg0, &desc, (int32_t)sizeof(desc), msg->arg1) != 0 ||
        desc.version != SUPERVISOR_SUBMIT_DESC_VERSION || desc.spec_len > SUPERVISOR_SPEC_MAX ||
        desc.spec[desc.spec_len] != '\0') {
        sv_reply(msg, WASMOS_ERR_SUPERVISOR_BAD_DESC);
        return;
    }
    sv_load_timing();
    rc = sv_spec_parse(desc.spec, &g_timing, sv_now(), &unit);
    if (rc != 0) {
        sv_reply(msg, rc);
        return;
    }
    for (int32_t i = 0; i < SV_UNITS_MAX; ++i) {
        if (g_units[i].state == SV_UNIT_FREE) {
            if (free_slot < 0) {
                free_slot = i;
            }
        } else if (strcmp(g_units[i].name, unit.name) == 0) {
            sv_reply(msg, WASMOS_ERR_SUPERVISOR_EXISTS);
            return;
        }
    }
    if (free_slot < 0) {
        sv_reply(msg, WASMOS_ERR_SUPERVISOR_NO_SPACE);
        return;
    }
    g_units[free_slot] = unit;
    sv_reply(msg, 0);
}

/* SUPERVISOR_IPC_LIST_REQ: arg0=buffer_id arg1=offset arg2=size of a
 * wasmos_supervisor_list_desc_t and the room for its records, lent READ|WRITE.
 * A capacity the region cannot hold is a bad descriptor; one larger than the
 * table is served up to the table. */
static void sv_on_list(void* user, const wasmos_ipc_message_t* msg) {
    wasmos_supervisor_list_desc_t desc;
    uint32_t room;
    uint32_t capacity;
    uint32_t total = 0;
    uint32_t count;
    (void)user;

    if (msg->arg2 < (int32_t)sizeof(desc) || msg->arg1 < 0 ||
        wasmos_xfer_buffer_read(msg->arg0, &desc, (int32_t)sizeof(desc), msg->arg1) != 0 ||
        desc.version != SUPERVISOR_LIST_DESC_VERSION) {
        sv_reply(msg, WASMOS_ERR_SUPERVISOR_BAD_DESC);
        return;
    }
    room = ((uint32_t)msg->arg2 - (uint32_t)sizeof(desc)) /
           (uint32_t)sizeof(wasmos_supervisor_unit_record_t);
    if (desc.capacity > room) {
        sv_reply(msg, WASMOS_ERR_SUPERVISOR_BAD_DESC);
        return;
    }
    capacity = desc.capacity < SV_UNITS_MAX ? desc.capacity : SV_UNITS_MAX;
    count = sv_units_list(g_units, SV_UNITS_MAX, g_list, capacity, &total);
    desc.total = total;
    desc.count = count;
    if ((count > 0 &&
         wasmos_xfer_buffer_write(msg->arg0,
                                  g_list,
                                  (int32_t)(count * sizeof(wasmos_supervisor_unit_record_t)),
                                  msg->arg1 + (int32_t)sizeof(desc)) != 0) ||
        wasmos_xfer_buffer_write(msg->arg0, &desc, (int32_t)sizeof(desc), msg->arg1) != 0) {
        sv_reply(msg, WASMOS_ERR_SUPERVISOR_BAD_DESC);
        return;
    }
    sv_reply(msg, 0);
}

/* Messages no intent or handler claims are dropped, never answered: an error
 * reply to an unsolicited message can ping-pong with a peer that does the
 * same. */
static void sv_on_unclaimed(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    (void)msg;
}

static void sv_on_registered(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    if (g_register_bid >= 0) {
        (void)wasmos_xfer_buffer_release(g_register_bid);
        g_register_bid = -1;
    }
    if (msg->type != SVC_IPC_REGISTER_RESP) {
        sv_log("[supervisor] registering \"supervisor\" failed\n");
    }
}

static int sv_register(void) {
    svc_register_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = WASMOS_SVC_REGISTER_DESC_VERSION;
    desc.service_endpoint = (uint32_t)g_ep;
    memcpy(desc.name, "supervisor", sizeof("supervisor"));
    g_register_bid = wasmos_xfer_stage(&desc, (int32_t)sizeof(desc));
    if (g_register_bid < 0) {
        g_register_bid = -1;
        return -1;
    }
    if (wasmos_sys_intent_send(&g_loop,
                               g_proc_ep,
                               g_ep,
                               SVC_IPC_REGISTER_DESC_REQ,
                               0,
                               (int32_t)sizeof(desc),
                               g_register_bid,
                               0,
                               sv_on_registered,
                               0,
                               0) != 0) {
        (void)wasmos_xfer_buffer_release(g_register_bid);
        g_register_bid = -1;
        return -1;
    }
    return 0;
}

/* A supervisor that cannot set itself up exits: it is a critical process, so
 * the kernel panics naming it rather than running on with nothing supervised. */
static void sv_fatal(const char* msg) {
    sv_log(msg);
    (void)wasmos_proc_exit(1);
    __builtin_trap();
}

WASMOS_WASM_EXPORT int32_t initialize(void) {
    g_proc_ep = wasmos_startup_proc_endpoint();
    if (g_proc_ep < 0) {
        sv_fatal("[supervisor] invalid init args\n");
    }
    g_ep = wasmos_ipc_create_endpoint();
    if (g_ep < 0) {
        sv_fatal("[supervisor] cannot create the service endpoint\n");
    }
    g_spawn_bid = wasmos_xfer_buffer_acquire((int32_t)SV_SPAWN_REGION);
    if (g_spawn_bid < 0) {
        sv_fatal("[supervisor] cannot acquire the spawn buffer\n");
    }
    sv_load_timing();
    g_fault_trap = sv_fault_is_trap();

    wasmos_sys_event_loop_init(&g_loop, g_ep, SV_LOOP_REQ_BASE);
    (void)wasmos_sys_event_set_default(&g_loop, sv_on_unclaimed, 0);
    if (wasmos_sys_event_register(&g_loop, SUPERVISOR_IPC_SUBMIT_REQ, sv_on_submit, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, SUPERVISOR_IPC_LIST_REQ, sv_on_list, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, PROC_IPC_CHILD_READY, sv_on_child_ready, 0) != 0 ||
        wasmos_sys_event_register(&g_loop, PROC_IPC_CHILD_EXITED, sv_on_child_exited, 0) != 0) {
        sv_fatal("[supervisor] cannot register message handlers\n");
    }
    if (sv_register() != 0) {
        sv_fatal("[supervisor] cannot register \"supervisor\"\n");
    }
    /* Sent after the registration: the process manager serves one sender in
     * order, so "supervisor" is registered by the time anyone sees it ready. */
    (void)wasmos_sys_intent_send(
        &g_loop, g_proc_ep, g_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, sv_on_ignored_reply, 0, 0);

    for (;;) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
        sv_tick();
    }
}
