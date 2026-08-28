/* Host stand-ins for the imports device_manager.c reaches outside its block
 * path, so that translation unit links into test_device_manager_block_rules.
 *
 * Every one is inert and reports the failure its caller treats as "nothing to
 * do": no endpoint, no message, no buffer. The block-rule matcher under test
 * neither sends nor receives -- it queues a rule and reports the match -- and a
 * spawn attempted off the back of a queued rule must therefore fail here
 * rather than pretend to have started a driver. Signatures are copied from
 * abi/generated/c/wasmos_imports.h and must be regenerated with it.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int32_t wasmos_ipc_drain(int32_t endpoint) {
    (void)endpoint;
    return 0;
}

int32_t wasmos_ipc_last_field(int32_t field) {
    (void)field;
    return 0;
}

int32_t wasmos_ipc_select_one(int32_t endpoint) {
    (void)endpoint;
    return -1;
}

int32_t wasmos_ipc_select_wait(int32_t sel) {
    (void)sel;
    return -1;
}

int32_t wasmos_ipc_select_wait_timeout(int32_t sel, int32_t timeout_ms) {
    (void)sel;
    (void)timeout_ms;
    return -1;
}

int32_t wasmos_ipc_send(int32_t dest,
                        int32_t src,
                        int32_t type,
                        int32_t request_id,
                        int32_t arg0,
                        int32_t arg1,
                        int32_t arg2,
                        int32_t arg3) {
    (void)dest;
    (void)src;
    (void)type;
    (void)request_id;
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return -1;
}

int32_t wasmos_sched_yield(void) {
    return 0;
}

int32_t wasmos_xfer_buffer_acquire(int32_t minimum_size) {
    (void)minimum_size;
    return -1;
}

int32_t wasmos_xfer_buffer_borrow(int32_t grantee, int32_t buffer_id, int32_t flags) {
    (void)grantee;
    (void)buffer_id;
    (void)flags;
    return -1;
}

int32_t wasmos_xfer_buffer_read(int32_t buffer_id, void* dst, int32_t len, int32_t offset) {
    (void)buffer_id;
    (void)dst;
    (void)len;
    (void)offset;
    return -1;
}

int32_t wasmos_xfer_buffer_release(int32_t buffer_id) {
    (void)buffer_id;
    return 0;
}

/* libc pieces the wasm build supplies in its own objects. str_copy's contract
 * (src/libc/include/string.h) is a NUL-terminating bounded copy returning the
 * number of bytes written, not the source length. */
size_t str_copy(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len == 0u) {
        return 0u;
    }
    size_t i = 0u;
    if (src) {
        for (; src[i] != '\0' && i + 1u < dst_len; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
    return i;
}

int putsn(const char* s, size_t len) {
    if (!s) {
        return 0;
    }
    extern long write(int fd, const void* buf, unsigned long count);
    return (int)write(2, s, (unsigned long)len);
}
