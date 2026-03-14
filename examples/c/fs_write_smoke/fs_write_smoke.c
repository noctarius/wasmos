#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "sys/stat.h"
#include "unistd.h"

int
main(int argc, char **argv)
{
    static const char original[] = "WASMOS-WRITE-SMOKE-ORIGINAL\n";
    static const char updated[] = "TRUNCATED\n";
    char buffer[sizeof(original)];
    struct stat st;
    int fd;
    ssize_t rc;

    (void)argc;
    (void)argv;

    fd = open("/write_smoke.txt", O_RDONLY);
    if (fd < 0) {
        puts("fs-write-smoke: open read failed");
        return 1;
    }
    rc = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (rc != (ssize_t)(sizeof(buffer) - 1u) || memcmp(buffer, original, sizeof(original) - 1u) != 0) {
        puts("fs-write-smoke: original mismatch");
        return 1;
    }

    fd = open("/write_smoke.txt", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        puts("fs-write-smoke: open write failed");
        return 1;
    }
    rc = write(fd, updated, sizeof(updated) - 1u);
    close(fd);
    if (rc != (ssize_t)(sizeof(updated) - 1u)) {
        puts("fs-write-smoke: write failed");
        return 1;
    }
    if (stat("/write_smoke.txt", &st) != 0 || st.st_size != (off_t)(sizeof(updated) - 1u)) {
        puts("fs-write-smoke: truncate stat failed");
        return 1;
    }

    fd = open("/write_smoke.txt", O_RDONLY);
    if (fd < 0) {
        puts("fs-write-smoke: reopen failed");
        return 1;
    }
    rc = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rc != (ssize_t)(sizeof(updated) - 1u) || memcmp(buffer, updated, sizeof(updated) - 1u) != 0) {
        puts("fs-write-smoke: verify failed");
        return 1;
    }

    fd = open("/write_smoke.txt", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        puts("fs-write-smoke: reopen write failed");
        return 1;
    }
    rc = write(fd, original, sizeof(original) - 1u);
    close(fd);
    if (rc != (ssize_t)(sizeof(original) - 1u)) {
        puts("fs-write-smoke: restore failed");
        return 1;
    }

    fd = open("/write_smoke.txt", O_RDONLY);
    if (fd < 0) {
        puts("fs-write-smoke: final reopen failed");
        return 1;
    }
    rc = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (rc != (ssize_t)(sizeof(buffer) - 1u) || memcmp(buffer, original, sizeof(original) - 1u) != 0) {
        puts("fs-write-smoke: final verify failed");
        return 1;
    }

    puts("fs-write-smoke: ok");
    return 0;
}
