/* mkfs_wfs - create a WFS volume as an image file.
 *
 * Usage: mkfs_wfs [options] <image> <size>
 *
 *   size            bytes, or with a K/M/G suffix (1M, 256M, 2G)
 *   --block-size N  4096 (default), 8192, or 16384
 *   --journal N     journal size in blocks; derived from the volume when absent
 *   --objects-ratio N  bytes of volume per object record (default 16384)
 *   --uuid HEX      32 hex digits; a fixed default when absent
 *   --quiet         print nothing on success
 *
 * The uuid defaults to a FIXED value rather than a random one, so an image
 * built twice from the same arguments is byte-identical. Reproducible images
 * are what let a test fixture be regenerated and diffed, and this tool has no
 * entropy source it could justify depending on. Pass --uuid for a distinct
 * volume identity.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void usage(void) {
    fprintf(stderr,
            "usage: mkfs_wfs [--block-size N] [--journal N] [--objects-ratio N]\n"
            "                [--uuid HEX32] [--quiet] <image> <size>\n");
}

int main(int argc, char** argv) {
    wfs_mkfs_params_t params;
    wfs_mkfs_layout_t layout;
    wfs_mkfs_sink_t sink;
    wasmos_error_code_t rc;
    const char* path = NULL;
    uint64_t size = 0;
    int quiet = 0;
    int i;
    FILE* f;

    memset(&params, 0, sizeof(params));
    /* A fixed default identity; see the note above on reproducibility. */
    memcpy(params.uuid, "WFS-default-vol\x01", WFS_UUID_LEN);

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

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "mkfs_wfs: cannot open %s\n", path);
        return 1;
    }

    params.size_bytes = size;
    sink.ctx = f;
    sink.write_block = sink_write;

    rc = wfs_mkfs_format(&params, &sink, &layout);
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
        printf("  group table  %u +%u\n", layout.group_table_start, layout.group_table_blocks);
        printf("  journal      %u +%u\n", layout.journal_start, layout.journal_blocks);
        printf("  object table %u +%u\n", layout.object_table_start, layout.object_table_blocks);
        printf("  bitmaps      %u +%u\n", layout.bitmap_start, layout.bitmap_blocks);
        printf("  data from    %u (root directory at %u)\n",
               layout.first_data_block,
               layout.root_data_block);
        printf("  free         %u blocks, %u objects\n", layout.free_blocks, layout.free_objects);
    }
    return 0;
}
