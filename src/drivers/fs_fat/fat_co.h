/* fat_co.h - minimal stackless-coroutine macros for the FAT reactor.
 *
 * Each resumable step function operates on a "coroutine context" — any struct
 * whose FIRST-class business is one operation or sub-machine and that carries an
 * `int cont;` resume field (the op ctx fat_op_ctx_t, and the embedded
 * sub-machine contexts like fat_chain_ctx_t / fat_dir_scan_ctx_t).  The step is
 * re-invoked by the reactor after each block completion and resumes where it
 * yielded.
 *
 * Rules (switch-based resume, à la protothreads):
 *   - A step body must be bracketed by FAT_CO_BEGIN(c) ... FAT_CO_END(c).
 *   - Do NOT place a `switch` statement containing a yield between them.
 *   - Any local that must survive a yield lives in the context struct, not on
 *     the C stack (the stack is not preserved across a yield/return).
 *   - Reset the context's cont to 0 before reusing it for a fresh run.
 *
 * All macros evaluate to a fat_r_t flow (FAT_R_WAIT / FAT_R_ERR / FAT_R_DONE)
 * on the paths that yield/return; on the fall-through path execution simply
 * continues past the macro. */
#ifndef FS_FAT_FAT_CO_H
#define FS_FAT_FAT_CO_H

#include "fat_block.h"
#include "fat_types.h"

/* c: pointer to a coroutine context (has `int cont`). */
#define FAT_CO_BEGIN(c)                                                                            \
    switch ((c)->cont) {                                                                           \
    case 0:
#define FAT_CO_END(c)                                                                              \
    }                                                                                              \
    (c)->cont = 0;                                                                                 \
    return FAT_R_DONE

/* Ensure sector `lba` is staged in fat_block_sector(blk); yield (submitting the
 * read) if it is not yet loaded, resuming here on completion.  blk->owner (the
 * active op) must be set by the reactor before the step runs. */
#define FAT_CO_READ(c, blk, lba)                                                                   \
    do {                                                                                           \
        (c)->cont = __LINE__;                                                                      \
    case __LINE__: {                                                                               \
        fat_r_t _cr = fat_need_sector((blk), (lba));                                               \
        if (_cr != FAT_R_DONE)                                                                     \
            return _cr;                                                                            \
    }                                                                                              \
    } while (0)

/* Land `count` whole sectors from `lba` directly into the client's transfer
 * buffer at `dst_offset` (nothing is staged); yield until it completes, resuming
 * here.  The buffer must already be reborrowed to the block server. */
#define FAT_CO_READ_DIRECT(c, blk, lba, count, buffer_id, borrow_id, dst_offset)                   \
    do {                                                                                           \
        (c)->cont = __LINE__;                                                                      \
    case __LINE__: {                                                                               \
        fat_r_t _cr =                                                                              \
            fat_block_read_direct((blk), (lba), (count), (buffer_id), (borrow_id), (dst_offset));  \
        if (_cr != FAT_R_DONE)                                                                     \
            return _cr;                                                                            \
    }                                                                                              \
    } while (0)

/* Push fat_block_sector(blk) to `lba` (a write); yield until it completes,
 * resuming here.  Fill the sector before invoking this. */
#define FAT_CO_WRITE(c, blk, lba)                                                                  \
    do {                                                                                           \
        (c)->cont = __LINE__;                                                                      \
    case __LINE__: {                                                                               \
        fat_r_t _cr = fat_block_write((blk), (lba));                                               \
        if (_cr != FAT_R_DONE)                                                                     \
            return _cr;                                                                            \
    }                                                                                              \
    } while (0)

/* Run sub-coroutine `callexpr` (a fat_r_t-returning step on an embedded
 * context) to completion, propagating its yields and errors upward. */
#define FAT_CO_AWAIT(c, callexpr)                                                                  \
    do {                                                                                           \
        (c)->cont = __LINE__;                                                                      \
    case __LINE__: {                                                                               \
        fat_r_t _cr = (callexpr);                                                                  \
        if (_cr != FAT_R_DONE)                                                                     \
            return _cr;                                                                            \
    }                                                                                              \
    } while (0)

/* Fail the op with an WASMOS_ERR_FS_* code (recorded on the active op via blk). */
#define FAT_CO_FAIL(c, blk, code)                                                                  \
    do {                                                                                           \
        (c)->cont = 0;                                                                             \
        fat_block_set_err((blk), (code));                                                          \
        return FAT_R_ERR;                                                                          \
    } while (0)

/* Complete the coroutine successfully. */
#define FAT_CO_DONE(c)                                                                             \
    do {                                                                                           \
        (c)->cont = 0;                                                                             \
        return FAT_R_DONE;                                                                         \
    } while (0)

#endif /* FS_FAT_FAT_CO_H */
