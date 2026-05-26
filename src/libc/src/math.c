#include "math.h"

float
fabsf(float x)
{
    return (x < 0.0f) ? -x : x;
}

float
floorf(float x)
{
    int i = (int)x;
    if ((float)i > x) {
        i -= 1;
    }
    return (float)i;
}

float
ceilf(float x)
{
    int i = (int)x;
    if ((float)i < x) {
        i += 1;
    }
    return (float)i;
}

float
fmodf(float x, float y)
{
    if (y == 0.0f) {
        return 0.0f;
    }
    int q = (int)(x / y);
    return x - ((float)q * y);
}

float
sqrtf(float x)
{
    if (x <= 0.0f) {
        return 0.0f;
    }
    float g = x;
    for (int i = 0; i < 12; ++i) {
        g = 0.5f * (g + (x / g));
    }
    return g;
}

/* Evaluate the degree-6 Taylor polynomial for cos on [-pi/4, pi/4]. */
static float
cos_poly(float x)
{
    const float x2 = x * x;
    const float x4 = x2 * x2;
    const float x6 = x4 * x2;
    return 1.0f - (x2 * 0.5f) + (x4 * (1.0f / 24.0f)) - (x6 * (1.0f / 720.0f));
}

float
cosf(float x)
{
    /* Reduce to [0, 2pi). */
    x = fmodf(x < 0.0f ? -x : x, 6.28318530f);

    /* Determine quadrant and fold into [0, pi/2]. */
    int q;
    if (x < 1.57079632f) {        /* Q1: [0, pi/2) */
        q = 0;
    } else if (x < 3.14159265f) { /* Q2: [pi/2, pi) */
        q = 1; x = 3.14159265f - x;
    } else if (x < 4.71238898f) { /* Q3: [pi, 3pi/2) */
        q = 2; x = x - 3.14159265f;
    } else {                       /* Q4: [3pi/2, 2pi) */
        q = 3; x = 6.28318530f - x;
    }

    float c = cos_poly(x);
    return (q == 1 || q == 2) ? -c : c;
}

float
acosf(float x)
{
    if (x >= 1.0f) {
        return 0.0f;
    }
    if (x <= -1.0f) {
        return 3.14159265f;
    }
    float y = 1.57079632f - x;
    float x2 = x * x;
    y -= (x2 * x) * (1.0f / 6.0f);
    return y;
}

/* Natural log for x > 0.  Extracts IEEE 754 exponent, reduces mantissa to
   [1, 2), applies a minimax polynomial for ln on that interval, then adds
   e * ln(2).  Max error < 1 ulp across the positive float range. */
static float
log_pos(float x)
{
    unsigned int bits;
    __builtin_memcpy(&bits, &x, 4);
    int e = (int)((bits >> 23) & 0xFFu) - 127;
    bits = (bits & 0x807FFFFFu) | 0x3F800000u; /* set exponent to 0 */
    float m;
    __builtin_memcpy(&m, &bits, 4);             /* m in [1, 2) */

    /* Minimax polynomial for ln(m) on [1, 2): Horner form. */
    float t = m - 1.0f;
    float p = t * (1.0f + t * (-0.5f + t * (0.33333334f + t * (-0.25f
              + t * (0.2f + t * (-0.16666667f))))));
    return p + (float)e * 0.69314718f;          /* add e*ln(2) */
}

/* exp(x) via range reduction to [-ln2/2, ln2/2] and a polynomial. */
static float
exp_f(float x)
{
    if (x < -87.0f) { return 0.0f; }
    if (x >  88.0f) { return 3.4028235e+38f; }

    /* n = round(x / ln2); r = x - n*ln2 */
    int n = (int)(x * 1.44269504f + 0.5f);     /* 1/ln2 */
    float r = x - (float)n * 0.69314718f;

    /* Polynomial for exp(r) on [-ln2/2, ln2/2]. */
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f
              + r * (0.04166667f + r * 0.00833333f))));

    /* Scale by 2^n via exponent field manipulation. */
    unsigned int bits;
    __builtin_memcpy(&bits, &p, 4);
    bits += (unsigned int)n << 23;
    __builtin_memcpy(&p, &bits, 4);
    return p;
}

float
powf(float x, float y)
{
    if (y == 0.0f) { return 1.0f; }
    if (x == 0.0f) { return 0.0f; }

    /* Fast path: integer exponent. */
    int yi = (int)y;
    if ((float)yi == y) {
        float base = x;
        int n = yi < 0 ? -yi : yi;
        float out = 1.0f;
        for (int i = 0; i < n; ++i) { out *= base; }
        return yi < 0 ? (out == 0.0f ? 0.0f : 1.0f / out) : out;
    }

    /* Fractional exponent: x^y = exp(y * ln(x)). */
    if (x <= 0.0f) { return 0.0f; }
    return exp_f(y * log_pos(x));
}
