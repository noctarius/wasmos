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

#include "wasmos_opcodes.h"

/* One file a test serves as fs-manager would, or NULL for none. While it is
 * set, the first FS_IPC_READ_PATH_REQ is answered with as much of it as the
 * request's buffer holds -- fs-manager's own clamp -- and the reply is what the
 * next receive returns. Unset, every call below stays inert. */
const char* g_test_fs_file;
int32_t g_test_fs_file_len;
static int32_t g_fs_reply_id;
static int32_t g_fs_reply_len = -1;

int32_t wasmos_ipc_drain(int32_t endpoint) {
    (void)endpoint;
    return 0;
}

int32_t wasmos_ipc_last_field(int32_t field) {
    switch (field) {
    case 0:
        return g_fs_reply_len >= 0 ? (int32_t)FS_IPC_RESP : 0;
    case 1:
        return g_fs_reply_id;
    case 2:
        return g_fs_reply_len >= 0 ? g_fs_reply_len : 0;
    default:
        return 0;
    }
}

/* wasmos_ipc_call registers an await on its server before sending; every
 * server here lives, so the await is always taken. */
int32_t wasmos_ipc_peer_await(int32_t waiter, int32_t awaited) {
    (void)waiter;
    (void)awaited;
    return 0;
}
int32_t wasmos_ipc_select_one(int32_t endpoint) {
    (void)endpoint;
    return g_fs_reply_len >= 0 ? 0 : -1;
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

int32_t wasmos_ipc_send(int32_t dest, int32_t src, int32_t type, int32_t request_id, int32_t arg0,
                        int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)dest;
    (void)src;
    (void)arg0;
    (void)arg2;
    (void)arg3;
    if (g_test_fs_file && type == (int32_t)FS_IPC_READ_PATH_REQ) {
        g_fs_reply_id = request_id;
        g_fs_reply_len = g_test_fs_file_len < arg1 ? g_test_fs_file_len : arg1;
        return 0;
    }
    return -1;
}

int32_t wasmos_sched_yield(void) {
    return 0;
}

int32_t wasmos_xfer_buffer_acquire(int32_t minimum_size) {
    (void)minimum_size;
    return g_test_fs_file ? 1 : -1;
}

int32_t wasmos_xfer_buffer_size(void) {
    return 65536;
}

int32_t wasmos_xfer_buffer_borrow(int32_t grantee, int32_t buffer_id, int32_t flags) {
    (void)grantee;
    (void)buffer_id;
    (void)flags;
    return g_test_fs_file ? 1 : -1;
}

/* Reads back the served file; the reply is consumed by the read. */
int32_t wasmos_xfer_buffer_read(int32_t buffer_id, void* dst, int32_t len, int32_t offset) {
    (void)buffer_id;
    if (!g_test_fs_file || g_fs_reply_len < 0 || offset != 0 || len > g_fs_reply_len) {
        return -1;
    }
    memcpy(dst, g_test_fs_file, (size_t)len);
    g_fs_reply_len = -1;
    return 0;
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

/* The kernel environment, as a test drives it.
 *
 * Not inert like the rest: `boot.partition` is an INPUT to the block path under
 * test. It decides which volume carries VOLUME_DESCRIPTOR_FLAG_BOOT, and so
 * which one an ATTR{boot} rule selects. A test sets g_test_env_boot_partition to
 * the string the kernel would have published, or leaves it empty for a boot the
 * firmware could not describe -- which is a distinct case, not an absent one. */
char g_test_env_boot_partition[64];

int32_t wasmos_env_get(const char* name, int32_t name_len, char* buf, int32_t buf_len) {
    static const char key[] = "boot.partition";
    int32_t n = 0;
    if (!name || !buf || buf_len <= 0) {
        return -1;
    }
    if (name_len != (int32_t)(sizeof(key) - 1u) || strncmp(name, key, sizeof(key) - 1u) != 0) {
        return -1;
    }
    if (g_test_env_boot_partition[0] == '\0') {
        return -1;
    }
    while (g_test_env_boot_partition[n] != '\0' && n + 1 < buf_len) {
        buf[n] = g_test_env_boot_partition[n];
        ++n;
    }
    buf[n] = '\0';
    return n;
}

/* Reached by the module-descriptor refetch (load_pci_rule_metadata), which a
 * rules load schedules. No module resolves here, so a refetched rule is left
 * without declared windows. */
int32_t wasmos_xfer_buffer_write(int32_t buffer_id, const void* src, int32_t len, int32_t offset) {
    (void)buffer_id;
    (void)src;
    (void)len;
    (void)offset;
    return g_test_fs_file ? 0 : -1;
}

int32_t wasmos_boot_module_name(int32_t index, int32_t out, int32_t out_len) {
    (void)index;
    (void)out;
    (void)out_len;
    return -1;
}

int32_t wasmos_sync_user_read(int32_t ptr, int32_t len) {
    (void)ptr;
    (void)len;
    return 0;
}
