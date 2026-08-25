/* wfs_path.c - walk a path from the root.
 *
 * Authority: docs/WFS_WASMOS_FILE_SYSTEM.md §15.
 */
#include "wfs_path.h"

#include "wfs_dir.h"
#include "wfs_mount.h"

wasmos_error_code_t wfs_path_init(wfs_path_ctx_t* ctx, const wfs_volume_t* vol, const char* path,
                                  uint32_t len) {
    uint32_t i;

    if (!ctx || !vol || !path) {
        return WASMOS_ERR_FS_BAD_ARGS;
    }
    if (len == 0u || path[0] != '/') {
        /* The driver is handed a path already rooted by the caller; there is no
         * working directory here to resolve a relative one against. */
        return WASMOS_ERR_FS_NOT_ABSOLUTE;
    }
    if (len >= WFS_PATH_MAX) {
        return WASMOS_ERR_FS_PATH_TOO_LONG;
    }

    ctx->pc = WFS_PATH_PC_START;
    ctx->vol = vol;
    for (i = 0; i < len; ++i) {
        ctx->path[i] = path[i];
    }
    ctx->path[len] = '\0';
    ctx->path_len = len;
    ctx->cursor = 0u;
    ctx->object_id = WFS_OBJECT_ROOT;
    ctx->err = WASMOS_ERR_NONE;
    ctx->child_started = 0u;
    ctx->found = 0u;
    return WASMOS_ERR_NONE;
}

/* Advance past separators and report the next component, or 0 when the path is
 * exhausted. Empty components are skipped, so "//a///b" and "/a/b" name the
 * same object and a trailing slash is harmless. */
static uint32_t next_component(wfs_path_ctx_t* ctx, uint32_t* out_start) {
    uint32_t start;
    uint32_t end;

    while (ctx->cursor < ctx->path_len && ctx->path[ctx->cursor] == '/') {
        ctx->cursor++;
    }
    start = ctx->cursor;
    end = start;
    while (end < ctx->path_len && ctx->path[end] != '/') {
        end++;
    }
    ctx->cursor = end;
    *out_start = start;
    return end - start;
}

int32_t wfs_path_task(void* user, uintptr_t* out_value) {
    wfs_path_ctx_t* ctx = (wfs_path_ctx_t*)user;
    uint32_t start = 0;
    uint32_t len;
    int32_t jr;

    (void)out_value;

    for (;;) {
        switch (ctx->pc) {
        case WFS_PATH_PC_START:
            ctx->pc = WFS_PATH_PC_OBJECT;
            continue;

        case WFS_PATH_PC_OBJECT:
            /* Read the object the walk stands on. Done for every component, and
             * once more at the end, so the caller gets the record of whatever
             * the path named without fetching it again. */
            ctx->object.pc = WFS_OBJECT_PC_START;
            ctx->object.vol = ctx->vol;
            ctx->object.object_id = ctx->object_id;
            ctx->object.err = WASMOS_ERR_NONE;
            wfs_ops_task_reset(&ctx->child);
            if (!wasmos_async_start(
                    wfs_ops_runtime(), &ctx->child, wfs_object_task, &ctx->object)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->child_started = 1u;
            ctx->pc = WFS_PATH_PC_OBJECT_JOIN;
            continue;

        case WFS_PATH_PC_OBJECT_JOIN:
            jr = 0;
            if (wasmos_wasm_coroutine_join(&ctx->child, &jr) == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->child_started = 0u;
            if (ctx->object.err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->object.err;
            }
            ctx->pc = WFS_PATH_PC_LOOKUP;
            continue;

        case WFS_PATH_PC_LOOKUP:
            len = next_component(ctx, &start);
            if (len == 0u) {
                /* The path is exhausted: the object just read is the answer. */
                ctx->found = 1u;
                return WASMOS_WASM_TASK_COMPLETE;
            }
            /* A component under something that is not a directory cannot
             * resolve, and saying so beats reporting the component missing. */
            if (ctx->object.out.type != WFS_TYPE_DIR) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_NOT_DIR);
            }
            wfs_dir_lookup_init(&ctx->dir, ctx->vol, &ctx->object.out, &ctx->path[start], len);
            wfs_ops_task_reset(&ctx->child);
            if (!wasmos_async_start(wfs_ops_runtime(), &ctx->child, wfs_dir_task, &ctx->dir)) {
                WFS_FAIL(ctx, WASMOS_ERR_FS_BUSY);
            }
            ctx->child_started = 1u;
            ctx->pc = WFS_PATH_PC_LOOKUP_JOIN;
            continue;

        case WFS_PATH_PC_LOOKUP_JOIN:
            jr = 0;
            if (wasmos_wasm_coroutine_join(&ctx->child, &jr) == WASMOS_WASM_AWAIT_PENDING) {
                return WASMOS_WASM_TASK_YIELDED;
            }
            ctx->child_started = 0u;
            if (ctx->dir.err != WASMOS_ERR_NONE) {
                return (int32_t)ctx->dir.err;
            }
            if (!ctx->dir.found) {
                /* A component that does not exist. Not a failure: an open that
                 * misses is how a create learns it may proceed. */
                ctx->found = 0u;
                return WASMOS_WASM_TASK_COMPLETE;
            }
            ctx->object_id = ctx->dir.object_id;
            ctx->pc = WFS_PATH_PC_OBJECT;
            continue;

        default:
            WFS_FAIL(ctx, WASMOS_ERR_FS_CORRUPT);
        }
    }
}
