/* mkfs_wfs - create a WFS volume as an image file.
 *
 * Usage: mkfs_wfs [options] <image> <size>
 *
 *   size            bytes, or with a K/M/G suffix (1M, 256M, 2G)
 *   --block-size N  4096 (default), 8192, or 16384
 *   --journal N     journal size in blocks; derived from the volume when absent
 *   --objects-ratio N  bytes of volume per object record (default 16384)
 *   --uuid HEX      32 hex digits; random when absent
 *   --populate DIR  copy DIR's contents into the volume, recursively
 *   --quiet         print nothing on success
 *
 * The uuid is RANDOM by default. It is the volume's identity and it seeds every
 * metadata checksum, so two volumes sharing one would accept each other's
 * blocks — which is the transplant the seeding exists to catch. A predictable
 * default would disable that property for every volume that did not ask for it.
 *
 * Pass --uuid to pin one. That is what a reproducible fixture does: determinism
 * belongs to the caller that needs it, not to every volume.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#endif

#include <dirent.h>
#include <sys/stat.h>

/* Bounds on a --populate tree. Static, because the formatter takes the entry and
 * plan arrays from its caller and allocates nothing itself; raising these is a
 * one-line change and a bigger binary rather than a design change. */
#define MAX_ENTRIES 4096u
#define MAX_PATH 4096u

#include "wfs_mkfs.h"
#include "wfs_super.h"

static int sink_write(void* ctx, uint32_t block, const void* data, uint32_t len) {
    FILE* f = (FILE*)ctx;

    (void)block; /* blocks arrive in ascending order, so appending is correct */
    return fwrite(data, 1u, len, f) == len ? 0 : -1;
}

static int parse_size(const char* s, uint64_t* out) {
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 0);

    if (end == s) {
        return -1;
    }
    if (*end == 'K' || *end == 'k') {
        v *= 1024ull;
        end++;
    } else if (*end == 'M' || *end == 'm') {
        v *= 1024ull * 1024ull;
        end++;
    } else if (*end == 'G' || *end == 'g') {
        v *= 1024ull * 1024ull * 1024ull;
        end++;
    }
    if (*end != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int parse_uuid(const char* s, uint8_t out[WFS_UUID_LEN]) {
    unsigned i;

    if (strlen(s) != WFS_UUID_LEN * 2u) {
        return -1;
    }
    for (i = 0; i < WFS_UUID_LEN; ++i) {
        char buf[3] = {s[i * 2u], s[i * 2u + 1u], '\0'};
        char* end = NULL;
        unsigned long v = strtoul(buf, &end, 16);

        if (*end != '\0') {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* Fill `out` with 16 random bytes, stamped as an RFC 4122 version 4 UUID so the
 * value is a legitimate UUID and can be displayed as one.
 *
 * Returns -1 rather than falling back to anything predictable. A formatter that
 * quietly produced a guessable identity because the entropy source was missing
 * would reintroduce exactly the collision this defends against, and the caller
 * always has --uuid.
 */
static int random_uuid(uint8_t out[WFS_UUID_LEN]) {
    int have = 0;

#if defined(__APPLE__) || defined(__linux__)
    have = (getentropy(out, WFS_UUID_LEN) == 0);
#endif
    if (!have) {
        FILE* f = fopen("/dev/urandom", "rb");

        if (!f) {
            return -1;
        }
        have = (fread(out, 1u, WFS_UUID_LEN, f) == WFS_UUID_LEN);
        fclose(f);
    }
    if (!have) {
        return -1;
    }

    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u); /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u); /* variant 1 */
    return 0;
}

static void print_uuid(const uint8_t u[WFS_UUID_LEN]) {
    unsigned i;

    for (i = 0; i < WFS_UUID_LEN; ++i) {
        if (i == 4u || i == 6u || i == 8u || i == 10u) {
            putchar('-');
        }
        printf("%02x", u[i]);
    }
}

/* One host file behind an entry. Opened lazily on the first read and left open:
 * the formatter reads a file's blocks in ascending order and never revisits one,
 * so a single descriptor per entry is enough and the tree is walked once. */
typedef struct {
    char path[MAX_PATH];
    FILE* fh;
} source_t;

static wfs_mkfs_entry_t g_entries[MAX_ENTRIES];
static wfs_mkfs_node_t g_plan[MAX_ENTRIES + 1u];
static source_t g_sources[MAX_ENTRIES];
static char g_names[MAX_ENTRIES][256];
static uint32_t g_count;
static uint32_t g_skipped;

/* Read one block's worth of a source file, zero-filling a short tail so the
 * block a formatter emits is fully defined. */
static int source_read(void* ctx, uint64_t offset, void* dst, uint32_t len) {
    source_t* src = (source_t*)ctx;
    size_t got;

    if (!src->fh) {
        src->fh = fopen(src->path, "rb");
        if (!src->fh) {
            fprintf(stderr, "mkfs_wfs: cannot read %s\n", src->path);
            return -1;
        }
    }
    if (fseek(src->fh, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    got = fread(dst, 1u, len, src->fh);
    if (got < (size_t)len) {
        memset((uint8_t*)dst + got, 0, (size_t)len - got);
    }
    return 0;
}

/* Add `dir`'s children to the entry list, recursing into subdirectories.
 *
 * Entries are appended parent-before-child, which is the order the formatter
 * requires: it assigns object ids in array order and sizes each directory from
 * the children that name it.
 *
 * Anything that is not a regular file or a directory is SKIPPED and counted. A
 * symlink is the notable one: the format has a place for it (section 20) but this
 * tool has no way yet to record a target, and silently following one would put a
 * second copy of a file in the image under a name that was meant to be a link.
 */
static int walk(const char* dir, uint32_t parent) {
    DIR* d = opendir(dir);
    struct dirent* de;

    if (!d) {
        fprintf(stderr, "mkfs_wfs: cannot open directory %s\n", dir);
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        char path[MAX_PATH];
        struct stat st;
        uint32_t idx;
        size_t name_len = strlen(de->d_name);

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (g_count >= MAX_ENTRIES) {
            fprintf(stderr, "mkfs_wfs: more than %u entries\n", MAX_ENTRIES);
            closedir(d);
            return -1;
        }
        if (name_len >= sizeof(g_names[0])) {
            fprintf(stderr, "mkfs_wfs: name too long: %s\n", de->d_name);
            closedir(d);
            return -1;
        }
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= sizeof(path)) {
            fprintf(stderr, "mkfs_wfs: path too long under %s\n", dir);
            closedir(d);
            return -1;
        }
        /* lstat, not stat: a symlink must be recognised as one rather than
         * reported as whatever it points at. */
        if (lstat(path, &st) != 0) {
            fprintf(stderr, "mkfs_wfs: cannot stat %s\n", path);
            closedir(d);
            return -1;
        }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "mkfs_wfs: skipping %s (not a regular file or directory)\n", path);
            g_skipped++;
            continue;
        }

        idx = g_count++;
        memcpy(g_names[idx], de->d_name, name_len + 1u);
        memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
        g_entries[idx].name = g_names[idx];
        g_entries[idx].name_len = (uint32_t)name_len;
        g_entries[idx].parent = parent;
        g_entries[idx].is_dir = S_ISDIR(st.st_mode) ? 1u : 0u;
        g_entries[idx].mode = (uint32_t)(st.st_mode & 0777);

        if (S_ISDIR(st.st_mode)) {
            if (walk(path, idx) != 0) {
                closedir(d);
                return -1;
            }
        } else {
            g_entries[idx].size = (uint64_t)st.st_size;
            memcpy(g_sources[idx].path, path, strlen(path) + 1u);
            g_sources[idx].fh = NULL;
            g_entries[idx].read = source_read;
            g_entries[idx].read_ctx = &g_sources[idx];
        }
    }
    closedir(d);
    return 0;
}

static void close_sources(void) {
    uint32_t i;

    for (i = 0; i < g_count; ++i) {
        if (g_sources[i].fh) {
            fclose(g_sources[i].fh);
            g_sources[i].fh = NULL;
        }
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: mkfs_wfs [--block-size N] [--journal N] [--objects-ratio N]\n"
            "                [--uuid HEX32] [--populate DIR] [--quiet] <image> <size>\n");
}

int main(int argc, char** argv) {
    wfs_mkfs_params_t params;
    wfs_mkfs_layout_t layout;
    wfs_mkfs_sink_t sink;
    wasmos_error_code_t rc;
    const char* path = NULL;
    uint64_t size = 0;
    const char* populate = NULL;
    int quiet = 0;
    int have_uuid = 0;
    int i;
    FILE* f;

    memset(&params, 0, sizeof(params));

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
            params.block_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            params.journal_blocks = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--objects-ratio") == 0 && i + 1 < argc) {
            params.bytes_per_object = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--uuid") == 0 && i + 1 < argc) {
            if (parse_uuid(argv[++i], params.uuid) != 0) {
                fprintf(stderr, "mkfs_wfs: --uuid takes 32 hex digits\n");
                return 2;
            }
            have_uuid = 1;
        } else if (strcmp(argv[i], "--populate") == 0 && i + 1 < argc) {
            populate = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (!path) {
            path = argv[i];
        } else if (parse_size(argv[i], &size) != 0) {
            fprintf(stderr, "mkfs_wfs: cannot parse size '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!path || size == 0) {
        usage();
        return 2;
    }

    if (!have_uuid && random_uuid(params.uuid) != 0) {
        fprintf(stderr, "mkfs_wfs: no entropy source; pass --uuid to choose one\n");
        return 1;
    }

    if (populate && walk(populate, WFS_MKFS_ROOT) != 0) {
        close_sources();
        return 1;
    }

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "mkfs_wfs: cannot open %s\n", path);
        return 1;
    }

    params.size_bytes = size;
    sink.ctx = f;
    sink.write_block = sink_write;

    rc = wfs_mkfs_format_tree(&params, g_entries, g_count, g_plan, &sink, &layout);
    close_sources();
    if (fclose(f) != 0 && rc == WASMOS_ERR_NONE) {
        rc = WASMOS_ERR_FS_IO;
    }
    if (rc != WASMOS_ERR_NONE) {
        fprintf(stderr, "mkfs_wfs: %s\n", wasmos_strerror(rc));
        return 1;
    }

    if (!quiet) {
        printf("%s: %u blocks of %u bytes, %u group(s), %u objects\n",
               path,
               layout.total_blocks,
               layout.block_size,
               layout.group_count,
               layout.total_objects);
        printf("  uuid         ");
        print_uuid(params.uuid);
        putchar('\n');
        printf("  group table  %u +%u\n", layout.group_table_start, layout.group_table_blocks);
        printf("  journal      %u +%u\n", layout.journal_start, layout.journal_blocks);
        printf("  object table %u +%u\n", layout.object_table_start, layout.object_table_blocks);
        printf("  bitmaps      %u +%u\n", layout.bitmap_start, layout.bitmap_blocks);
        printf("  data from    %u (root directory at %u)\n",
               layout.first_data_block,
               layout.root_data_block);
        printf("  root dir     %u +%u\n", layout.root_data_block, layout.root_blocks);
        if (layout.entry_count != 0u) {
            printf("  entries      %u in %u block(s)\n", layout.entry_count, layout.used_blocks);
        }
        printf("  free         %u blocks, %u objects\n", layout.free_blocks, layout.free_objects);
    }

    /* Non-zero when anything was skipped, so a build that expected the whole
     * tree in the image notices rather than shipping a volume missing files. */
    if (g_skipped != 0u) {
        fprintf(stderr, "mkfs_wfs: %u entr%s skipped\n", g_skipped, g_skipped == 1u ? "y" : "ies");
        return 3;
    }
    return 0;
}
