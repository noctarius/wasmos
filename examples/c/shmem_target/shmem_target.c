#include "fcntl.h"
#include "stdio.h"
#include "stdint.h"
#include "string.h"
#include "unistd.h"
#include "wasmos/api.h"

/*
 * shmem_target (shmtgt) — first half of the shmem end-to-end test.
 *
 * Minos2-aligned design: no busy-polling.
 *   1. Write PID + IPC endpoint to the sync file.
 *   2. Call wasmos_proc_notify_ready() so the CLI returns immediately and
 *      shmownr can be spawned concurrently.
 *   3. Block on wasmos_ipc_select_one() waiting for stage signals from shmownr.
 */

#define SHMEM_SYNC_PATH "/boot/shmem_e2e.bin"
#define SHMEM_MAP_OFF   0x2000
#define SHMEM_MAP_SIZE  0x1000

/* Must match shmem_owner.c */
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

/* Block until shmownr sends a stage signal, then read the sync file. */
static int
wait_for_stage(int32_t ep, shmem_sync_t *s)
{
    if (wasmos_ipc_select_one(ep) < 0) return -1;
    return sync_read(s);
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    shmem_sync_t sync;
    memset(&sync, 0, sizeof(sync));

    int32_t my_ep = wasmos_ipc_create_endpoint();
    if (my_ep < 0) {
        puts("[test] shmem e2e target create-ep-failed");
        return 1;
    }

    sync.stage      = -1;
    sync.target_pid = wasmos_sched_current_pid();
    sync.target_ep  = my_ep;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e target write-init-failed");
        return 1;
    }

    /* Detach from the CLI so the prompt returns and shmownr can be launched. */
    wasmos_proc_notify_ready();

    /* Stage 0: shmownr has created the shmem region and signalled us. */
    if (wait_for_stage(my_ep, &sync) != 0 || sync.stage != 0 || sync.shmem_id <= 0) {
        puts("[test] shmem e2e target no-stage0");
        return 1;
    }
    int32_t owner_ep = sync.owner_ep;

    /* Pre-grant policy checks.  Use map_auto for the id/grant policy checks so
     * a deny is always a real policy rejection, never an artefact of WARP's
     * non-page-aligned linear-memory base (a fixed guest offset can't yield a
     * page-aligned host address).  The unaligned-offset check below must stay on
     * the fixed-offset map — an unaligned request is exactly what it verifies. */
    if (wasmos_shmem_map_auto(sync.shmem_id + 0x1234, SHMEM_MAP_SIZE) >= 0) {
        puts("[test] shmem e2e forged id deny mismatch"); return 1;
    }
    puts("[test] shmem e2e forged id deny ok");

    if (wasmos_shmem_map(sync.shmem_id, SHMEM_MAP_OFF + 1, SHMEM_MAP_SIZE) == 0) {
        puts("[test] shmem e2e map policy deny mismatch"); return 1;
    }
    puts("[test] shmem e2e map policy deny ok");

    if (wasmos_shmem_map_auto(sync.shmem_id, SHMEM_MAP_SIZE) >= 0) {
        puts("[test] shmem e2e pregrant deny mismatch"); return 1;
    }
    puts("[test] shmem e2e pregrant deny ok");

    /* Advance to stage 1: tell owner pre-grant checks passed. */
    sync.stage = 1;
    if (sync_write(&sync) != 0) { puts("[test] shmem e2e target write-stage1-failed"); return 1; }
    if (wasmos_ipc_send(owner_ep, my_ep, 1, 0, 0, 0, 0, 0) != 0) {
        puts("[test] shmem e2e target send-stage1-failed"); return 1;
    }

    /* Stage 2: owner has granted the shmem. */
    if (wait_for_stage(my_ep, &sync) != 0 || sync.stage != 2) {
        puts("[test] shmem e2e target no-stage2"); return 1;
    }

    if (wasmos_shmem_map_auto(sync.shmem_id, SHMEM_MAP_SIZE) < 0) {
        puts("[test] shmem e2e grant map failed"); return 1;
    }
    if (wasmos_shmem_unmap(sync.shmem_id) != 0) {
        puts("[test] shmem e2e grant unmap failed"); return 1;
    }
    puts("[test] shmem e2e grant map ok");

    sync.stage = 3;
    if (sync_write(&sync) != 0) { puts("[test] shmem e2e target write-stage3-failed"); return 1; }
    if (wasmos_ipc_send(owner_ep, my_ep, 3, 0, 0, 0, 0, 0) != 0) {
        puts("[test] shmem e2e target send-stage3-failed"); return 1;
    }

    /* Stage 4: owner has revoked the grant. */
    if (wait_for_stage(my_ep, &sync) != 0 || sync.stage != 4) {
        puts("[test] shmem e2e target no-stage4"); return 1;
    }

    if (wasmos_shmem_map_auto(sync.shmem_id, SHMEM_MAP_SIZE) >= 0) {
        puts("[test] shmem e2e revoke deny mismatch"); return 1;
    }
    puts("[test] shmem e2e revoke deny ok");

    sync.stage = 5;
    if (sync_write(&sync) != 0) { puts("[test] shmem e2e target write-stage5-failed"); return 1; }
    if (wasmos_ipc_send(owner_ep, my_ep, 5, 0, 0, 0, 0, 0) != 0) {
        puts("[test] shmem e2e target send-stage5-failed"); return 1;
    }

    /* Stage 90: owner has revoked completely (stale check). */
    if (wait_for_stage(my_ep, &sync) != 0 || sync.stage != 90) {
        puts("[test] shmem e2e target no-stage90"); return 1;
    }

    if (wasmos_shmem_map_auto(sync.shmem_id, SHMEM_MAP_SIZE) >= 0) {
        puts("[test] shmem e2e stale deny mismatch"); return 1;
    }
    puts("[test] shmem e2e stale revoke deny ok");

    sync.stage = 91;
    if (sync_write(&sync) != 0) { puts("[test] shmem e2e target write-stage91-failed"); return 1; }
    if (wasmos_ipc_send(owner_ep, my_ep, 91, 0, 0, 0, 0, 0) != 0) {
        puts("[test] shmem e2e target send-stage91-failed"); return 1;
    }

    return 0;
}
