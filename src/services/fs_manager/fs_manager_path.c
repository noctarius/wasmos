#include "fs_manager_path.h"

#include <stddef.h>

static int32_t
ascii_tolower(int32_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static int32_t
ascii_case_equal(const char *a, const char *b, int32_t n)
{
    int32_t i;
    if (!a || !b || n <= 0) {
        return 0;
    }
    for (i = 0; i < n; ++i) {
        if (ascii_tolower((uint8_t)a[i]) != ascii_tolower((uint8_t)b[i])) {
            return 0;
        }
    }
    return 1;
}

int32_t
fsmgr_route_path_for_mounts(const char *path,
                            int32_t path_len,
                            const char *const *mount_names,
                            int32_t mount_count,
                            int32_t allow_relative,
                            int32_t *out_mount_index,
                            char *out_path,
                            int32_t out_path_cap,
                            int32_t *out_path_len)
{
    int32_t start = 0;
    int32_t mount_start;
    int32_t mount_end;
    int32_t mount_len;
    int32_t i;
    int32_t tail_start;
    int32_t tail_len;

    if (!path || !mount_names || mount_count <= 0 || !out_mount_index || !out_path || out_path_cap < 2 || !out_path_len) {
        return 0;
    }
    if (path_len <= 0) {
        return 0;
    }
    if (path[0] == '/') {
        start = 1;
    } else if (allow_relative) {
        start = 0;
    } else {
        return 0;
    }
    if (start >= path_len || path[start] == '\0') {
        return 0;
    }

    mount_start = start;
    mount_end = mount_start;
    while (mount_end < path_len && path[mount_end] != '/' && path[mount_end] != '\0') {
        mount_end++;
    }
    mount_len = mount_end - mount_start;
    if (mount_len <= 0) {
        return 0;
    }

    for (i = 0; i < mount_count; ++i) {
        const char *name = mount_names[i];
        int32_t name_len = 0;
        if (!name) {
            continue;
        }
        while (name[name_len] != '\0') {
            name_len++;
        }
        if (name_len != mount_len) {
            continue;
        }
        if (!ascii_case_equal(name, path + mount_start, mount_len)) {
            continue;
        }

        tail_start = mount_end;
        if (tail_start >= path_len || path[tail_start] == '\0') {
            out_path[0] = '/';
            out_path[1] = '\0';
            *out_path_len = 1;
        } else {
            int32_t j = 0;
            tail_len = path_len - tail_start;
            if (tail_len >= out_path_cap) {
                return 0;
            }
            for (j = 0; j < tail_len; ++j) {
                out_path[j] = path[tail_start + j];
            }
            out_path[tail_len] = '\0';
            *out_path_len = tail_len;
        }
        *out_mount_index = i;
        return 1;
    }
    return 0;
}
