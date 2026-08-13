/* stubs_native_libsys.c - host-compilation stub for the one libc helper the
 * native libsys sources pull in. Their test compile lines omit src/libc, so
 * str_copy_bytes (used by libsys_native.c's service-descriptor and class-name
 * paths) is supplied here: refuse-on-overflow bounded copy, NUL terminated,
 * 0 on success and -1 when it does not fit.
 *
 * FIXME: src/libc/src/string.c also refuses src_len == 0, this copy does not.
 * A zero-length name or class therefore reports success on the host and failure
 * on target, so no test can pin that boundary. */
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
