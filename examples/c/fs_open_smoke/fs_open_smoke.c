#include "stdio.h"
int
main(int argc, char **argv)
{
    FILE *file;
    char buffer[128];
    size_t count;
    size_t total = 0;

    (void)argc;
    (void)argv;

    file = fopen("/large_read.txt", "r");
    if (!file) {
        puts("fs-open-smoke: open failed");
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
