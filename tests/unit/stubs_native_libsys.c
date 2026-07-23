#include <stddef.h>
#include <stdint.h>

int str_copy_bytes(char* dst, size_t dst_len, const uint8_t* src, size_t src_len) {
    size_t i;

    if (!dst || !src || src_len >= dst_len) {
        return -1;
    }
    for (i = 0; i < src_len; ++i) {
        dst[i] = (char)src[i];
    }
    dst[src_len] = '\0';
    return 0;
}
