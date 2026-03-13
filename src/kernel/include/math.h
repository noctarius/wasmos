#ifndef WASMOS_MATH_H
#define WASMOS_MATH_H

#include <stdint.h>

#ifndef NAN
#define NAN (0.0/0.0)
#endif

static inline int isnan(double x)
{
    return x != x;
}

static inline int isnanf(float x)
{
    return x != x;
}

static inline int signbit(double x)
{
    union {
        double d;
        uint64_t u;
    } v;
    v.d = x;
    return (int)((v.u >> 63) & 1u);
}

static inline int signbitf(float x)
{
    union {
        float f;
        uint32_t u;
    } v;
    v.f = x;
    return (int)((v.u >> 31) & 1u);
}

#endif
