/* fs_manager_units.h - the supervised units behind fs-manager's mounts.
 *
 * fs-manager does not spawn the backend of a mount it places. It submits it to
 * the supervisor (SUPERVISOR_IPC_SUBMIT_REQ) as one unit per mount, named
 * `<driver>@<mount path>`, so a backend that dies is restarted and the mount it
 * serves comes back when the successor registers. The name is derived from the
 * mount, never allocated: a mount path is unique by construction, and the same
 * path names the same unit across restarts.
 *
 * This table records the units fs-manager submitted, which is what an unmount
 * retracts (SUPERVISOR_IPC_RETRACT_REQ). A unit is held from its submission
 * until its retraction is answered. The decisions here are pure functions of the
 * table; tests/unit/test_fs_manager_units.c links this translation unit alone.
 */
#ifndef WASMOS_FS_MANAGER_UNITS_H
#define WASMOS_FS_MANAGER_UNITS_H

#include <stdint.h>

#include "fs_manager_types.h"

#define FSMGR_UNIT_CAP FS_BACKEND_CAP
/* How long an unmount waits for its backend -- to answer the quiesce, then to
 * end -- before the unit is retracted anyway, which kills the backend; and how
 * long a forced retraction then waits for that end before the unmount is
 * answered regardless. */
#define FSMGR_RETIRE_TIMEOUT_MS 5000u
/* When a retraction could not be sent, how soon it is tried again. */
#define FSMGR_RETRACT_RETRY_MS 20u

typedef enum {
    FSMGR_UNIT_FREE = 0,
    /* SUBMIT sent; the supervisor has not answered. */
    FSMGR_UNIT_SUBMITTING,
    /* Accepted: the supervisor runs the backend. */
    FSMGR_UNIT_HELD,
    /* Unmounted: the backend was quiesced, and its end is awaited before the
     * unit is retracted, so the retraction finds no instance and frees the name
     * at once (docs/architecture/18-filesystem-stack.md, "Removing a Mount"). */
    FSMGR_UNIT_RETIRING,
    /* RETRACT sent; its answer completes the unmount, or moves it to ENDING. */
    FSMGR_UNIT_RETRACTING,
    /* The retraction killed a live instance; the supervisor keeps the name
     * until that instance has ended, so the unmount is answered only then. */
    FSMGR_UNIT_ENDING
} fsmgr_unit_state_t;

/* What a unit needs next (fsmgr_unit_due, fsmgr_unit_retracted). */
typedef enum {
    FSMGR_UNIT_WAIT = 0,
    /* Send its RETRACT. */
    FSMGR_UNIT_DO_RETRACT,
    /* Answer its unmount with success and release the slot. */
    FSMGR_UNIT_DO_ANSWER,
    /* Answer its unmount with `result` and keep the unit: the supervisor
     * still holds it, and so its name and its path. */
    FSMGR_UNIT_DO_REFUSE
} fsmgr_unit_action_t;

typedef struct {
    int32_t state; /* fsmgr_unit_state_t */
    char name[SUPERVISOR_UNIT_NAME_MAX + 1];
    char mount[FSMGR_MOUNT_PATH_MAX];
    /* RETIRING and ENDING: the backend endpoint whose end is awaited, and
     * whether its quiesce has been answered. */
    int32_t backend;
    int32_t quiesced;
    /* The client and request an unmount answers once the unit is retracted, or
     * client -1 when nobody waits. */
    int32_t client;
    int32_t client_request_id;
    /* The request id of the SUBMIT or RETRACT in flight. */
    int32_t request_id;
    /* The transfer buffer lent to the supervisor for that request, or -1. */
    int32_t buffer_id;
    /* RETIRING: the sched-time (ms) past which the unit is retracted whether or
     * not the backend has answered or ended. ENDING: past which the unmount is
     * answered whether or not it has ended. */
    uint32_t deadline_ms;
    /* DO_REFUSE: the supervisor's packed refusal the unmount is answered with. */
    int32_t result;
} fsmgr_unit_t;

/* Write the unit name for the driver at `module_path` serving `mount` into
 * `out`: the module's file name without its extension, '@', the mount path
 * ("/boot/system/drivers/fs_wfs.wap", "/wfs" -> "fs_wfs@/wfs"). Returns the
 * name's length, or WASMOS_ERR_FS_PATH_TOO_LONG when it would exceed
 * SUPERVISOR_UNIT_NAME_MAX or `cap` -- a unit name is a service name's length,
 * shorter than a mount path may be, and a truncated name would not be unique.
 * `out` is empty on a refusal. */
int32_t fsmgr_unit_name(const char* module_path, const char* mount, char* out, uint32_t cap);

/* Write the SUPERVISOR_IPC_SUBMIT_REQ spec line for unit `name`, running
 * `module_path` restart=on-failure with startup `args`:
 * "<name> <module_path> restart=on-failure -- <args>". Returns its length, or
 * WASMOS_ERR_FS_BAD_ARGS when it would exceed SUPERVISOR_SPEC_MAX or `cap`. */
int32_t fsmgr_unit_spec(const char* name, const char* module_path, const char* args, char* out,
                        uint32_t cap);

/* Claim a free slot for unit `name` at `mount`, SUBMITTING, with no client,
 * request or buffer. Returns NULL when the table is full, or when a unit of that
 * name OR at that mount is already held: a held unit's backend may be restarting
 * or still ending, and a second filesystem at its path would serve it alongside
 * the first once that backend registers. */
fsmgr_unit_t* fsmgr_unit_claim(fsmgr_unit_t* table, uint32_t cap, const char* name,
                               const char* mount);

/* The held unit (any state but FREE) serving `mount`, or NULL. */
fsmgr_unit_t* fsmgr_unit_by_mount(fsmgr_unit_t* table, uint32_t cap, const char* mount);

/* The unit whose SUBMIT or RETRACT is request `request_id`, or NULL. */
fsmgr_unit_t* fsmgr_unit_by_request(fsmgr_unit_t* table, uint32_t cap, int32_t request_id);

/* What a RETIRING or ENDING unit needs at `now_ms`, given whether its backend
 * still has an owner.
 *
 * RETIRING: retract once the quiesce is answered and the backend has ended, or
 * once the deadline passes -- including when the quiesce was never answered, so
 * a backend that ignores it cannot hold the unmount forever. ENDING: answer once
 * the backend has ended, or once the deadline passes. Every other state waits. */
int32_t fsmgr_unit_due(const fsmgr_unit_t* unit, uint32_t now_ms, int32_t backend_alive);

/* Record the supervisor's answer to `unit`'s RETRACT and return what follows.
 * `accepted` is whether it answered SUPERVISOR_IPC_RESP; `code` is its packed
 * refusal otherwise; `pid_killed` is the reply's arg1.
 *
 * - accepted, no instance killed: DO_ANSWER -- the name is already free.
 * - accepted, an instance killed: the unit becomes ENDING with a fresh deadline
 *   and the answer waits for that instance's end (WAIT).
 * - refused WASMOS_ERR_SUPERVISOR_NO_UNIT: DO_ANSWER -- the unit is gone.
 * - any other refusal: the unit is HELD again with `result` set: DO_REFUSE. */
int32_t fsmgr_unit_retracted(fsmgr_unit_t* unit, int32_t accepted, int32_t code, int32_t pid_killed,
                             uint32_t now_ms);

/* A RETRACT for `unit` could not be sent: it is RETIRING again, quiesced, due
 * once FSMGR_RETRACT_RETRY_MS has passed. */
void fsmgr_unit_retract_unsent(fsmgr_unit_t* unit, uint32_t now_ms);

/* Return a slot to FREE. */
void fsmgr_unit_release(fsmgr_unit_t* unit);

/* Whether sched-time `now_ms` is at or past `deadline_ms`, across the uint32
 * wrap. */
int32_t fsmgr_deadline_passed(uint32_t now_ms, uint32_t deadline_ms);

#endif /* WASMOS_FS_MANAGER_UNITS_H */
