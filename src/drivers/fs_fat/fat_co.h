/* fat_co.h - minimal stackless-coroutine macros for the FAT reactor.
 *
 * Each resumable step function operates on a "coroutine context": any struct
 * that carries an `int cont;` resume field and holds the state of one operation
 * or sub-machine (fat_op_ctx_t itself, and the embedded sub-machine contexts
 * like fat_chain_ctx_t / fat_dir_scan_ctx_t).  The step is re-invoked by the
 * reactor after each block completion and resumes where it yielded.
 *
 * Rules (switch-based resume, à la protothreads):
 *   - A step body must be bracketed by FAT_CO_BEGIN(c) ... FAT_CO_END(c).
 *   - Do NOT place a `switch` statement containing a yield between them.
 *   - Any local that must survive a yield lives in the context struct, not on
 *     the C stack (the stack is not preserved across a yield/return).
 *   - Reset the context's cont to 0 before reusing it for a fresh run.
 *   - Resume points are generated from __LINE__, so two yielding macros must not
 *     share a source line: a `do { FAT_CO_READ(...); FAT_CO_WRITE(...); }` on one
 *     line produces duplicate case labels and will not compile. Keeping one per
 *     line is what makes the labels unique, not a convention.
 *
 * All macros evaluate to a fat_r_t flow (FAT_R_WAIT / FAT_R_ERR / FAT_R_DONE)
 * on the paths that yield/return; on the fall-through path execution simply
 * continues past the macro.
 *
 * Every yielding macro returns out of the ENCLOSING FUNCTION -- they are not
 * expressions and cannot appear in one. A step therefore cannot hold a lock,
 * own a borrow, or leave any other cleanup pending across a yield unless the
 * teardown path (fat_op_free) also knows how to release it. */
#ifndef FS_FAT_FAT_CO_H
#define FS_FAT_FAT_CO_H

#include "fat_block.h"
#include "fat_types.h"

/* Ownership of the shared block buffer.
 *
 * There is exactly ONE staging buffer for the whole driver (fat_block_t), and
 * the reactor drives one op to completion at a time. Before stepping an op the
 * reactor sets blk->owner to it, which is what makes the yielding macros below
 * work: a completion knows which op to resume, and FAT_CO_FAIL knows which op to
 * record its error code on. A step therefore may assume the staged sector is the
 * one IT last asked for -- across a yield of its own -- but must assume nothing
 * about it on entry, because the previous op left whatever it left there. That
 * is why FAT_CO_READ goes through fat_need_sector, which consults the cache tag
 * and only submits a read on a miss, rather than reading unconditionally.
 *
 * What each macro leaves staged differs, and the difference matters:
 *   - FAT_CO_READ      stages `lba` and tags the buffer with it.
 *   - FAT_CO_WRITE     pushes the buffer to `lba` and tags it with that lba, so
 *                      the contents remain valid for the sector just written.
 *   - FAT_CO_READ_DIRECT lands nothing here: the cache tag and contents are left
 *                      exactly as they were, which is why it is safe to issue
 *                      one in the middle of a sequence that is using the buffer.
 *   - FAT_CO_AWAIT     may restage the buffer arbitrarily, because the
 *                      sub-machine does its own I/O. A step must re-issue
 *                      FAT_CO_READ after one before touching the sector again.
 * A failed completion clears the tag, so an I/O error never leaves stale bytes
 * looking cached.
 *
 * A pointer into fat_block_sector() is therefore valid only until the next
 * yield. Anything that must outlive one belongs in the context struct. */

/* Open a resumable body: jump to the resume point recorded in `c->cont`, or fall
 * into the top on a fresh run (cont == 0). Everything between this and
 * FAT_CO_END is one switch statement, which is why a `switch` of your own
 * containing a yield cannot appear there. */
#define FAT_CO_BEGIN(c)                                                                            \
    switch ((c)->cont) {                                                                           \
    case 0:
/* Close the body: reset the context so it can be reused for a fresh run and
 * report success. Falling off the end of a step is thus equivalent to
 * FAT_CO_DONE -- reaching FAT_CO_END is the normal completion path, not an
 * error, and nothing after it in the function body is reachable. */
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

/* Fail with a packed WASMOS_ERR_FS_* code, recorded on the op that owns the
 * block buffer (blk->owner), which the reactor reports as FS_IPC_ERROR.  The
 * context is reset so it can be reused for a fresh run. */
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
