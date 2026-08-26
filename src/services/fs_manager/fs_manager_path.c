/* fs_manager_path.c - case-insensitive path-to-mount routing helper */
#include "fs_manager_path.h"

#include <stddef.h>

/* ASCII-only tolower; used for case-insensitive mount name comparison. */
static int32_t ascii_tolower(int32_t c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

/* Case-insensitive comparison of exactly n bytes; returns 1 if equal. */
static int32_t ascii_case_equal(const char* a, const char* b, int32_t n) {
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

int32_t fsmgr_cwd_join(const char* cwd, const char* arg, char* out_path, int32_t out_cap) {
    int32_t len = 0;
    int32_t i = 0;
    const char* src = 0;

    if (!cwd || !arg || !out_path || out_cap < 2) {
        return 0;
    }
    if (cwd[0] != '/') {
        return 0;
    }

    /* An absolute argument discards the working directory entirely; a relative
     * one is canonicalized on top of it, so seeding out_path with cwd and then
     * walking arg's segments handles both with one loop. */
    if (arg[0] == '/') {
        out_path[0] = '/';
        len = 1;
        src = arg + 1;
    } else {
        while (cwd[len] != '\0') {
            if (len + 1 >= out_cap) {
                return 0;
            }
            out_path[len] = cwd[len];
            len++;
        }
        /* Strip a trailing slash so segment appends are uniform; the root keeps
         * its single '/' as the one path that legitimately ends in one. */
        while (len > 1 && out_path[len - 1] == '/') {
            len--;
        }
        src = arg;
    }

    while (src[i] != '\0') {
        int32_t seg_start;
        int32_t seg_len;
        int32_t k;

        if (src[i] == '/') {
            i++;
            continue;
        }
        seg_start = i;
        while (src[i] != '\0' && src[i] != '/') {
            i++;
        }
        seg_len = i - seg_start;

        if (seg_len == 1 && src[seg_start] == '.') {
            continue;
        }
        if (seg_len == 2 && src[seg_start] == '.' && src[seg_start + 1] == '.') {
            /* Pop one segment; at the root there is nothing to pop and the join
             * stays there rather than escaping the VFS. */
            while (len > 1 && out_path[len - 1] != '/') {
                len--;
            }
            while (len > 1 && out_path[len - 1] == '/') {
                len--;
            }
            continue;
        }
        if (len > 1) {
            if (len + 1 >= out_cap) {
                return 0;
            }
            out_path[len++] = '/';
        }
        if (len + seg_len >= out_cap) {
            return 0;
        }
        for (k = 0; k < seg_len; ++k) {
            out_path[len++] = src[seg_start + k];
        }
    }

    if (len == 0) {
        out_path[len++] = '/';
    }
    out_path[len] = '\0';
    return 1;
}

int32_t fsmgr_route_path_for_mounts(const char* path, int32_t path_len,
                                    const char* const* mount_names, int32_t mount_count,
                                    int32_t allow_relative, int32_t* out_mount_index,
                                    char* out_path, int32_t out_path_cap, int32_t* out_path_len) {
    int32_t start = 0;
    int32_t mount_start;
    int32_t mount_end;
    int32_t mount_len;
    int32_t i;
    int32_t tail_start;
    int32_t tail_len;

    if (!path || !mount_names || mount_count <= 0 || !out_mount_index || !out_path ||
        out_path_cap < 2 || !out_path_len) {
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
        const char* name = mount_names[i];
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
