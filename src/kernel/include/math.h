/* math.h - Minimal floating-point math declarations for kernel/wasm3 builds. */
#ifndef WASMOS_MATH_H
#define WASMOS_MATH_H

#include <stdint.h>

/* Quiet NaN, produced by evaluating 0.0/0.0 rather than by naming a bit pattern. */
#ifndef NAN
#define NAN (0.0 / 0.0)
#endif

/* Non-zero when x is any NaN, relying on NaN comparing unequal to itself.  This is the
 * whole test: it does not distinguish quiet from signalling NaNs, and it requires that
 * the build has not enabled fast-math, which licenses the compiler to fold `x != x` to
 * false. */
static inline int isnan(double x) {
    return x != x;
}

static inline int isnanf(float x) {
    return x != x;
}

/* Sign bit of x as 0 or 1, read straight out of the IEEE-754 encoding, so it reports 1 for
 * -0.0 and for a negative NaN — which is exactly what distinguishes it from `x < 0`. */
static inline int signbit(double x) {
    union {
        double d;
        uint64_t u;
    } v;
    v.d = x;
    return (int)((v.u >> 63) & 1u);
}

static inline int signbitf(float x) {
    union {
        float f;
        uint32_t u;
    } v;
    v.f = x;
    return (int)((v.u >> 31) & 1u);
}

/* Float math functions required by wasm3 with d_m3HasFloat enabled.
 * All map directly to compiler builtins (SSE2 instructions on x86_64).
 * IEEE-754 semantics throughout, including NaN and infinity propagation.  nearbyint and
 * rint both round to the current rounding mode (nearest-even unless MXCSR was changed);
 * they differ only in whether the inexact exception may be raised, which nothing here
 * observes.  These execute SSE instructions, so kernel SIMD must already be enabled. */

static inline double fabs(double x) {
    return __builtin_fabs(x);
}
static inline float fabsf(float x) {
    return __builtin_fabsf(x);
}

static inline double ceil(double x) {
    return __builtin_ceil(x);
}
static inline float ceilf(float x) {
    return __builtin_ceilf(x);
}

static inline double floor(double x) {
    return __builtin_floor(x);
}
static inline float floorf(float x) {
    return __builtin_floorf(x);
}

static inline double trunc(double x) {
    return __builtin_trunc(x);
}
static inline float truncf(float x) {
    return __builtin_truncf(x);
}

static inline double nearbyint(double x) {
    return __builtin_nearbyint(x);
}
static inline float nearbyintf(float x) {
    return __builtin_nearbyintf(x);
}

static inline double rint(double x) {
    return __builtin_rint(x);
}
static inline float rintf(float x) {
    return __builtin_rintf(x);
}

static inline double sqrt(double x) {
    return __builtin_sqrt(x);
}
static inline float sqrtf(float x) {
    return __builtin_sqrtf(x);
}

static inline double copysign(double x, double y) {
    return __builtin_copysign(x, y);
}
static inline float copysignf(float x, float y) {
    return __builtin_copysignf(x, y);
}

#endif
