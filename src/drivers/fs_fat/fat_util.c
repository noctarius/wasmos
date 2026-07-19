/* fat_util.c - small shared helpers for the FAT backend.  See fat_util.h. */
#include "fat_util.h"
#include "ctype.h"
#include "string.h"
#include "wasmos/api.h"

void fat_log(const char* msg) {
    const char* prefix = "[fat] ";
    int32_t n;
    wasmos_console_write(addr_cast(int32_t, prefix), 6);
    n = fat_str_len(msg);
    if (n > 0) {
        wasmos_console_write(addr_cast(int32_t, msg), n);
    }
}

char fat_to_upper(char c) {
    return (char)toupper((unsigned char)c);
}

int32_t fat_str_len(const char* s) {
    return (int32_t)strlen(s);
}

void fat_unpack_name(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char* out,
                     uint32_t out_len) {
    uint32_t args[4] = {arg0, arg1, arg2, arg3};
    uint32_t pos = 0;
    for (uint32_t i = 0; i < 4 && pos + 1 < out_len; ++i) {
        uint32_t v = args[i];
        for (uint32_t b = 0; b < 4 && pos + 1 < out_len; ++b) {
            char c = (char)(v & 0xFF);
            if (c == '\0') {
                out[pos] = '\0';
                return;
            }
            out[pos++] = fat_to_upper(c);
            v >>= 8;
        }
    }
    out[pos] = '\0';
}

const char* fat_find_token_value(const char* args, const char* key) {
    uint32_t i = 0;
    uint32_t key_len = 0;
    if (!args || !key || key[0] == '\0') {
        return 0;
    }
    while (key[key_len] != '\0') {
        key_len++;
    }
    for (;;) {
        uint32_t j = 0;
        while (args[i] == ' ') {
            i++;
        }
        if (args[i] == '\0') {
            return 0;
        }
        while (key[j] != '\0' && args[i + j] == key[j]) {
            j++;
        }
        if (j == key_len) {
            return &args[i + key_len];
        }
        while (args[i] != '\0' && args[i] != ' ') {
            i++;
        }
    }
}
