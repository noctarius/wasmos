/* test_libc_math.c - unit tests for the wasmos libc float helpers.
 * Compiled against the real math.c on the host.
 *
 * sqrtf is what this file exists for. It is Newton-Raphson, so the property
 * that matters is not one value but CONVERGENCE ACROSS THE EXPONENT RANGE: an
 * implementation seeded with x itself spends one iteration per octave before
 * quadratic convergence begins, so it is accurate near 1 and arbitrarily wrong
 * away from it. A suite that only checks sqrtf(4) passes against exactly that
 * bug -- the previous implementation did, returning 2.4e6 for sqrtf(1e10) and
 * being wrong by six orders the other way at 1e-20.
 *
 * Accuracy is checked against the DEFINING PROPERTY, got*got == x, evaluated in
 * double, rather than against another sqrt implementation. That avoids pulling
 * the host libm into a translation unit that already compiles against the
 * project's <math.h> (the two cannot both be in scope), and it is the stronger
 * statement anyway: agreeing with a second implementation is only as good as
 * that implementation. Squaring doubles the relative error, so the 2e-6
 * tolerance corresponds to ~1e-6 in the root -- comfortably above float epsilon
 * (~1.2e-7) and far below the ~1e-1 and larger errors the seeding bug produced,
 * pinning the defect without pinning the iteration count.
 */
#include <stddef.h>
#include <stdint.h>

#include "test_shuffle.h"

float sqrtf(float x);

#define SQUARE_REL_TOLERANCE 2e-6

/* True when got is a square root of x: |got^2 - x| / x within tolerance. x is
 * always positive at the call sites, so the division is safe. */
static int close_enough(float got, float x) {
    double squared = (double)got * (double)got;
    double error = squared - (double)x;
    if (error < 0.0) {
        error = -error;
    }
    return (error / (double)x) <= SQUARE_REL_TOLERANCE;
}

/* Arguments near 1, where even the badly seeded version converged. These would
 * pass against the bug and are here so a future rewrite cannot regress the easy
 * range while fixing the hard one. */
static int test_sqrtf_near_one(void) {
    const float xs[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 100.0f};
    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i) {
        if (!close_enough(sqrtf(xs[i]), xs[i])) {
            return __LINE__;
        }
    }
    return 0;
}

/* The large half of the range. 1e7 is roughly where the old 12-iteration form
 * began to diverge; 3.4e38 is the top of finite float. */
static int test_sqrtf_large(void) {
    const float xs[] = {1.0e6f, 1.0e7f, 1.0e10f, 1.0e20f, 1.0e30f, 3.4e38f};
    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i) {
        if (!close_enough(sqrtf(xs[i]), xs[i])) {
            return __LINE__;
        }
    }
    return 0;
}

/* The small half. The old form was wrong here too, which the task description
 * for this bug did not mention: it reported only the large-argument failure. */
static int test_sqrtf_small(void) {
    const float xs[] = {1.0e-6f, 1.0e-10f, 1.0e-20f, 1.0e-30f};
    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i) {
        if (!close_enough(sqrtf(xs[i]), xs[i])) {
            return __LINE__;
        }
    }
    return 0;
}

/* Exact squares must come back exact, not merely close: a converged Newton step
 * on a representable root lands on it. */
static int test_sqrtf_exact_squares(void) {
    const float xs[] = {1.0f, 4.0f, 9.0f, 16.0f, 65536.0f, 1048576.0f};
    const float roots[] = {1.0f, 2.0f, 3.0f, 4.0f, 256.0f, 1024.0f};
    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i) {
        if (sqrtf(xs[i]) != roots[i]) {
            return __LINE__;
        }
    }
    return 0;
}

/* Domain edges. Zero, negatives and NaN all answer 0 rather than trapping or
 * looping: this libc has no errno and its callers cannot handle a trap. */
static int test_sqrtf_domain_edges(void) {
    if (sqrtf(0.0f) != 0.0f) {
        return __LINE__;
    }
    if (sqrtf(-1.0f) != 0.0f) {
        return __LINE__;
    }
    if (sqrtf(-1.0e20f) != 0.0f) {
        return __LINE__;
    }
    /* NaN takes the same branch; written so the compiler cannot fold it. */
    volatile float zero = 0.0f;
    float nan_value = zero / zero;
    if (sqrtf(nan_value) != 0.0f) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_sqrtf_near_one),
        WASMOS_TEST_CASE(test_sqrtf_large),
        WASMOS_TEST_CASE(test_sqrtf_small),
        WASMOS_TEST_CASE(test_sqrtf_exact_squares),
        WASMOS_TEST_CASE(test_sqrtf_domain_edges),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
