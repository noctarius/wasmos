#ifndef WASMOS_SUPERVISOR_UNIT_H
#define WASMOS_SUPERVISOR_UNIT_H

#include <stdint.h>

#include "../../drivers/include/wasmos_driver_abi.h"

/* The supervisor's per-unit policy: what a unit spec says, and what to do when
 * a unit is spawned, becomes READY, ends, or reaches a deadline. No IPC, no
 * clock and no globals: time is passed in as milliseconds and every decision
 * is returned as a verdict for supervisor.c to carry out, so the rules are
 * testable on the host.
 *
 * Times are uint32_t milliseconds from an arbitrary origin and compared with
 * wrap-safe differences, so a run longer than 49 days stays ordered as long as
 * no single interval exceeds 24 days. */

#define SV_UNIT_NAME_MAX 36u /* SUPERVISOR_UNIT_NAME_MAX + NUL */
#define SV_PATH_MAX 256u     /* SPAWN_WATCH_PATH_MAX + NUL */
#define SV_ARGS_MAX 256u     /* SPAWN_WATCH_ARGS_MAX + NUL */
/* A unit path of this prefix names a boot module by app name instead of a file
 * (SPAWN_WATCH_FLAG_MODULE): "module:fs-manager". */
#define SV_MODULE_PREFIX "module:"
#define SV_MODULE_PREFIX_LEN 7u
/* Ends the intensity window can hold; the configured limit must not
 * exceed it. */
#define SV_INTENSITY_SLOTS 16u
/* A unit's probe endpoint when its running instance named none
 * (IPC_ENDPOINT_NONE, as PROC_IPC_CHILD_READY carries it). */
#define SV_PROBE_NONE 0xffffffffu

/* The enums below take their values from abi/constants.yaml, so a unit's
 * fields are reported over SUPERVISOR_IPC_LIST_REQ as they are. */
typedef enum {
    SV_RESTART_NEVER = SUPERVISOR_RESTART_NEVER,
    SV_RESTART_ON_FAILURE = SUPERVISOR_RESTART_ON_FAILURE,
    SV_RESTART_ALWAYS = SUPERVISOR_RESTART_ALWAYS,
} sv_restart_t;

/* What the death of a unit costs the system once it can no longer be
 * restarted. CRITICAL units are never restarted: their first death ends the
 * system. */
typedef enum {
    SV_CLASS_OPTIONAL = SUPERVISOR_CLASS_OPTIONAL,
    SV_CLASS_REQUIRED = SUPERVISOR_CLASS_REQUIRED,
    SV_CLASS_CRITICAL = SUPERVISOR_CLASS_CRITICAL,
} sv_class_t;

typedef enum {
    SV_UNIT_FREE = 0, /* table slot unused; never reported */
    /* to be spawned once `deadline` has passed */
    SV_UNIT_SPAWN_DUE = SUPERVISOR_UNIT_STATE_SPAWN_DUE,
    /* spawn request in flight; no pid yet */
    SV_UNIT_SPAWNING = SUPERVISOR_UNIT_STATE_SPAWNING,
    /* spawned; READY due by `deadline` */
    SV_UNIT_STARTING = SUPERVISOR_UNIT_STATE_STARTING,
    /* READY; counts as stable at `deadline` */
    SV_UNIT_RUNNING = SUPERVISOR_UNIT_STATE_RUNNING,
    /* READY and survived stable_ms */
    SV_UNIT_STABLE = SUPERVISOR_UNIT_STATE_STABLE,
    /* killed by the supervisor; its exit is due */
    SV_UNIT_STOPPING = SUPERVISOR_UNIT_STATE_STOPPING,
    /* ended and, by its policy, not restarted */
    SV_UNIT_STOPPED = SUPERVISOR_UNIT_STATE_STOPPED,
    /* given up on: restart limit reached, or a critical unit died */
    SV_UNIT_FAILED = SUPERVISOR_UNIT_STATE_FAILED,
    /* held stopped because its parent stopped or was given up on */
    SV_UNIT_DEGRADED = SUPERVISOR_UNIT_STATE_DEGRADED,
} sv_state_t;

/* Why the supervisor is ending a unit's instance for a reason other than the
 * instance's own failure. Ordered: a later reason overrides an earlier one
 * still pending, never the reverse. None of them counts against the unit. */
typedef enum {
    SV_STOP_NONE = 0,
    SV_STOP_FOLLOW,  /* its parent ended and restarts: respawn at once */
    SV_STOP_DEGRADE, /* its parent will not run again: hold it DEGRADED */
    SV_STOP_STOP,    /* stopped on request (SUPERVISOR_IPC_STOP_REQ): hold it STOPPED */
    SV_STOP_RETRACT, /* withdrawn (SUPERVISOR_IPC_RETRACT_REQ): free the slot */
} sv_stop_t;

/* What happened to a unit's parent, as its children are told. */
typedef enum {
    SV_PARENT_RESTARTING = 0, /* its instance ended and another one follows */
    SV_PARENT_GONE,           /* it stopped, failed or is degraded: no instance follows */
    SV_PARENT_STARTING,       /* it was started again after it was gone */
} sv_parent_event_t;

/* Policy numbers, in milliseconds; supervisor.c scales them. */
typedef struct {
    uint32_t ready_timeout_ms; /* spawn to READY, else a failure */
    uint32_t stable_ms;        /* READY to stable; resets the backoff */
    uint32_t backoff_min_ms;
    uint32_t backoff_max_ms;
    uint32_t intensity_max;       /* ends within the window that give up */
    uint32_t intensity_window_ms; /* sliding window over end times */
    uint32_t probe_interval_ms;   /* liveness probe period, unless the spec names one */
    uint32_t probe_misses_max;    /* probes missed in a row that kill the unit */
    uint32_t scale;               /* the factor already applied; a spec's probe= gets it too */
} sv_timing_t;

typedef struct {
    sv_state_t state;
    sv_restart_t restart;
    sv_class_t cls;
    uint32_t tty; /* PROC_SPAWN_PATH_TTY encoding: 0 = unpinned, else slot + 1 */
    char name[SV_UNIT_NAME_MAX];
    char path[SV_PATH_MAX];
    char args[SV_ARGS_MAX];
    /* The unit this one is bound to (spec parent=), empty when none. The
     * parent was held when this unit was admitted, and a parent outlives its
     * children: retracting it retracts them first. */
    char parent[SV_UNIT_NAME_MAX];
    sv_stop_t stop;                    /* pending stop of the instance, if any */
    uint32_t caps_len;                 /* 0 = no hardware access */
    uint8_t caps[SUPERVISOR_CAPS_MAX]; /* wasmos_spawn_caps_v2_t + region grants */
    uint32_t pid;                      /* the running instance; 0 when none */
    uint32_t deadline;                 /* meaning depends on state (see sv_state_t) */
    uint32_t backoff_ms;
    uint32_t ends_at[SV_INTENSITY_SLOTS]; /* ring of recent counted ends */
    uint32_t ends_head;                   /* next slot of ends_at */
    uint32_t ends_recorded;               /* valid entries in ends_at */
    uint32_t restart_count;               /* restarts over the unit's lifetime */
    int32_t last_status;                  /* exit status, or spawn error code */
    uint32_t last_reason;                 /* PROC_EXIT_REASON_* */
    /* Liveness. A running instance is probed only when its READY named an
     * endpoint; one probe is outstanding at a time, and a probe still
     * unanswered when the next is due is a miss. */
    uint32_t probe_s;         /* spec probe=<seconds>; 0 = the default period */
    uint32_t probe_ep;        /* the running instance's; SV_PROBE_NONE when none */
    uint32_t probe_due;       /* when the next probe is sent */
    uint32_t probe_pending;   /* request id of the outstanding probe; 0 when none */
    uint32_t probes_answered; /* by the current or last instance */
    uint32_t probe_misses;    /* in a row, by the current or last instance */
    uint8_t boot;             /* held when the boot set was submitted */
} sv_unit_t;

/* What the caller does next. */
typedef enum {
    SV_VERDICT_NONE = 0,
    SV_VERDICT_SPAWN,     /* spawn the unit now */
    SV_VERDICT_KILL,      /* kill `pid`: it missed its READY deadline */
    SV_VERDICT_PROBE,     /* send a liveness probe to `probe_ep`, then sv_unit_probe_sent */
    SV_VERDICT_HUNG,      /* kill `pid`: it missed probe_misses_max probes in a row */
    SV_VERDICT_RESTART,   /* scheduled: spawn due at `deadline` */
    SV_VERDICT_STOPPED,   /* ended for good by its policy; not a failure */
    SV_VERDICT_FAILED,    /* given up on: log it once */
    SV_VERDICT_CRITICAL,  /* a critical unit ended: the system goes down */
    SV_VERDICT_CASCADE,   /* kill `pid` for `stop`; its end is not counted against it */
    SV_VERDICT_FOLLOW,    /* stopped for its parent's restart: spawn due now, not counted */
    SV_VERDICT_DEGRADED,  /* held DEGRADED until its parent runs again */
    SV_VERDICT_RETRACTED, /* withdrawn: the slot is FREE, the name still readable */
    SV_VERDICT_START,     /* started on request or with its parent: spawn due now */
} sv_verdict_t;

/* Parses a SUPERVISOR_IPC_SUBMIT_REQ spec into a fresh unit in state
 * SV_UNIT_SPAWN_DUE, due at `now`. Defaults: restart=on-failure,
 * class=optional, no tty pin, the default probe period, no parent, no args. The path is
 * absolute, or SV_MODULE_PREFIX and a boot module's app name, which takes no
 * args and no tty pin. Returns 0, or WASMOS_ERR_SUPERVISOR_BAD_SPEC with *out
 * unchanged. */
int32_t sv_spec_parse(const char* spec, const sv_timing_t* timing, uint32_t now, sv_unit_t* out);

/* The boot module a unit names (the app name after SV_MODULE_PREFIX), or NULL
 * for a unit spawned from a file. */
const char* sv_unit_module(const sv_unit_t* unit);

/* Gives the unit the capability descriptor a submission carried (len bytes of
 * wasmos_spawn_caps_v2_t and its I/O ranges), which every spawn of it passes to
 * the PM; len 0 leaves it without hardware access. The PM validates the
 * descriptor itself. Returns 0, WASMOS_ERR_SUPERVISOR_BAD_DESC for a length
 * outside the header..SUPERVISOR_CAPS_MAX, or WASMOS_ERR_SUPERVISOR_DMA_CAPS for
 * a DMA request that names no PCI function for the kernel to quiesce, with the
 * unit unchanged. */
int32_t sv_unit_set_caps(sv_unit_t* unit, const uint8_t* caps, uint32_t len);

/* The spawn request was sent. */
void sv_unit_spawn_sent(sv_unit_t* unit);

/* The spawn succeeded with `pid`; READY is due within ready_timeout_ms. A stop
 * that became pending while the spawn was in flight applies to the new
 * instance at once: CASCADE, and the unit moves to STOPPING. NONE otherwise. */
sv_verdict_t sv_unit_spawned(sv_unit_t* unit, const sv_timing_t* timing, uint32_t pid,
                             uint32_t now);

/* The spawn was refused with packed `code`: an exit that never started. */
sv_verdict_t sv_unit_spawn_failed(sv_unit_t* unit, const sv_timing_t* timing, int32_t code,
                                  uint32_t now);

/* The unit sent READY, naming `probe_ep` for liveness probes or SV_PROBE_NONE.
 * A probed unit's first probe is due at once, and it is not stable before it
 * has answered one. A READY that does not belong to the starting instance is
 * ignored. */
void sv_unit_ready(sv_unit_t* unit, const sv_timing_t* timing, uint32_t probe_ep, uint32_t now);

/* The running instance ended with `status` and PROC_EXIT_REASON_* `reason`. */
sv_verdict_t sv_unit_exited(sv_unit_t* unit, const sv_timing_t* timing, int32_t status,
                            uint32_t reason, uint32_t now);

/* Kill on request (SUPERVISOR_IPC_KILL_REQ): a unit with a spawned instance --
 * starting, running or stable -- moves to STOPPING, and the caller kills `pid`;
 * the instance's end is then a failure, as for a missed READY. Returns 0, or
 * WASMOS_ERR_SUPERVISOR_NOT_RUNNING with the unit unchanged. */
int32_t sv_unit_kill(sv_unit_t* unit);

/* The unit's parent restarts or is gone (sv_verdict_parent_event). A running
 * instance moves to STOPPING with the matching stop pending, and the caller
 * kills `pid` (CASCADE); an instance being spawned or killed gets the stop
 * pending and is settled when the spawn answers or the instance ends. A unit
 * waiting to be spawned is left due when the parent restarts -- start order
 * is not a correctness property (docs/architecture/03, invariant 5) -- and
 * becomes DEGRADED when it is gone. A critical unit stopped this way ends the
 * system as any end of it does (CRITICAL). Stopped, failed and degraded units
 * are left as they are, except that a DEGRADED unit is due again at once
 * (START) when its parent starts. Units in any other state ignore a start. */
sv_verdict_t sv_unit_parent_event(sv_unit_t* unit, const sv_timing_t* timing,
                                  sv_parent_event_t event, uint32_t now);

/* Withdraws the unit (SUPERVISOR_IPC_RETRACT_REQ). A unit without an instance
 * is freed at once (*verdict RETRACTED); a running one moves to STOPPING and
 * the caller kills `pid` (CASCADE); one being spawned or killed is freed when
 * that settles. Returns 0, WASMOS_ERR_SUPERVISOR_NO_UNIT for a free slot, or
 * WASMOS_ERR_SUPERVISOR_CRITICAL for a critical unit, which is left
 * unchanged. The caller retracts the unit's children
 * first (sv_units_subtree_critical refuses the whole tree up front). */
int32_t sv_unit_retract(sv_unit_t* unit, sv_verdict_t* verdict);

/* Stops the unit on request (SUPERVISOR_IPC_STOP_REQ): it is not restarted
 * and its end is not counted against it. A running instance moves to STOPPING
 * and the caller kills `pid` (*verdict CASCADE); a unit waiting to be spawned
 * stops at once (STOPPED); one being spawned or killed stops when that
 * settles (NONE), unless it is being retracted. Returns 0,
 * WASMOS_ERR_SUPERVISOR_NOT_RUNNING for a unit already stopped, failed or
 * degraded, WASMOS_ERR_SUPERVISOR_NO_UNIT for a free slot, or
 * WASMOS_ERR_SUPERVISOR_CRITICAL for a critical unit, each with the unit
 * unchanged. The caller refuses a unit with a critical descendant up front
 * (sv_units_subtree_critical): its children are degraded. */
int32_t sv_unit_stop(sv_unit_t* unit, sv_verdict_t* verdict);

/* Starts a stopped, failed or degraded unit (SUPERVISOR_IPC_START_REQ): it is
 * due at `now` with its intensity window and backoff cleared, as if newly
 * submitted; its restart count is kept. `parent` is the unit's parent= unit,
 * or NULL when it names none. The caller then carries out SV_VERDICT_START,
 * which brings back its degraded children. Returns 0,
 * WASMOS_ERR_SUPERVISOR_RUNNING for a unit in any other state,
 * WASMOS_ERR_SUPERVISOR_NO_UNIT for a free slot, or
 * WASMOS_ERR_SUPERVISOR_PARENT_DOWN when the parent is stopped, failed or
 * degraded, each with the unit unchanged. */
int32_t sv_unit_start(sv_unit_t* unit, const sv_unit_t* parent, const sv_timing_t* timing,
                      uint32_t now);

/* What a unit's verdict means to its children: 1 with *event set when they
 * are told (sv_unit_parent_event), 0 when the verdict leaves them alone. */
int sv_verdict_parent_event(sv_verdict_t verdict, sv_parent_event_t* event);

/* Runs the unit's deadline if it has passed: a due spawn (SPAWN), a missed
 * READY (KILL, the unit moves to STOPPING), or a run that became stable (the
 * backoff resets, NONE). */
sv_verdict_t sv_unit_tick(sv_unit_t* unit, const sv_timing_t* timing, uint32_t now);

/* Runs the unit's liveness probe if it is due: PROBE to send the next one, or
 * HUNG when the outstanding one makes probe_misses_max misses in a row (the
 * unit moves to STOPPING). NONE for a unit that is not running or not
 * probed. */
sv_verdict_t sv_unit_probe_tick(sv_unit_t* unit, const sv_timing_t* timing, uint32_t now);

/* The probe with `request_id` was sent, or its send failed -- which the next
 * tick counts as a miss. The next probe is due one period from `now`. */
void sv_unit_probe_sent(sv_unit_t* unit, const sv_timing_t* timing, uint32_t request_id,
                        uint32_t now);

/* A HEALTH_RESP with `request_id` arrived. Returns 1 when it answers the
 * outstanding probe: the misses reset, and a running unit past its stable
 * deadline becomes stable. A late answer to an earlier probe returns 0. */
int sv_unit_probe_answered(sv_unit_t* unit, const sv_timing_t* timing, uint32_t request_id,
                           uint32_t now);

/* The probe period of the unit, in milliseconds. */
uint32_t sv_unit_probe_interval(const sv_unit_t* unit, const sv_timing_t* timing);

/* 1 when the unit has a deadline -- its state's, or its next probe's -- with
 * the earliest in *out. */
int sv_unit_deadline(const sv_unit_t* unit, uint32_t* out);

/* A failure is any end other than a clean exit with status 0. An instance the
 * supervisor killed -- a missed READY, missed probes, or a kill on request --
 * failed whatever its
 * exit says; sv_unit_exited knows it by the STOPPING state, not this. */
int sv_exit_is_failure(int32_t status, uint32_t reason);

/* Writes the SUPERVISOR_IPC_LIST_REQ records of the occupied slots of
 * `units[0..n)`, in table order, into out[0..capacity). Returns the records
 * written; *total gets the occupied slots, which exceeds the return value
 * when `capacity` was short. */
uint32_t sv_units_list(const sv_unit_t* units, uint32_t n, const sv_timing_t* timing,
                       wasmos_supervisor_unit_record_t* out, uint32_t capacity, uint32_t* total);

/* The index of the held unit named `name`, or -1. */
int32_t sv_units_find(const sv_unit_t* units, uint32_t n, const char* name);

/* Records a parsed `unit` in a free slot of units[0..n), *slot naming it.
 * Refused with WASMOS_ERR_SUPERVISOR_EXISTS when its name is held,
 * WASMOS_ERR_SUPERVISOR_NO_PARENT when it names a parent that is not held or
 * is being retracted, and WASMOS_ERR_SUPERVISOR_NO_SPACE when no slot is
 * free. A unit whose parent is stopped, failed or degraded is recorded
 * DEGRADED instead of due. */
int32_t sv_units_admit(sv_unit_t* units, uint32_t n, const sv_unit_t* unit, uint32_t* slot);

/* 1 when units[index] or any unit bound to it through parent=, at any depth,
 * is critical: retracting it would end the system. */
int sv_units_subtree_critical(const sv_unit_t* units, uint32_t n, uint32_t index);

/* Up: READY, and -- when probed -- a probe answered, which shows the loop
 * runs. Unlike stable, up does not wait out stable_ms. */
int sv_unit_is_up(const sv_unit_t* unit);

/* Where the boot milestone stands over the boot units (`boot` set) that are
 * required or critical. */
typedef enum {
    SV_BOOT_PENDING = 0,
    SV_BOOT_COMPLETE, /* every one of them is up; *count gets how many */
    SV_BOOT_FAILED,   /* one failed or stopped, so it will not come up; *index names it */
} sv_boot_t;

sv_boot_t sv_boot_check(const sv_unit_t* units, uint32_t n, uint32_t* count, uint32_t* index);

/* What the sender of a request holds, as supervisor.c learns it from the
 * kernel for the request's source endpoint. */
#define SV_SENDER_CONTROL (1u << 0) /* svc.control */
#define SV_SENDER_GRANT (1u << 1)   /* spawn.grant */

/* Whether a sender holding `sender` (SV_SENDER_*) may make request `type`
 * (SUPERVISOR_IPC_*_REQ). Submitting, killing, retracting, stopping,
 * starting and declaring the boot set change what runs, so they require
 * svc.control; listing is open to everyone. A
 * submission carrying a capability descriptor (`caps_len` > 0) also requires
 * spawn.grant: the supervisor spawns the unit with those grants under its own
 * name, so it relays them only from a submitter the process manager would accept
 * them from directly. Returns 0, WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED, or
 * WASMOS_ERR_SUPERVISOR_GRANT_NOT_AUTHORIZED. */
int32_t sv_authorize(uint32_t type, uint32_t sender, uint32_t caps_len);

const char* sv_state_name(sv_state_t state);
const char* sv_reason_name(uint32_t reason);

#endif
