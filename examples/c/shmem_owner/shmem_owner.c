#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "wasmos/api.h"

#define SHMEM_SYNC_PATH "/shmem_e2e.bin"
#define SHMEM_MAP_OFF   0x1000
#define SHMEM_MAP_SIZE  0x1000

typedef struct {
    int32_t stage;
    int32_t shmem_id;
    int32_t owner_pid;
    int32_t target_pid;
} shmem_sync_t;

static int
sync_write(const shmem_sync_t *sync)
{
    int fd = open(SHMEM_SYNC_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return -1;
    }
    ssize_t rc = write(fd, sync, sizeof(*sync));
    close(fd);
    return rc == (ssize_t)sizeof(*sync) ? 0 : -1;
}

static int
sync_read(shmem_sync_t *sync)
{
    int fd = open(SHMEM_SYNC_PATH, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t rc = read(fd, sync, sizeof(*sync));
    close(fd);
    return rc == (ssize_t)sizeof(*sync) ? 0 : -1;
}

int
main(int argc, char **argv)
{
    shmem_sync_t sync;
    int32_t shmem_id;
    int32_t target_pid;
    int spins;

    (void)argc;
    (void)argv;

    memset(&sync, 0, sizeof(sync));
    target_pid = -1;
    for (spins = 0; spins < 3000; ++spins) {
        if (sync_read(&sync) == 0 && sync.stage == -1 && sync.target_pid > 0) {
            target_pid = sync.target_pid;
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (target_pid <= 0) {
        puts("[test] shmem e2e setup no-target");
        return 1;
    }

    shmem_id = wasmos_shmem_create(1, 0);
    if (shmem_id <= 0) {
        puts("[test] shmem e2e setup create-failed");
        return 1;
    }
    if (wasmos_shmem_map(shmem_id, SHMEM_MAP_OFF, SHMEM_MAP_SIZE) != 0) {
        puts("[test] shmem e2e owner map failed");
        return 1;
    }

    sync.stage = 0;
    sync.shmem_id = shmem_id;
    sync.owner_pid = wasmos_sched_current_pid();
    sync.target_pid = target_pid;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage0-failed");
        return 1;
    }

    for (spins = 0; spins < 3000; ++spins) {
        if (sync_read(&sync) == 0 && sync.stage >= 1) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (sync.stage < 1) {
        puts("[test] shmem e2e setup no-pregrant-ack");
        return 1;
    }

    if (wasmos_shmem_grant(shmem_id, target_pid) != 0) {
        puts("[test] shmem e2e setup grant-failed");
        return 1;
    }
    sync.stage = 2;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage2-failed");
        return 1;
    }

    for (spins = 0; spins < 3000; ++spins) {
        if (sync_read(&sync) == 0 && sync.stage >= 3) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (sync.stage < 3) {
        puts("[test] shmem e2e setup no-grant-ack");
        return 1;
    }

    if (wasmos_shmem_revoke(shmem_id, target_pid) != 0) {
        puts("[test] shmem e2e setup revoke-failed");
        return 1;
    }
    sync.stage = 4;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage4-failed");
        return 1;
    }

    for (spins = 0; spins < 3000; ++spins) {
        if (sync_read(&sync) == 0 && sync.stage >= 5) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (sync.stage < 5) {
        puts("[test] shmem e2e setup no-revoke-ack");
        return 1;
    }

    if (wasmos_shmem_unmap(shmem_id) != 0) {
        puts("[test] shmem e2e owner map failed");
        return 1;
    }

    puts("[test] shmem e2e done ok");
    return 0;
}
