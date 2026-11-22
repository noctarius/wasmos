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
    /* RETRACT sent; its answer completes the unmount. */
    FSMGR_UNIT_RETRACTING
} fsmgr_unit_state_t;

typedef struct {
    int32_t state; /* fsmgr_unit_state_t */
    char name[SUPERVISOR_UNIT_NAME_MAX + 1];
    char mount[FSMGR_MOUNT_PATH_MAX];
    /* RETIRING: the backend endpoint whose end is awaited, and whether its
     * quiesce has been answered. The unit is retracted only after both. */
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
     * not the backend has ended. */
    uint32_t deadline_ms;
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
 * request or buffer. Returns NULL when the table is full or a unit of that name
 * is already held. */
fsmgr_unit_t* fsmgr_unit_claim(fsmgr_unit_t* table, uint32_t cap, const char* name,
                               const char* mount);

/* The held unit (any state but FREE) serving `mount`, or NULL. */
fsmgr_unit_t* fsmgr_unit_by_mount(fsmgr_unit_t* table, uint32_t cap, const char* mount);

/* The unit whose SUBMIT or RETRACT is request `request_id`, or NULL. */
fsmgr_unit_t* fsmgr_unit_by_request(fsmgr_unit_t* table, uint32_t cap, int32_t request_id);

/* Return a slot to FREE. */
void fsmgr_unit_release(fsmgr_unit_t* unit);

/* Whether sched-time `now_ms` is at or past `deadline_ms`, across the uint32
 * wrap. */
int32_t fsmgr_deadline_passed(uint32_t now_ms, uint32_t deadline_ms);

#endif /* WASMOS_FS_MANAGER_UNITS_H */
