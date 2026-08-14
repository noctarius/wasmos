/* warp/sjlj_unwind.cpp - Minimal SJLJ exception unwind runtime for bare-metal.
 *
 * Dormant in the current build: the WARP translation units compile with
 * -fexceptions (Dwarf), so nothing calls the entry points below.  It is linked
 * so that switching the WARP flags to -fsjlj-exceptions needs no new code.
 *
 * With -fsjlj-exceptions the compiler generates:
 *   - A try block entry that calls _Unwind_SjLj_Register(&ctx) and then does
 *     an inline __builtin_setjmp(ctx.jbuf).
 *   - A try block exit that calls _Unwind_SjLj_Unregister(&ctx).
 *   - A throw that calls __cxa_throw → _Unwind_SjLj_RaiseException(exc).
 *
 * This implementation walks the context stack and __builtin_longjmps to the
 * first frame whose personality function accepts the exception.  catch(...)
 * always accepts, so it always catches.
 *
 * No setjmp.h is needed: the compiler emits the setjmp inline, and the only
 * call made here is __builtin_longjmp (a clang intrinsic, no header required).
 *
 * The context stack is per-CPU-slot, which is sound only under the WARP
 * single-CPU invariant (one CPU inside WARP at a time).  That invariant is an
 * assumption, not something warp_runtime_enter enforces — see the
 * FIXME(smp-warp) in warp/shim.cpp. */

#include <stdint.h>
#include <stddef.h>

#include "../include/kpanic.h"

extern "C" {
#include "klog.h"
#include "kpanic.h"
#include "arch/x86_64/smp.h"
}

// ---------------------------------------------------------------------------
// _Unwind_FunctionContext — must match the layout clang generates for
// -fsjlj-exceptions on x86_64.  The jbuf is a 5-void* __builtin_setjmp buf.
// ---------------------------------------------------------------------------

typedef int32_t _Unwind_Reason_Code;
typedef int32_t _Unwind_Action;
typedef uint64_t _Unwind_Exception_Class;

static constexpr _Unwind_Reason_Code _URC_NO_REASON = 0;
static constexpr _Unwind_Reason_Code _URC_HANDLER_FOUND = 6;
static constexpr _Unwind_Reason_Code _URC_CONTINUE_UNWIND = 8;
static constexpr _Unwind_Action _UA_SEARCH_PHASE = 1;

struct _Unwind_Exception {
    _Unwind_Exception_Class exception_class;
    void (*exception_cleanup)(_Unwind_Reason_Code, _Unwind_Exception*);
    unsigned long private_1;
    unsigned long private_2;
};

typedef _Unwind_Reason_Code (*__personality_routine)(int version, _Unwind_Action actions,
                                                     _Unwind_Exception_Class exclass,
                                                     _Unwind_Exception* exc, void* context);

struct _Unwind_FunctionContext {
    _Unwind_FunctionContext* prev;
    int32_t resumeIndex;
    void* resumeParameters[4];
    __personality_routine personality;
    uintptr_t lsda;
    void* jbuf[5]; /* __builtin_setjmp buffer */
};

// ---------------------------------------------------------------------------
// Per-CPU-slot context stack (sound only under the WARP single-CPU invariant)
// ---------------------------------------------------------------------------

static _Unwind_FunctionContext* g_sjlj_top[64];

static inline _Unwind_FunctionContext** ctx_stack(void) {
    uint32_t id = cpu_local()->cpu_id;
    return &g_sjlj_top[id < 64u ? id : 0u];
}

// ---------------------------------------------------------------------------
// SJLJ ABI
// ---------------------------------------------------------------------------

extern "C" {

/* Push `ctx` onto this CPU's context stack.  `ctx` is borrowed and lives in the
 * registering frame, so it must be unregistered before that frame returns. */
void _Unwind_SjLj_Register(_Unwind_FunctionContext* ctx) {
    _Unwind_FunctionContext** top = ctx_stack();
    ctx->prev = *top;
    *top = ctx;
}

/* Pop `ctx` if it is the top of this CPU's stack; a non-top context is silently
 * ignored rather than spliced out, so out-of-order unregistration leaves the stack
 * holding a dead frame. */
void _Unwind_SjLj_Unregister(_Unwind_FunctionContext* ctx) {
    _Unwind_FunctionContext** top = ctx_stack();
    if (*top == ctx)
        *top = ctx->prev;
}

/* Deliver `exc` to the innermost registered frame whose personality accepts it,
 * popping that frame and longjmping to its landing pad.  Does not return on success;
 * panics when no frame accepts.  `exc` is borrowed and is not freed here — the landing
 * pad owns it.  Frames skipped on the way are NOT unwound, so their destructors do not
 * run. */
_Unwind_Reason_Code _Unwind_SjLj_RaiseException(_Unwind_Exception* exc) {
    _Unwind_FunctionContext** top = ctx_stack();
    _Unwind_FunctionContext* ctx = *top;

    while (ctx) {
        if (ctx->personality) {
            _Unwind_Reason_Code rc =
                ctx->personality(1, _UA_SEARCH_PHASE, exc->exception_class, exc, ctx);
            if (rc == _URC_HANDLER_FOUND) {
                /* Pop this frame and longjmp to its catch landing pad.
                 * __builtin_longjmp val must be the compile-time constant 1;
                 * the compiler selects the landing pad via resumeIndex. */
                *top = ctx->prev;
                __builtin_longjmp(ctx->jbuf, 1);
            }
        }
        ctx = ctx->prev;
    }

    klog_write("[warp] uncaught exception in sjlj handler\n");
    kpanic("uncaught_sjlj_exception_panic", 0ULL, 0ULL);
}

/* Resume/rethrow both restart delivery from the current top of the context stack,
 * which is the frame after the one that was popped, so an exception rethrown from a
 * landing pad propagates outward. */
_Unwind_Reason_Code _Unwind_SjLj_Resume(_Unwind_Exception* exc) {
    return _Unwind_SjLj_RaiseException(exc);
}

_Unwind_Reason_Code _Unwind_SjLj_Resume_or_Rethrow(_Unwind_Exception* exc) {
    return _Unwind_SjLj_RaiseException(exc);
}

/* SJLJ C++ personality function — called by _Unwind_SjLj_RaiseException for
 * each frame.  Returns HANDLER_FOUND unconditionally, so an exception is
 * delivered to the innermost frame that registered a landing pad, whatever its
 * declared catch type. */
_Unwind_Reason_Code __gxx_personality_sj0(int /*version*/, _Unwind_Action actions,
                                          _Unwind_Exception_Class /*exclass*/,
                                          _Unwind_Exception* /*exc*/, void* /*context*/) {
    if (actions & _UA_SEARCH_PHASE)
        return _URC_HANDLER_FOUND;
    return _URC_CONTINUE_UNWIND;
}

/* Run `exc`'s cleanup callback if it has one; the exception object's own storage is
 * not freed here.  A null `exc` is a no-op. */
void _Unwind_SjLj_DeleteException(_Unwind_Exception* exc) {
    if (exc && exc->exception_cleanup)
        exc->exception_cleanup(_URC_NO_REASON, exc);
}

} // extern "C"
