/* wfs_co.h - stackless-coroutine macros for the WFS reactor.
 *
 * Every operation the driver performs is a resumable step function over a
 * context struct carrying an `int cont;` resume field. The reactor re-invokes
 * the step after each block completion and it resumes where it yielded. This is
 * the same shape as src/drivers/fs_fat/fat_co.h, deliberately: a driver whose
 * logic blocks cannot serve a second request while the first waits on a device,
 * and retrofitting that afterwards means rewriting every step.
 *
 * Rules (switch-based resume, à la protothreads):
 *   - A step body is bracketed by WFS_CO_BEGIN(c) ... WFS_CO_END(c).
 *   - Do NOT place a `switch` of your own containing a yield between them.
 *   - Any local that must survive a yield lives in the context struct, not on
 *     the C stack: the stack is not preserved across a yield/return.
 *   - Reset the context's `cont` to 0 before reusing it for a fresh run.
 *   - Resume points are generated from __LINE__, so two yielding macros must
 *     not share a source line. One per line is what makes the labels unique,
 *     not a style convention.
 *
 * Every yielding macro returns out of the ENCLOSING FUNCTION. They are not
 * expressions and cannot appear in one. A step therefore cannot hold a borrow
 * or leave any other cleanup pending across a yield unless the teardown path
 * knows how to release it.
 *
 * A pointer into wfs_block_data() is valid only until the next yield: the
 * staging buffer is shared, and the reactor may run another op's step in
 * between. Anything that must outlive a yield belongs in the context struct.
 */
#ifndef FS_WFS_WFS_CO_H
#define FS_WFS_WFS_CO_H

#include "wfs_block.h"
#include "wfs_types.h"

/* Open a resumable body: jump to the resume point in `c->cont`, or fall into
 * the top on a fresh run (cont == 0). Everything between this and WFS_CO_END is
 * one switch statement, which is why a `switch` of your own containing a yield
 * cannot appear there. */
#define WFS_CO_BEGIN(c)                                                                            \
    switch ((c)->cont) {                                                                           \
    case 0:

/* Close the body: reset the context for reuse and report success. Falling off
 * the end of a step is equivalent to WFS_CO_DONE — reaching WFS_CO_END is the
 * normal completion path, not an error, and nothing after it is reachable. */
#define WFS_CO_END(c)                                                                              \
    }                                                                                              \
    (c)->cont = 0;                                                                                 \
    return WFS_R_DONE

/* Ensure block `blk_no` is staged in wfs_block_data(b); yield (submitting the
 * read) if it is not already there, resuming here on completion. The reactor
 * sets b->owner to the active op before stepping it, so a completion knows whom
 * to resume. */
#define WFS_CO_READ(c, b, blk_no)                                                                  \
    do {                                                                                           \
        (c)->cont = __LINE__;                                                                      \
    case __LINE__: {                                                                               \
        wfs_r_t _cr = wfs_block_need((b), (blk_no));                                               \
        if (_cr != WFS_R_DONE)                                                                     \
            return _cr;                                                                            \
    }                                                                                              \
    } while (0)

/* Push wfs_block_data(b) to `blk_no`; yield until it completes, resuming here.
 * Fill the buffer before invoking this. */
#define WFS_CO_WRITE(c, b, blk_no)                                                                 \
    do {                                                                                           \
        (c)->cont = __LINE__;                                                                      \
    case __LINE__: {                                                                               \
        wfs_r_t _cr = wfs_block_write((b), (blk_no));                                              \
        if (_cr != WFS_R_DONE)                                                                     \
            return _cr;                                                                            \
    }                                                                                              \
    } while (0)

/* Run sub-coroutine `callexpr` (a wfs_r_t-returning step on an embedded
 * context) to completion, propagating its yields and errors upward. The
 * sub-machine does its own I/O and may restage the buffer arbitrarily, so a
 * step must re-issue WFS_CO_READ after one before touching the block again. */
#define WFS_CO_AWAIT(c, callexpr)                                                                  \
    do {                                                                                           \
        (c)->cont = __LINE__;                                                                      \
    case __LINE__: {                                                                               \
        wfs_r_t _cr = (callexpr);                                                                  \
        if (_cr != WFS_R_DONE)                                                                     \
            return _cr;                                                                            \
    }                                                                                              \
    } while (0)

/* Fail with a packed WASMOS_ERR_FS_* code, recorded on the op that owns the
 * block buffer, which the reactor reports to the client. The context is reset
 * so it can be reused for a fresh run. */
#define WFS_CO_FAIL(c, b, code)                                                                    \
    do {                                                                                           \
        (c)->cont = 0;                                                                             \
        wfs_block_set_err((b), (code));                                                            \
        return WFS_R_ERR;                                                                          \
    } while (0)

/* Complete the coroutine successfully before the end of its body. */
#define WFS_CO_DONE(c)                                                                             \
    do {                                                                                           \
        (c)->cont = 0;                                                                             \
        return WFS_R_DONE;                                                                         \
    } while (0)

#endif /* FS_WFS_WFS_CO_H */
