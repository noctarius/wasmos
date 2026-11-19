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
} sv_state_t;

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
    uint32_t pid;      /* the running instance; 0 when none */
    uint32_t deadline; /* meaning depends on state (see sv_state_t) */
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
} sv_unit_t;

/* What the caller does next. */
typedef enum {
    SV_VERDICT_NONE = 0,
    SV_VERDICT_SPAWN,    /* spawn the unit now */
    SV_VERDICT_KILL,     /* kill `pid`: it missed its READY deadline */
    SV_VERDICT_PROBE,    /* send a liveness probe to `probe_ep`, then sv_unit_probe_sent */
    SV_VERDICT_HUNG,     /* kill `pid`: it missed probe_misses_max probes in a row */
    SV_VERDICT_RESTART,  /* scheduled: spawn due at `deadline` */
    SV_VERDICT_STOPPED,  /* ended for good by its policy; not a failure */
    SV_VERDICT_FAILED,   /* given up on: log it once */
    SV_VERDICT_CRITICAL, /* a critical unit ended: the system goes down */
} sv_verdict_t;

/* Parses a SUPERVISOR_IPC_SUBMIT_REQ spec into a fresh unit in state
 * SV_UNIT_SPAWN_DUE, due at `now`. Defaults: restart=on-failure,
 * class=optional, no tty pin, the default probe period, no args. Returns 0, or
 * WASMOS_ERR_SUPERVISOR_BAD_SPEC with *out unchanged. */
int32_t sv_spec_parse(const char* spec, const sv_timing_t* timing, uint32_t now, sv_unit_t* out);

/* The spawn request was sent. */
void sv_unit_spawn_sent(sv_unit_t* unit);

/* The spawn succeeded with `pid`; READY is due within ready_timeout_ms. */
void sv_unit_spawned(sv_unit_t* unit, const sv_timing_t* timing, uint32_t pid, uint32_t now);

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
 * supervisor killed -- a missed READY or missed probes -- failed whatever its
 * exit says; sv_unit_exited knows it by the STOPPING state, not this. */
int sv_exit_is_failure(int32_t status, uint32_t reason);

/* Writes the SUPERVISOR_IPC_LIST_REQ records of the occupied slots of
 * `units[0..n)`, in table order, into out[0..capacity). Returns the records
 * written; *total gets the occupied slots, which exceeds the return value
 * when `capacity` was short. */
uint32_t sv_units_list(const sv_unit_t* units, uint32_t n, const sv_timing_t* timing,
                       wasmos_supervisor_unit_record_t* out, uint32_t capacity, uint32_t* total);

const char* sv_state_name(sv_state_t state);
const char* sv_reason_name(uint32_t reason);

#endif
