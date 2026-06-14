/* warp/sjlj_unwind.cpp - Minimal SJLJ exception unwind runtime for bare-metal.
 *
 * With -fsjlj-exceptions the compiler generates:
 *   - A try block entry that calls _Unwind_SjLj_Register(&ctx) and then does
 *     an inline __builtin_setjmp(ctx.jbuf).
 *   - A try block exit that calls _Unwind_SjLj_Unregister(&ctx).
 *   - A throw that calls __cxa_throw → _Unwind_SjLj_RaiseException(exc).
 *
 * Our runtime walks the context stack and __builtin_longjmps to the first
 * frame whose personality function accepts the exception.  catch(...) always
 * accepts, so it always catches.
 *
 * No setjmp.h is needed: the compiler emits the setjmp inline; we only call
 * __builtin_longjmp (a clang intrinsic, no header required).
 *
 * Context stack is per-CPU-slot because WARP execution is serialized by
 * warp_runtime_enter (only one CPU inside WARP at any time). */

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "klog.h"
#include "arch/x86_64/smp.h"
}

// ---------------------------------------------------------------------------
// _Unwind_FunctionContext — must match the layout clang generates for
// -fsjlj-exceptions on x86_64.  The jbuf is a 5-void* __builtin_setjmp buf.
// ---------------------------------------------------------------------------

typedef int32_t  _Unwind_Reason_Code;
typedef int32_t  _Unwind_Action;
typedef uint64_t _Unwind_Exception_Class;

static constexpr _Unwind_Reason_Code _URC_NO_REASON       = 0;
static constexpr _Unwind_Reason_Code _URC_HANDLER_FOUND   = 6;
static constexpr _Unwind_Reason_Code _URC_CONTINUE_UNWIND = 8;
static constexpr _Unwind_Action      _UA_SEARCH_PHASE     = 1;

struct _Unwind_Exception {
    _Unwind_Exception_Class exception_class;
    void (*exception_cleanup)(_Unwind_Reason_Code, _Unwind_Exception *);
    unsigned long private_1;
    unsigned long private_2;
};

typedef _Unwind_Reason_Code (*__personality_routine)(
    int version, _Unwind_Action actions, _Unwind_Exception_Class exclass,
    _Unwind_Exception *exc, void *context);

struct _Unwind_FunctionContext {
    _Unwind_FunctionContext *prev;
    int32_t                  resumeIndex;
    void                    *resumeParameters[4];
    __personality_routine    personality;
    uintptr_t                lsda;
    void                    *jbuf[5];   /* __builtin_setjmp buffer */
};

// ---------------------------------------------------------------------------
// Per-CPU-slot context stack (WARP is serialized by warp_runtime_enter)
// ---------------------------------------------------------------------------

static _Unwind_FunctionContext *g_sjlj_top[64];

static inline _Unwind_FunctionContext **ctx_stack(void)
{
    uint32_t id = cpu_local()->cpu_id;
    return &g_sjlj_top[id < 64u ? id : 0u];
}

// ---------------------------------------------------------------------------
// SJLJ ABI
// ---------------------------------------------------------------------------

extern "C" {

void _Unwind_SjLj_Register(_Unwind_FunctionContext *ctx)
{
    _Unwind_FunctionContext **top = ctx_stack();
    ctx->prev = *top;
    *top      = ctx;
}

void _Unwind_SjLj_Unregister(_Unwind_FunctionContext *ctx)
{
    _Unwind_FunctionContext **top = ctx_stack();
    if (*top == ctx) *top = ctx->prev;
}

_Unwind_Reason_Code _Unwind_SjLj_RaiseException(_Unwind_Exception *exc)
{
    _Unwind_FunctionContext **top = ctx_stack();
    _Unwind_FunctionContext  *ctx = *top;

    while (ctx) {
        if (ctx->personality) {
            _Unwind_Reason_Code rc = ctx->personality(
                1, _UA_SEARCH_PHASE, exc->exception_class, exc, ctx);
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
    for (;;) { __asm__ volatile("hlt"); }
}

_Unwind_Reason_Code _Unwind_SjLj_Resume(_Unwind_Exception *exc)
{
    return _Unwind_SjLj_RaiseException(exc);
}

_Unwind_Reason_Code _Unwind_SjLj_Resume_or_Rethrow(_Unwind_Exception *exc)
{
    return _Unwind_SjLj_RaiseException(exc);
}

/* SJLJ C++ personality function — called by _Unwind_SjLj_RaiseException for
 * each frame.  Returns HANDLER_FOUND for every frame so that catch(...) in
 * warp_driver.cpp always catches WARP exceptions at the first opportunity. */
_Unwind_Reason_Code __gxx_personality_sj0(
    int /*version*/, _Unwind_Action actions, _Unwind_Exception_Class /*exclass*/,
    _Unwind_Exception * /*exc*/, void * /*context*/)
{
    if (actions & _UA_SEARCH_PHASE) return _URC_HANDLER_FOUND;
    return _URC_CONTINUE_UNWIND;
}

void _Unwind_SjLj_DeleteException(_Unwind_Exception *exc)
{
    if (exc && exc->exception_cleanup)
        exc->exception_cleanup(_URC_NO_REASON, exc);
}

} // extern "C"
