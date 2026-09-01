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

int32_t fsmgr_path_is_within(const char* mount, const char* path) {
    int32_t mlen = 0;
    int32_t plen = 0;

    if (!mount || !path || mount[0] != '/' || path[0] != '/') {
        return 0;
    }
    while (mount[mlen] != '\0') {
        mlen++;
    }
    /* A mount declared "/wfs/" names the same directory as "/wfs"; comparing the
     * separator as part of the prefix would make it contain nothing. */
    while (mlen > 1 && mount[mlen - 1] == '/') {
        mlen--;
    }
    while (path[plen] != '\0') {
        plen++;
    }
    if (mlen == 1) {
        return 1;
    }
    if (plen < mlen || !ascii_case_equal(mount, path, mlen)) {
        return 0;
    }
    /* The byte after the prefix decides sibling from child: end of string means
     * the path IS the mount, a separator means it is under it, and anything else
     * means the prefix ended mid-segment ("/wfsx" against "/wfs"). */
    return plen == mlen || path[mlen] == '/';
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
                out_path[0] = '\0';
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
                out_path[0] = '\0';
                return 0;
            }
            out_path[len++] = '/';
        }
        if (len + seg_len >= out_cap) {
            out_path[0] = '\0';
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

/* Length of `mount` with any trailing slashes removed, or 0 when it is not an
 * absolute path and therefore cannot own anything. The root keeps its single
 * '/', which is what makes a length of 1 mean "the root" below. */
static int32_t mount_path_len(const char* mount) {
    int32_t len = 0;
    if (!mount || mount[0] != '/') {
        return 0;
    }
    while (mount[len] != '\0') {
        len++;
    }
    while (len > 1 && mount[len - 1] == '/') {
        len--;
    }
    return len;
}

int32_t fsmgr_route_path_for_mounts(const char* path, int32_t path_len,
                                    const char* const* mount_paths, int32_t mount_count,
                                    int32_t* out_mount_index, char* out_path, int32_t out_path_cap,
                                    int32_t* out_path_len) {
    int32_t best = -1;
    /* Length of the matched mount, which is what ranks the candidates, and the
     * bytes of `path` that mount accounts for, which is where the tail starts.
     * They differ only for the root: it matches with length 1 and consumes
     * nothing, so the whole path is the tail. */
    int32_t best_match = 0;
    int32_t best_consume = 0;
    int32_t i;

    if (!path || !mount_paths || mount_count <= 0 || !out_mount_index || !out_path ||
        out_path_cap < 2 || !out_path_len) {
        return 0;
    }
    /* Absolute only. Every client path is joined onto the client's working
     * directory before it reaches here (fsmgr_cwd_join), so a relative one is a
     * caller bug rather than something to resolve against a guess. */
    if (path_len <= 0 || path[0] != '/') {
        return 0;
    }

    for (i = 0; i < mount_count; ++i) {
        int32_t mlen = mount_path_len(mount_paths[i]);
        int32_t consume;

        if (mlen <= 0) {
            continue;
        }
        if (mlen == 1) {
            consume = 0; /* the root: prefixes everything, strips nothing */
        } else {
            if (path_len < mlen || !ascii_case_equal(mount_paths[i], path, mlen)) {
                continue;
            }
            /* Whole segments only. Without this "/wfs" would own "/wfsx" and hand
             * its backend a tail of "x", addressing a file nobody named. */
            if (path_len > mlen && path[mlen] != '/') {
                continue;
            }
            consume = mlen;
        }
        /* Strictly longer, so two mounts of equal depth resolve to the first
         * registered rather than to whichever was scanned last. */
        if (mlen > best_match) {
            best = i;
            best_match = mlen;
            best_consume = consume;
        }
    }
    if (best < 0) {
        return 0;
    }

    if (best_consume >= path_len) {
        /* The path IS its mount, so it names that filesystem's own root. */
        out_path[0] = '/';
        out_path[1] = '\0';
        *out_path_len = 1;
    } else {
        int32_t tail_len = path_len - best_consume;
        int32_t j;
        /* Refused rather than truncated: a shortened path names a different
         * file, which the caller would then open without knowing. */
        if (tail_len >= out_path_cap) {
            return 0;
        }
        for (j = 0; j < tail_len; ++j) {
            out_path[j] = path[best_consume + j];
        }
        out_path[tail_len] = '\0';
        *out_path_len = tail_len;
    }
    *out_mount_index = best;
    return 1;
}
