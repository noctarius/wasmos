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

float
cosf(float x)
{
    const float x2 = x * x;
    const float x4 = x2 * x2;
    const float x6 = x4 * x2;
    return 1.0f - (x2 * 0.5f) + (x4 * (1.0f / 24.0f)) - (x6 * (1.0f / 720.0f));
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

float
powf(float x, float y)
{
    int yi = (int)y;
    if ((float)yi != y) {
        return 1.0f;
    }
    float out = 1.0f;
    int n = yi < 0 ? -yi : yi;
    for (int i = 0; i < n; ++i) {
        out *= x;
    }
    if (yi < 0) {
        return (out == 0.0f) ? 0.0f : (1.0f / out);
    }
    return out;
}
