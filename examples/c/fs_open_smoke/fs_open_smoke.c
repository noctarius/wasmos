#include "string.h"
#include "stdio.h"
#include "sys/stat.h"
#include "unistd.h"
int
main(int argc, char **argv)
{
    FILE *file;
    char buffer[128];
    char tail[18];
    size_t count;
    size_t total = 0;
    struct stat st;

    (void)argc;
    (void)argv;

    if (stat("/large_read.txt", &st) != 0 ||
        st.st_size != 6858u ||
        (st.st_mode & S_IFMT) != S_IFREG) {
        puts("fs-open-smoke: stat failed");
        return 1;
    }

    file = fopen("/large_read.txt", "r");
    if (!file) {
        puts("fs-open-smoke: open failed");
        return 1;
    }

    if (ftell(file) != 0L ||
        fseek(file, 512L, SEEK_SET) != 0 ||
        ftell(file) != 512L ||
        fread(buffer, 1u, 24u, file) != 24u ||
        memcmp(buffer, " FAT chain walking.\nWASM", 24u) != 0 ||
        fseek(file, -18L, SEEK_END) != 0 ||
        ftell(file) != 6840L ||
        fread(tail, 1u, sizeof(tail), file) != sizeof(tail) ||
        memcmp(tail, "END-OF-LARGE-READ\n", sizeof(tail)) != 0 ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        puts("fs-open-smoke: seek failed");
        return 1;
    }

    for (;;) {
        count = fread(buffer, 1u, sizeof(buffer) - 1u, file);
        buffer[count] = '\0';
        total += count;
        if (count + 1u < sizeof(buffer)) {
            break;
        }
    }

    if (total <= 4096u || !feof(file)) {
        fclose(file);
        puts("fs-open-smoke: read failed");
        return 1;
    }
    fclose(file);

    puts("fs-open-smoke: ok");
    return 0;
}
