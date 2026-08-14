/* math.h - Minimal floating-point math declarations for WASM libc. */
#ifndef WASMOS_LIBC_MATH_H
#define WASMOS_LIBC_MATH_H

/* These are approximations sized for the callers that exist, not a conforming
 * libm: none of them set errno, raise FP exceptions, or handle NaN/infinity, and
 * several return 0 where ISO C requires NaN. The accuracy notes below bound the
 * error over the stated range only.
 *
 * fabsf is exact.
 *
 * floorf/ceilf round via a 32-bit integer conversion and return |x| >= 2^31
 * unchanged (already integral at that magnitude). A NaN argument reaches the
 * int conversion, which is undefined for it.
 *
 * fmodf(x, 0) returns 0 rather than NaN. The remainder is computed as
 * x - trunc(x/y)*y, so it loses precision when |x/y| is large.
 *
 * sqrtf(x <= 0) returns 0, negatives included, rather than NaN. It runs a fixed
 * 12 Newton iterations seeded with x itself, which is enough for arguments up to
 * about 1e6 (relative error there ~5e-4) and exact for small perfect squares;
 * beyond ~1e7 the iteration has not converged and the result is far too large
 * (1e10 comes back ~24x high).
 *
 * cosf reduces modulo 2*pi, folds into [0, pi/2] and evaluates a degree-6 Taylor
 * polynomial: the absolute error is around 1e-4 at 1.2 rad and grows toward
 * ~1e-3 at pi/2. Large arguments additionally lose the precision the modulo
 * reduction costs.
 *
 * acosf clamps to 0 for x >= 1 and pi for x <= -1, and elsewhere evaluates a
 * five-term asin series through sqrtf.
 *
 * powf(x, 0) is 1 and powf(0, y) is 0 for every other y, including negative y.
 * An integer y uses |y| repeated multiplications (exact up to rounding, but the
 * loop cost is linear in |y|). A fractional y is exp(y*ln x), which inherits a
 * log error of up to ~0.08 absolute just below each power of two, so results
 * there can be off by a few percent; a fractional y with x <= 0 returns 0. */
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float fmodf(float x, float y);
float sqrtf(float x);
float cosf(float x);
float acosf(float x);
float powf(float x, float y);

#endif
