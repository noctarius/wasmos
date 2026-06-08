#include "fcntl.h"
#include "stdio.h"
#include "stdint.h"
#include "string.h"
#include "unistd.h"
#include "wasmos/api.h"

/*
 * shmem_owner (shmownr) — second half of the shmem end-to-end test.
 *
 * Minos2-aligned design: no busy-polling.
 *   shmtgt wrote its IPC endpoint to the sync file before calling
 *   notify_ready.  By the time the CLI shows the prompt and the test
 *   script runs shmownr, the file is guaranteed to contain target_ep.
 *
 *   For each stage: write the updated sync file, send an IPC message to
 *   target_ep to wake shmtgt, then block on owner_ep waiting for shmtgt's
 *   acknowledgement.
 */

#define SHMEM_SYNC_PATH "/boot/shmem_e2e.bin"
#define SHMEM_MAP_OFF   0x1000
#define SHMEM_MAP_SIZE  0x1000

/* Must match shmem_target.c */
typedef struct {
    int32_t stage;
    int32_t shmem_id;
    int32_t owner_pid;
    int32_t target_pid;
    int32_t target_ep;
    int32_t owner_ep;
} shmem_sync_t;

static int
sync_write(const shmem_sync_t *s)
{
    int fd = open(SHMEM_SYNC_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    ssize_t rc = write(fd, s, sizeof(*s));
    close(fd);
    return rc == (ssize_t)sizeof(*s) ? 0 : -1;
}

static int
sync_read(shmem_sync_t *s)
{
    int fd = open(SHMEM_SYNC_PATH, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t rc = read(fd, s, sizeof(*s));
    close(fd);
    return rc == (ssize_t)sizeof(*s) ? 0 : -1;
}

/* Send a stage signal to shmtgt and wait for its acknowledgement. */
static int
signal_and_wait(int32_t owner_ep, int32_t target_ep, int32_t stage)
{
    if (wasmos_ipc_send(target_ep, owner_ep, stage, 0, 0, 0, 0, 0) != 0) return -1;
    if (wasmos_ipc_select_one(owner_ep) < 0) return -1;
    return 0;
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    shmem_sync_t sync;
    memset(&sync, 0, sizeof(sync));

    /*
     * shmtgt wrote {stage=-1, target_pid, target_ep} to the sync file
     * before calling notify_ready.  The CLI shows the prompt only after
     * notify_ready, so by the time we are here the file is ready.
     * A small retry covers any scheduler-ordering edge case.
     */
    int32_t target_ep = -1;
    for (int i = 0; i < 10; ++i) {
        if (sync_read(&sync) == 0 && sync.stage == -1 && sync.target_ep > 0) {
            target_ep = sync.target_ep;
            break;
        }
        wasmos_sched_yield();
    }
    if (target_ep <= 0) {
        puts("[test] shmem e2e setup no-target");
        return 1;
    }
    int32_t target_pid = sync.target_pid;

    int32_t owner_ep = wasmos_ipc_create_endpoint();
    if (owner_ep < 0) {
        puts("[test] shmem e2e setup create-ep-failed");
        return 1;
    }

    int32_t shmem_id = wasmos_shmem_create(1, 0);
    if (shmem_id <= 0) {
        puts("[test] shmem e2e setup create-failed");
        return 1;
    }
    if (wasmos_shmem_map(shmem_id, SHMEM_MAP_OFF, SHMEM_MAP_SIZE) != 0) {
        puts("[test] shmem e2e owner map failed");
        return 1;
    }

    /* Stage 0: shmem created, tell shmtgt to run pre-grant policy checks. */
    sync.stage     = 0;
    sync.shmem_id  = shmem_id;
    sync.owner_pid = wasmos_sched_current_pid();
    sync.target_pid = target_pid;
    sync.target_ep  = target_ep;
    sync.owner_ep   = owner_ep;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage0-failed");
        return 1;
    }
    /* Wake shmtgt; wait for stage-1 ack (pre-grant checks done). */
    if (signal_and_wait(owner_ep, target_ep, 0) != 0) {
        puts("[test] shmem e2e setup no-pregrant-ack");
        return 1;
    }

    /* Stage 2: grant shmem, tell shmtgt it can now map. */
    if (wasmos_shmem_grant(shmem_id, target_pid) != 0) {
        puts("[test] shmem e2e setup grant-failed");
        return 1;
    }
    sync.stage = 2;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage2-failed");
        return 1;
    }
    /* Wake shmtgt; wait for stage-3 ack (map/unmap verified). */
    if (signal_and_wait(owner_ep, target_ep, 2) != 0) {
        puts("[test] shmem e2e setup no-grant-ack");
        return 1;
    }

    /* Stage 4: revoke, tell shmtgt the map should now be denied. */
    if (wasmos_shmem_revoke(shmem_id, target_pid) != 0) {
        puts("[test] shmem e2e setup revoke-failed");
        return 1;
    }
    sync.stage = 4;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage4-failed");
        return 1;
    }
    /* Wake shmtgt; wait for stage-5 ack. */
    if (signal_and_wait(owner_ep, target_ep, 4) != 0) {
        puts("[test] shmem e2e setup no-revoke-ack");
        return 1;
    }

    /* Unmap owner's copy. */
    if (wasmos_shmem_unmap(shmem_id) != 0) {
        puts("[test] shmem e2e owner unmap failed");
        return 1;
    }

    /* Stage 90: fully revoked, tell shmtgt to check stale access is denied. */
    sync.stage = 90;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e stale write-stage90-failed");
        return 1;
    }
    /* Wake shmtgt; wait for stage-91 ack. */
    if (signal_and_wait(owner_ep, target_ep, 90) != 0) {
        puts("[test] shmem e2e stale no-stage91");
        return 1;
    }

    puts("[test] shmem e2e done ok");
    return 0;
}
