#include "fcntl.h"
#include "stdio.h"
#include "stdint.h"
#include "string.h"
#include "unistd.h"
#include "wasmos/api.h"

#define SHMEM_SYNC_PATH "/shmem_e2e.bin"
#define SHMEM_MAP_OFF   0x2000
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
    int spins;

    (void)argc;
    (void)argv;

    memset(&sync, 0, sizeof(sync));
    sync.stage = -1;
    sync.target_pid = wasmos_sched_current_pid();
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup publish-target-failed");
        return 1;
    }

    memset(&sync, 0, sizeof(sync));
    for (spins = 0; spins < 3000; ++spins) {
        if (sync_read(&sync) == 0 && sync.stage == 0 && sync.shmem_id > 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (sync.stage != 0 || sync.shmem_id <= 0) {
        puts("[test] shmem e2e setup no-owner-stage0");
        return 1;
    }

    if (wasmos_shmem_map(sync.shmem_id, SHMEM_MAP_OFF, SHMEM_MAP_SIZE) == 0) {
        puts("[test] shmem e2e pregrant deny mismatch");
        return 1;
    }
    puts("[test] shmem e2e pregrant deny ok");
    sync.stage = 1;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage1-failed");
        return 1;
    }

    for (spins = 0; spins < 3000; ++spins) {
        if (sync_read(&sync) == 0 && sync.stage == 2) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (sync.stage != 2) {
        puts("[test] shmem e2e setup no-owner-stage2");
        return 1;
    }

    if (wasmos_shmem_map(sync.shmem_id, SHMEM_MAP_OFF, SHMEM_MAP_SIZE) != 0) {
        puts("[test] shmem e2e grant map failed");
        return 1;
    }
    if (wasmos_shmem_unmap(sync.shmem_id) != 0) {
        puts("[test] shmem e2e grant map failed");
        return 1;
    }
    puts("[test] shmem e2e grant map ok");
    sync.stage = 3;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage3-failed");
        return 1;
    }

    for (spins = 0; spins < 3000; ++spins) {
        if (sync_read(&sync) == 0 && sync.stage == 4) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (sync.stage != 4) {
        puts("[test] shmem e2e setup no-owner-stage4");
        return 1;
    }

    if (wasmos_shmem_map(sync.shmem_id, SHMEM_MAP_OFF, SHMEM_MAP_SIZE) == 0) {
        puts("[test] shmem e2e revoke deny mismatch");
        return 1;
    }
    puts("[test] shmem e2e revoke deny ok");
    sync.stage = 5;
    if (sync_write(&sync) != 0) {
        puts("[test] shmem e2e setup write-stage5-failed");
        return 1;
    }

    return 0;
}
