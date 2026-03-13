#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "wasmos/imports.h"

static int
contains_token(const char *text, const char *token)
{
    size_t text_len = strlen(text);
    size_t token_len = strlen(token);

    if (token_len == 0 || token_len > text_len) {
        return 0;
    }

    for (size_t i = 0; i + token_len <= text_len; ++i) {
        size_t j = 0;
        while (j < token_len && text[i + j] == token[j]) {
            j++;
        }
        if (j == token_len) {
            return 1;
        }
    }

    return 0;
}

WASMOS_WASM_EXPORT int32_t
main(int32_t arg0,
     int32_t arg1,
     int32_t arg2,
     int32_t arg3)
{
    FILE *file;
    char buffer[128];
    size_t count;

    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;

    file = fopen("/startup.nsh", "r");
    if (!file) {
        puts("fs-open-smoke: open failed");
        return 1;
    }

    count = fread(buffer, 1u, sizeof(buffer) - 1u, file);
    buffer[count] = '\0';
    fclose(file);

    if (count == 0 || !contains_token(buffer, "BOOTX64.EFI")) {
        puts("fs-open-smoke: read failed");
        return 1;
    }

    puts("fs-open-smoke: ok");
    return 0;
}
