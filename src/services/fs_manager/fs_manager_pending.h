/* fs_manager_pending.h - the FS manager's table of requests in flight to backends
 *
 * fs-manager never waits for a backend. Every request it sends one is recorded
 * here and the service returns to its loop; the backend's answer arrives on the
 * reply endpoint like any other message, is matched to its record by request id,
 * and completes or advances it. A backend that holds a request -- a filesystem
 * waiting out a restart of its disk driver -- therefore holds only that request,
 * not the service: fs-manager keeps serving everyone else, including the process
 * manager's module reads that the restart itself depends on
 * (docs/architecture/18-filesystem-stack.md, "Deferred Replies").
 *
 * The request id a record carries is fs-manager's own, not the client's. Two
 * clients may use the same id and both be in flight at one backend, and the
 * replies would be indistinguishable; the client's id is kept beside it and is
 * what the client is answered with.
 *
 * Split out of fs_manager.c, which cannot be linked without IPC and transfer-
 * buffer stubs, so the table's bookkeeping is testable on its own.
 * tests/unit/test_fs_manager_pending.c links this translation unit alone.
 */
#ifndef WASMOS_FS_MANAGER_PENDING_H
#define WASMOS_FS_MANAGER_PENDING_H

#include <stdint.h>

#include "fs_manager_types.h"

/* Requests in flight at once, across all backends. A client request that finds
 * the table full is refused with WASMOS_ERR_FS_BUSY, which is retryable. Sized
 * well above what the clients of a running system issue together, because what
 * fills it is a backend holding requests, and a refusal is all it would cost. */
#define FSMGR_PENDING_CAP 64u

/* What a record is waiting for, and so what its reply completes. */
typedef enum {
    /* A client request relayed as it came: the reply goes back to the client.
     * An OPEN's reply allocates the client fd, a CLOSE's releases it. */
    FSMGR_STEP_FORWARD = 1,
    /* A close fs-manager sends of its own accord, for a backend fd no client
     * holds (an OPEN that succeeded for a client that ended or had no fd free).
     * Nobody is answered. */
    FSMGR_STEP_DISCARD_CLOSE,
    /* READ_PATH is an open, a read and a close; the record carries the backend
     * fd and the outcome from one step to the next. */
    FSMGR_STEP_READ_PATH_OPEN,
    FSMGR_STEP_READ_PATH_READ,
    FSMGR_STEP_READ_PATH_CLOSE,
    /* READDIR of the client's working directory. */
    FSMGR_STEP_READDIR,
    /* The STAT that validates a CHDIR target; the target is committed on its
     * reply. */
    FSMGR_STEP_CHDIR_STAT,
    /* A mount point created after a registration. Nobody is answered. */
    FSMGR_STEP_MKDIR,
    /* A backend's identity, pulled when it joins the backend class. */
    FSMGR_STEP_BACKEND_INFO,
    /* The quiesce of a backend being unmounted. */
    FSMGR_STEP_UNMOUNT,
} fsmgr_step_t;

typedef struct {
    uint8_t in_use;
    uint8_t step; /* fsmgr_step_t */
    int32_t id;   /* fs-manager's request id at the backend; positive */
    int32_t backend;
    /* The endpoint answered when the record completes, or -1 when nobody is:
     * a request fs-manager made of its own accord, or one whose client ended
     * while it was in flight (fsmgr_pending_orphan_client). */
    int32_t client;
    int32_t client_request_id;
    /* The client's context, which its state is keyed by; -1 alongside client. */
    int32_t context;
    int32_t type; /* FORWARD: the request type relayed */
    /* A grant fs-manager lent the backend for this request, unborrowed when the
     * record ends; -1 when there is none. */
    int32_t borrow;
    /* A transfer buffer fs-manager owns for this request, released when the
     * record ends (which cascade-revokes any grant on it); -1 when none. */
    int32_t buffer;
    int32_t client_fd;     /* FORWARD of an fd op: the client's fd */
    int32_t client_buffer; /* CHDIR: where the resulting cwd is reported */
    int32_t fd;            /* READ_PATH: the backend fd once opened */
    int32_t read_cap;      /* READ_PATH: bytes the read may return */
    int32_t pid;           /* BACKEND_INFO: the backend's process, or 0 */
    /* READ_PATH: bytes read once the read succeeded, or the packed code the
     * client is answered with after the close. */
    int32_t result;
    char path[FSMGR_CWD_MAX]; /* CHDIR: the canonical target */
} fsmgr_pending_t;

typedef struct {
    fsmgr_pending_t slots[FSMGR_PENDING_CAP];
    /* The id the next record gets, unless a record in flight holds it. */
    int32_t next_id;
} fsmgr_pending_table_t;

/* Claim a record for `step` at `backend`, with a fresh id and no client, borrow
 * or buffer. Returns NULL when the table is full. */
fsmgr_pending_t* fsmgr_pending_alloc(fsmgr_pending_table_t* table, fsmgr_step_t step,
                                     int32_t backend);

/* Give `entry` a fresh id for the next request of a chain (READ_PATH), so a late
 * reply to the previous step can never be read as the answer to this one.
 * Returns the new id. */
int32_t fsmgr_pending_renew(fsmgr_pending_table_t* table, fsmgr_pending_t* entry);

/* The record in flight under `id`, or NULL. */
fsmgr_pending_t* fsmgr_pending_find(fsmgr_pending_table_t* table, int32_t id);

/* The first record in flight at `backend`, or NULL. A caller failing every
 * record of a backend that ended frees each one it is handed before asking
 * again. */
fsmgr_pending_t* fsmgr_pending_next_for_backend(fsmgr_pending_table_t* table, int32_t backend);

/* The client endpoint `endpoint` ended: its records stay in flight, since their
 * replies still arrive and what fs-manager lent for them is still lent, but
 * nobody is answered and no client state is touched when they complete.
 * Returns the number of records affected. */
uint32_t fsmgr_pending_orphan_client(fsmgr_pending_table_t* table, int32_t endpoint);

/* Records in flight. */
uint32_t fsmgr_pending_count(const fsmgr_pending_table_t* table);

/* Return `entry` to the table. The caller has already released whatever it
 * held; the table touches no grant or buffer. */
void fsmgr_pending_free(fsmgr_pending_t* entry);

#endif
