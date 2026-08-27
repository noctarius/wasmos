/* wfs_namespace.c - creating, removing and renaming directory entries (§10). */
#include "wfs_namespace.h"

#include <stddef.h>

#include "wfs_alloc.h"
#include "wfs_block.h"
#include "wfs_crc32c.h"
#include "wfs_dirent.h"
#include "wfs_mount.h"
#include "wfs_ops.h"
#include "wfs_path.h"

/* One directory block, staged in driver memory rather than in the block layer's
 * buffer: an operation reads a block, mutates it, and writes it back through two
 * separate tasks, and the layer's single staged block is not guaranteed to still
 * hold it in between. */
static uint8_t g_dirblk[WFS_BLOCK_SIZE_MAX];

static wfs_path_ctx_t g_ns_path;
static wfs_object_ctx_t g_ns_obj;
static wfs_objalloc_ctx_t g_ns_objalloc;
static wfs_objfree_ctx_t g_ns_objfree;

static void wr16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t* p, uint32_t off, uint32_t v) {
    wr16(p, off, (uint16_t)(v & 0xFFFFu));
    wr16(p, off + 2u, (uint16_t)((v >> 16) & 0xFFFFu));
}

static uint32_t rd32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

static void wr64(uint8_t* p, uint32_t off, uint64_t v) {
    wr32(p, off, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p, off + 4u, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

/* ---- one block in, one block out --------------------------------------- */

typedef enum { NS_BLK_PC_START = 0, NS_BLK_PC_DONE } ns_blk_pc_t;

typedef struct {
    ns_blk_pc_t pc;
    uint32_t block;
    uint32_t len;
    wasmos_error_code_t err;
} ns_blk_ctx_t;

static int32_t ns_read_block_task(void* user, uintptr_t* out_value) {
    ns_blk_ctx_t* ctx = (ns_blk_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t i;

    (void)out_value;

    switch (ctx->pc) {
    case NS_BLK_PC_START:
        WFS_AWAIT(ctx, wfs_block_read_begin(b, ctx->block), NS_BLK_PC_DONE);
        /* fall through when the block was already staged */

    case NS_BLK_PC_DONE:
        ctx->err = wfs_block_take(b);
        if (ctx->err != WASMOS_ERR_NONE) {
            return (int32_t)ctx->err;
        }
        for (i = 0; i < ctx->len; ++i) {
            g_dirblk[i] = wfs_block_data(b)[i];
        }
        return WASMOS_WASM_TASK_COMPLETE;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

static int32_t ns_write_block_task(void* user, uintptr_t* out_value) {
    ns_blk_ctx_t* ctx = (ns_blk_ctx_t*)user;
    wfs_block_t* b = wfs_ops_block();
    uint32_t i;

    (void)out_value;

    switch (ctx->pc) {
    case NS_BLK_PC_START:
        for (i = 0; i < ctx->len; ++i) {
            wfs_block_data(b)[i] = g_dirblk[i];
        }
        WFS_AWAIT(ctx, wfs_block_write_begin(b, ctx->block), NS_BLK_PC_DONE);
        /* fall through */

    case NS_BLK_PC_DONE:
        ctx->err = wfs_block_take(b);
        return (int32_t)ctx->err;

    default:
        WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
    }
}

static wasmos_error_code_t ns_read_block(const wfs_volume_t* vol, uint32_t block) {
    ns_blk_ctx_t c;
    int32_t status;

    c.pc = NS_BLK_PC_START;
    c.block = block;
    c.len = vol->super.block_size;
    c.err = WASMOS_ERR_NONE;
    status = wfs_ops_run(ns_read_block_task, &c);
    return status != 0 ? (wasmos_error_code_t)status : WASMOS_ERR_NONE;
}

static wasmos_error_code_t ns_write_block(const wfs_volume_t* vol, uint32_t block) {
    ns_blk_ctx_t c;
    int32_t status;

    c.pc = NS_BLK_PC_START;
    c.block = block;
    c.len = vol->super.block_size;
    c.err = WASMOS_ERR_NONE;
    status = wfs_ops_run(ns_write_block_task, &c);
    return status != 0 ? (wasmos_error_code_t)status : WASMOS_ERR_NONE;
}

/* ---- helpers ----------------------------------------------------------- */

/* The physical block a directory's logical block maps to, from the record's
 * inline extents. A directory with an extent TREE is refused by the callers: no
 * writer produces one, so none can be maintained. */
static uint32_t dir_physical(const struct wfs_object* obj, uint64_t logical) {
    uint32_t i;

    for (i = 0; i < obj->extent_count && i < WFS_INLINE_EXTENTS; ++i) {
        const struct wfs_extent* e = &obj->extents[i];

        if (logical >= e->logical_block && logical < e->logical_block + e->length) {
            return (uint32_t)(e->physical_block + (logical - e->logical_block));
        }
    }
    return 0u;
}

static uint32_t dir_block_count(const wfs_volume_t* vol, const struct wfs_object* obj) {
    uint32_t bs = vol->super.block_size;

    return (uint32_t)((obj->size + bs - 1u) / bs);
}

static wasmos_error_code_t ns_load_object(wfs_volume_t* vol, uint32_t id) {
    int32_t status;

    memset(&g_ns_obj, 0, sizeof(g_ns_obj));
    g_ns_obj.vol = vol;
    g_ns_obj.object_id = id;
    status = wfs_ops_run(wfs_object_task, &g_ns_obj);
    return status != 0 ? (wasmos_error_code_t)status : WASMOS_ERR_NONE;
}

/* Resolve the directory a path's final component lives in, leaving the parent in
 * g_ns_path and the split in the out params. */
static wasmos_error_code_t ns_resolve_parent(wfs_volume_t* vol, uint32_t cwd_object,
                                             const char* path, uint32_t path_len,
                                             const char** out_name, uint32_t* out_name_len) {
    uint32_t parent_len = 0u;
    wasmos_error_code_t rc;
    int32_t status;

    rc = wfs_dirent_split_path(path, path_len, &parent_len, out_name, out_name_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    /* An empty parent resolves to where the client stands, which is what a
     * relative name means (wfs_path_init_from). */
    rc = wfs_path_init_from(&g_ns_path, vol, cwd_object, path, parent_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    status = wfs_ops_run(wfs_path_task, &g_ns_path);
    if (status != 0) {
        return (wasmos_error_code_t)status;
    }
    if (!g_ns_path.found) {
        return WASMOS_ERR_FS_NOT_FOUND;
    }
    if (g_ns_path.object.out.type != WFS_TYPE_DIR) {
        return WASMOS_ERR_FS_NOT_DIR;
    }
    if (g_ns_path.object.out.extent_tree_block != 0u) {
        /* No writer maintains an extent tree, so a directory that has one cannot
         * be modified without corrupting it. */
        return WASMOS_ERR_FS_UNSUPPORTED;
    }
    return WASMOS_ERR_NONE;
}

/* Patch fields of an object record in place and reseal it. `size`, `extent`,
 * `link_delta` and `now_ns` are applied when non-zero / non-NULL, so one helper
 * serves the several records these operations touch. */
static wasmos_error_code_t ns_patch_record(wfs_volume_t* vol, uint32_t id, const uint64_t* size,
                                           const struct wfs_extent* extent, int32_t link_delta,
                                           uint64_t now_ns) {
    uint32_t bs = vol->super.block_size;
    uint32_t per_block = wfs_objects_per_block(bs);
    uint32_t rec_block = vol->super.object_table_start + id / per_block;
    uint32_t at = (id % per_block) * WFS_OBJECT_SIZE;
    uint8_t* d;
    wasmos_error_code_t rc;

    rc = ns_read_block(vol, rec_block);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    d = g_dirblk + at;
    if (size) {
        wr64(d, (uint32_t)offsetof(struct wfs_object, size), *size);
    }
    if (extent) {
        uint32_t e = (uint32_t)offsetof(struct wfs_object, extents);

        wr64(d, e + 0u, extent->logical_block);
        wr64(d, e + 8u, extent->physical_block);
        wr32(d, e + 16u, extent->length);
        wr32(d, e + 20u, 0u);
        wr32(d, (uint32_t)offsetof(struct wfs_object, extent_count), 1u);
        /* A directory's bytes are records, never inline content. */
        wr16(d, (uint32_t)offsetof(struct wfs_object, flags), 0u);
    }
    if (link_delta != 0) {
        uint32_t links = rd32(d, (uint32_t)offsetof(struct wfs_object, link_count));

        if (link_delta < 0 && links >= (uint32_t)(-link_delta)) {
            links -= (uint32_t)(-link_delta);
        } else if (link_delta > 0) {
            links += (uint32_t)link_delta;
        }
        wr32(d, (uint32_t)offsetof(struct wfs_object, link_count), links);
    }
    if (now_ns != 0u) {
        wr64(d, (uint32_t)offsetof(struct wfs_object, mtime), now_ns);
        wr64(d, (uint32_t)offsetof(struct wfs_object, ctime), now_ns);
    }
    wr32(d, (uint32_t)offsetof(struct wfs_object, checksum), 0u);
    wr32(d,
         (uint32_t)offsetof(struct wfs_object, checksum),
         wfs_checksum_struct(vol->super.uuid,
                             id,
                             d,
                             WFS_OBJECT_SIZE,
                             (uint32_t)offsetof(struct wfs_object, checksum)));
    return ns_write_block(vol, rec_block);
}

/* Find `name` anywhere in a directory, reporting the block it is in and the id it
 * names. A directory spans blocks, and wfs_dirent_find sees one at a time. */
static wasmos_error_code_t ns_find_entry(wfs_volume_t* vol, const struct wfs_object* dir,
                                         const char* name, uint32_t name_len, uint32_t* out_block,
                                         uint32_t* out_id) {
    uint32_t blocks = dir_block_count(vol, dir);
    uint32_t i;

    for (i = 0; i < blocks; ++i) {
        uint32_t phys = dir_physical(dir, i);
        int32_t at;
        wasmos_error_code_t rc;

        if (phys == 0u) {
            continue; /* a hole in a directory maps nothing */
        }
        rc = ns_read_block(vol, phys);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        at = wfs_dirent_find(g_dirblk, vol->super.block_size, name, name_len);
        if (at >= 0) {
            uint32_t id = 0u;
            uint32_t k;

            for (k = 0; k < 4u; ++k) {
                id |= (uint32_t)g_dirblk[(uint32_t)at + k] << (k * 8u);
            }
            if (out_block) {
                *out_block = phys;
            }
            if (out_id) {
                *out_id = id;
            }
            return WASMOS_ERR_NONE;
        }
    }
    return WASMOS_ERR_FS_NOT_FOUND;
}

/* Insert a record into whichever of a directory's blocks has room. */
static wasmos_error_code_t ns_insert_entry(wfs_volume_t* vol, const struct wfs_object* dir,
                                           const char* name, uint32_t name_len, uint32_t id,
                                           uint8_t type) {
    uint32_t blocks = dir_block_count(vol, dir);
    uint32_t i;

    for (i = 0; i < blocks; ++i) {
        uint32_t phys = dir_physical(dir, i);
        wasmos_error_code_t rc;

        if (phys == 0u) {
            continue;
        }
        rc = ns_read_block(vol, phys);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        rc = wfs_dirent_insert(
            g_dirblk, vol->super.block_size, vol->super.uuid, phys, name, name_len, id, type);
        if (rc == WASMOS_ERR_FS_NO_SPACE) {
            continue;
        }
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        return ns_write_block(vol, phys);
    }
    /* TODO: grow the directory instead. Every existing block is full, and a
     * directory is regular file data, so this needs one allocated block laid out
     * by wfs_dirent_init_block and appended to the record's extent map -- the
     * same append wfs_write.c does for a file. Until then a directory holds only
     * what its formatted blocks have room for. */
    return WASMOS_ERR_FS_NO_SPACE;
}

/* Whether a directory holds any entry other than `.` and `..`. */
static wasmos_error_code_t ns_dir_is_empty(wfs_volume_t* vol, const struct wfs_object* dir,
                                           int* out_empty) {
    uint32_t usable = wfs_dir_usable_bytes(vol->super.block_size);
    uint32_t blocks = dir_block_count(vol, dir);
    uint32_t i;

    *out_empty = 1;
    for (i = 0; i < blocks; ++i) {
        uint32_t phys = dir_physical(dir, i);
        uint32_t off = 0u;
        wasmos_error_code_t rc;

        if (phys == 0u) {
            continue;
        }
        rc = ns_read_block(vol, phys);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        while (off + WFS_DIR_ENTRY_HEADER <= usable) {
            uint32_t len = (uint32_t)g_dirblk[off + 8u] | ((uint32_t)g_dirblk[off + 9u] << 8);
            uint32_t nl = g_dirblk[off + 10u];
            uint32_t id = 0u;
            uint32_t k;

            if (len < WFS_DIR_RECORD_MIN || (len & 7u) != 0u || off + len > usable) {
                return WASMOS_ERR_FS_CORRUPT;
            }
            for (k = 0; k < 4u; ++k) {
                id |= (uint32_t)g_dirblk[off + k] << (k * 8u);
            }
            if (id != 0u && nl != 0u) {
                const uint8_t* nm = g_dirblk + off + WFS_DIR_ENTRY_HEADER;
                int is_dot = nl == 1u && nm[0] == '.';
                int is_dotdot = nl == 2u && nm[0] == '.' && nm[1] == '.';

                if (!is_dot && !is_dotdot) {
                    *out_empty = 0;
                    return WASMOS_ERR_NONE;
                }
            }
            off += len;
        }
    }
    return WASMOS_ERR_NONE;
}

/* ---- the operations ---------------------------------------------------- */

wasmos_error_code_t wfs_ns_create(wfs_volume_t* vol, uint32_t cwd_object, const char* path,
                                  uint32_t path_len, uint16_t type, uint32_t mode, uint64_t now_ns,
                                  uint32_t* out_object_id) {
    const char* name = 0;
    uint32_t name_len = 0u;
    uint32_t parent_id;
    struct wfs_object parent;
    wasmos_error_code_t rc;
    uint32_t new_id;
    int32_t status;

    if (!vol || !vol->mounted) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (vol->super.read_only) {
        return WASMOS_ERR_FS_READ_ONLY;
    }
    rc = ns_resolve_parent(vol, cwd_object, path, path_len, &name, &name_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    parent_id = g_ns_path.object_id;
    parent = g_ns_path.object.out;

    /* Checked across every block before anything is allocated, so a refused
     * create leaks nothing. wfs_dirent_insert also refuses a duplicate, but only
     * within the one block it is given. */
    rc = ns_find_entry(vol, &parent, name, name_len, 0, 0);
    if (rc == WASMOS_ERR_NONE) {
        return WASMOS_ERR_FS_EXISTS;
    }
    if (rc != WASMOS_ERR_FS_NOT_FOUND) {
        return rc;
    }

    memset(&g_ns_objalloc, 0, sizeof(g_ns_objalloc));
    g_ns_objalloc.vol = vol;
    g_ns_objalloc.type = type;
    g_ns_objalloc.mode = mode;
    /* A directory starts with two links: its own name in the parent, and its `.`. */
    g_ns_objalloc.link_count = type == WFS_TYPE_DIR ? 2u : 1u;
    g_ns_objalloc.now_ns = now_ns;
    g_ns_objalloc.prefer_group = 0u;
    status = wfs_ops_run(wfs_alloc_object_task, &g_ns_objalloc);
    if (status != 0) {
        return (wasmos_error_code_t)status;
    }
    new_id = g_ns_objalloc.object_id;

    if (type == WFS_TYPE_DIR) {
        wfs_alloc_ctx_t blocks;
        struct wfs_extent e;
        uint64_t size = vol->super.block_size;

        memset(&blocks, 0, sizeof(blocks));
        blocks.vol = vol;
        blocks.want = 1u;
        blocks.prefer_group = 0u;
        status = wfs_ops_run(wfs_alloc_blocks_task, &blocks);
        if (status != 0) {
            return (wasmos_error_code_t)status;
        }
        /* Lay the block out and put `.` and `..` in it BEFORE the record points
         * at it: a crash in between leaves an allocated block nothing names. */
        wfs_dirent_init_block(g_dirblk, vol->super.block_size, vol->super.uuid, blocks.first_block);
        rc = wfs_dirent_insert(g_dirblk,
                               vol->super.block_size,
                               vol->super.uuid,
                               blocks.first_block,
                               ".",
                               1u,
                               new_id,
                               (uint8_t)WFS_TYPE_DIR);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        rc = wfs_dirent_insert(g_dirblk,
                               vol->super.block_size,
                               vol->super.uuid,
                               blocks.first_block,
                               "..",
                               2u,
                               parent_id,
                               (uint8_t)WFS_TYPE_DIR);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        rc = ns_write_block(vol, blocks.first_block);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        e.logical_block = 0u;
        e.physical_block = blocks.first_block;
        e.length = 1u;
        e.reserved = 0u;
        rc = ns_patch_record(vol, new_id, &size, &e, 0, now_ns);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
    }

    /* The directory record LAST, so the object exists before anything names it. */
    rc = ns_insert_entry(vol,
                         &parent,
                         name,
                         name_len,
                         new_id,
                         (uint8_t)(type == WFS_TYPE_DIR ? WFS_TYPE_DIR : WFS_TYPE_FILE));
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    if (type == WFS_TYPE_DIR) {
        /* The new `..` is a link to the parent. */
        rc = ns_patch_record(vol, parent_id, 0, 0, 1, now_ns);
    } else {
        rc = ns_patch_record(vol, parent_id, 0, 0, 0, now_ns);
    }
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    if (out_object_id) {
        *out_object_id = new_id;
    }
    return WASMOS_ERR_NONE;
}

/* Shared tail of unlink and rmdir: the entry is off disk, so release what it
 * named. Data blocks first, then the record's own id.
 *
 * The extents are freed DIRECTLY rather than through wfs_truncate_task, which
 * refuses a directory outright — and rightly: truncating a directory is not
 * something a client may do, and relaxing that for an internal caller would
 * weaken a guarantee to save a few lines. Deletion also has no use for what
 * truncate does beyond freeing: there is no surviving tail block to zero.
 */
static wasmos_error_code_t ns_release_object(wfs_volume_t* vol, uint32_t id,
                                             const struct wfs_object* obj) {
    uint32_t i;
    int32_t status;

    if (obj->extent_tree_block != 0u) {
        /* No writer maintains an extent tree, so its blocks cannot be walked for
         * release without risking a partial free. */
        return WASMOS_ERR_FS_UNSUPPORTED;
    }
    for (i = 0; i < obj->extent_count && i < WFS_INLINE_EXTENTS; ++i) {
        wfs_free_ctx_t f;

        if (obj->extents[i].length == 0u) {
            continue;
        }
        memset(&f, 0, sizeof(f));
        f.vol = vol;
        f.first_block = (uint32_t)obj->extents[i].physical_block;
        f.length = obj->extents[i].length;
        status = wfs_ops_run(wfs_free_blocks_task, &f);
        if (status != 0) {
            return (wasmos_error_code_t)status;
        }
    }
    memset(&g_ns_objfree, 0, sizeof(g_ns_objfree));
    g_ns_objfree.vol = vol;
    g_ns_objfree.object_id = id;
    status = wfs_ops_run(wfs_free_object_task, &g_ns_objfree);
    return status != 0 ? (wasmos_error_code_t)status : WASMOS_ERR_NONE;
}

static wasmos_error_code_t ns_remove(wfs_volume_t* vol, uint32_t cwd_object, const char* path,
                                     uint32_t path_len, uint16_t want_type, uint64_t now_ns) {
    const char* name = 0;
    uint32_t name_len = 0u;
    uint32_t parent_id;
    uint32_t entry_block = 0u;
    uint32_t entry_id = 0u;
    struct wfs_object victim;
    wasmos_error_code_t rc;

    if (!vol || !vol->mounted) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (vol->super.read_only) {
        return WASMOS_ERR_FS_READ_ONLY;
    }
    rc = ns_resolve_parent(vol, cwd_object, path, path_len, &name, &name_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    parent_id = g_ns_path.object_id;

    rc = ns_find_entry(vol, &g_ns_path.object.out, name, name_len, &entry_block, &entry_id);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = ns_load_object(vol, entry_id);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    victim = g_ns_obj.out;
    if (want_type == WFS_TYPE_DIR && victim.type != WFS_TYPE_DIR) {
        return WASMOS_ERR_FS_NOT_DIR;
    }
    if (want_type == WFS_TYPE_FILE && victim.type == WFS_TYPE_DIR) {
        /* rmdir has a precondition unlink does not, so the two are separate
         * operations rather than one that guesses. */
        return WASMOS_ERR_FS_IS_DIR;
    }
    if (want_type == WFS_TYPE_DIR) {
        int empty = 0;

        rc = ns_dir_is_empty(vol, &victim, &empty);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        if (!empty) {
            return WASMOS_ERR_FS_NOT_EMPTY;
        }
    }

    /* The record comes off disk FIRST. Interrupted here, the object is
     * unreferenced -- space fsck reclaims. Freeing first would leave the entry
     * naming an id a later create can hand out again. */
    rc = ns_read_block(vol, entry_block);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = wfs_dirent_remove(
        g_dirblk, vol->super.block_size, vol->super.uuid, entry_block, name, name_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = ns_write_block(vol, entry_block);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }

    rc = ns_release_object(vol, entry_id, &victim);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    /* A removed directory took its `..` with it. */
    return ns_patch_record(vol, parent_id, 0, 0, want_type == WFS_TYPE_DIR ? -1 : 0, now_ns);
}

wasmos_error_code_t wfs_ns_unlink(wfs_volume_t* vol, uint32_t cwd_object, const char* path,
                                  uint32_t path_len, uint64_t now_ns) {
    return ns_remove(vol, cwd_object, path, path_len, (uint16_t)WFS_TYPE_FILE, now_ns);
}

wasmos_error_code_t wfs_ns_rmdir(wfs_volume_t* vol, uint32_t cwd_object, const char* path,
                                 uint32_t path_len, uint64_t now_ns) {
    return ns_remove(vol, cwd_object, path, path_len, (uint16_t)WFS_TYPE_DIR, now_ns);
}

wasmos_error_code_t wfs_ns_rename(wfs_volume_t* vol, uint32_t cwd_object, const char* from,
                                  uint32_t from_len, const char* to, uint32_t to_len,
                                  uint64_t now_ns) {
    const char* from_name = 0;
    const char* to_name = 0;
    uint32_t from_name_len = 0u;
    uint32_t to_name_len = 0u;
    uint32_t from_block = 0u;
    uint32_t from_parent_id = 0u;
    uint32_t to_parent_id = 0u;
    uint32_t entry_id = 0u;
    uint32_t entry_type = 0u;
    struct wfs_object from_dir;
    struct wfs_object to_dir;
    wasmos_error_code_t rc;

    if (!vol || !vol->mounted) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (vol->super.read_only) {
        return WASMOS_ERR_FS_READ_ONLY;
    }
    rc = ns_resolve_parent(vol, cwd_object, from, from_len, &from_name, &from_name_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    from_dir = g_ns_path.object.out;
    from_parent_id = g_ns_path.object_id;
    rc = ns_find_entry(vol, &from_dir, from_name, from_name_len, &from_block, &entry_id);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = ns_load_object(vol, entry_id);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    entry_type = g_ns_obj.out.type;

    rc = ns_resolve_parent(vol, cwd_object, to, to_len, &to_name, &to_name_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    to_dir = g_ns_path.object.out;
    to_parent_id = g_ns_path.object_id;
    /* This does not REPLACE. A rename onto an existing name would have to remove
     * that object too, and doing so before the insert leaves a window with
     * neither name resolving. */
    rc = ns_find_entry(vol, &to_dir, to_name, to_name_len, 0, 0);
    if (rc == WASMOS_ERR_NONE) {
        return WASMOS_ERR_FS_EXISTS;
    }
    if (rc != WASMOS_ERR_FS_NOT_FOUND) {
        return rc;
    }

    /* INSERT FIRST, which is the opposite of every other removal here.
     * Interrupted, the object is reachable under BOTH names -- a duplicate entry
     * fsck can resolve -- where removing first would leave it reachable under
     * NEITHER, which loses the file. */
    rc = ns_insert_entry(vol, &to_dir, to_name, to_name_len, entry_id, (uint8_t)entry_type);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = ns_read_block(vol, from_block);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = wfs_dirent_remove(
        g_dirblk, vol->super.block_size, vol->super.uuid, from_block, from_name, from_name_len);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = ns_write_block(vol, from_block);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    /* Both directories changed, so both timestamps move. Within one directory the
     * two patches address the same record and the second simply rewrites it. */
    rc = ns_patch_record(vol, from_parent_id, 0, 0, 0, now_ns);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    return ns_patch_record(vol, to_parent_id, 0, 0, 0, now_ns);
}
