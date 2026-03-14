#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "sys/stat.h"
#include "unistd.h"

int
main(int argc, char **argv)
{
    static char grow_pattern[1024];
    static const char stdio_initial[] = "STDIO-WRITE\n";
    static const char stdio_append[] = "APPEND\n";
    static const char stdio_expected[] = "STDIO-WRITE\nAPPEND\n";
    static const char original[] = "WASMOS-WRITE-SMOKE-ORIGINAL\n";
    static const char append_suffix[] = "APPEND\n";
    static const char appended[] = "WASMOS-WRITE-SMOKE-ORIGINAL\nAPPEND\n";
    static const char updated[] = "TRUNCATED\n";
    char buffer[sizeof(grow_pattern)];
    FILE *stream;
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
    rc = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rc != (ssize_t)(sizeof(original) - 1u) || memcmp(buffer, original, sizeof(original) - 1u) != 0) {
        puts("fs-write-smoke: original mismatch");
        return 1;
    }

    fd = open("/create.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        puts("fs-write-smoke: create open failed");
        return 1;
    }
    close(fd);
    if (stat("/create.txt", &st) != 0 || st.st_size != 0) {
        puts("fs-write-smoke: create stat failed");
        return 1;
    }
    fd = open("/create.txt", O_RDONLY);
    if (fd < 0) {
        puts("fs-write-smoke: create reopen failed");
        return 1;
    }
    rc = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rc != 0) {
        puts("fs-write-smoke: create verify failed");
        return 1;
    }
    for (size_t i = 0; i < sizeof(grow_pattern); ++i) {
        grow_pattern[i] = (char)('A' + (i % 23u));
    }
    fd = open("/create.txt", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        puts("fs-write-smoke: grow open failed");
        return 1;
    }
    rc = write(fd, grow_pattern, sizeof(grow_pattern));
    close(fd);
    if (rc != (ssize_t)sizeof(grow_pattern)) {
        puts("fs-write-smoke: grow write failed");
        return 1;
    }
    if (stat("/create.txt", &st) != 0 || st.st_size != (off_t)sizeof(grow_pattern)) {
        puts("fs-write-smoke: grow stat failed");
        return 1;
    }
    fd = open("/create.txt", O_RDONLY);
    if (fd < 0) {
        puts("fs-write-smoke: grow reopen failed");
        return 1;
    }
    rc = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rc != (ssize_t)sizeof(grow_pattern) || memcmp(buffer, grow_pattern, sizeof(grow_pattern)) != 0) {
        puts("fs-write-smoke: grow verify failed");
        return 1;
    }
    if (unlink("/create.txt") != 0) {
        puts("fs-write-smoke: unlink failed");
        return 1;
    }
    if (stat("/create.txt", &st) == 0) {
        puts("fs-write-smoke: unlink stat failed");
        return 1;
    }
    fd = open("/create.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        puts("fs-write-smoke: recreate open failed");
        return 1;
    }
    close(fd);
    stream = fopen("/stdio.txt", "wb");
    if (!stream) {
        puts("fs-write-smoke: stdio write open failed");
        return 1;
    }
    if (fwrite(stdio_initial, 1u, sizeof(stdio_initial) - 1u, stream) != sizeof(stdio_initial) - 1u ||
        fclose(stream) != 0) {
        puts("fs-write-smoke: stdio write failed");
        return 1;
    }
    stream = fopen("/stdio.txt", "ab");
    if (!stream) {
        puts("fs-write-smoke: stdio append open failed");
        return 1;
    }
    if (fwrite(stdio_append, 1u, sizeof(stdio_append) - 1u, stream) != sizeof(stdio_append) - 1u ||
        fclose(stream) != 0) {
        puts("fs-write-smoke: stdio append failed");
        return 1;
    }
    stream = fopen("/stdio.txt", "rb");
    if (!stream) {
        puts("fs-write-smoke: stdio read open failed");
        return 1;
    }
    if (fread(buffer, 1u, sizeof(stdio_expected) - 1u, stream) != sizeof(stdio_expected) - 1u ||
        fclose(stream) != 0 ||
        memcmp(buffer, stdio_expected, sizeof(stdio_expected) - 1u) != 0) {
        puts("fs-write-smoke: stdio verify failed");
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

    fd = open("/write_smoke.txt", O_WRONLY | O_APPEND);
    if (fd < 0) {
        puts("fs-write-smoke: append open failed");
        return 1;
    }
    rc = write(fd, append_suffix, sizeof(append_suffix) - 1u);
    close(fd);
    if (rc != (ssize_t)(sizeof(append_suffix) - 1u)) {
        puts("fs-write-smoke: append failed");
        return 1;
    }
    if (stat("/write_smoke.txt", &st) != 0 || st.st_size != (off_t)(sizeof(appended) - 1u)) {
        puts("fs-write-smoke: append stat failed");
        return 1;
    }

    fd = open("/write_smoke.txt", O_RDONLY);
    if (fd < 0) {
        puts("fs-write-smoke: append reopen failed");
        return 1;
    }
    rc = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rc != (ssize_t)(sizeof(appended) - 1u) || memcmp(buffer, appended, sizeof(appended) - 1u) != 0) {
        puts("fs-write-smoke: append verify failed");
        return 1;
    }

    fd = open("/write_smoke.txt", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        puts("fs-write-smoke: final restore open failed");
        return 1;
    }
    rc = write(fd, original, sizeof(original) - 1u);
    close(fd);
    if (rc != (ssize_t)(sizeof(original) - 1u)) {
        puts("fs-write-smoke: final restore failed");
        return 1;
    }

    fd = open("/write_smoke.txt", O_RDONLY);
    if (fd < 0) {
        puts("fs-write-smoke: final reopen failed");
        return 1;
    }
    rc = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rc != (ssize_t)(sizeof(original) - 1u) || memcmp(buffer, original, sizeof(original) - 1u) != 0) {
        puts("fs-write-smoke: final verify failed");
        return 1;
    }

    puts("fs-write-smoke: ok");
    return 0;
}
