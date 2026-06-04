/* math.h - Minimal floating-point math declarations for WASM libc. */
#ifndef WASMOS_LIBC_MATH_H
#define WASMOS_LIBC_MATH_H

float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float fmodf(float x, float y);
float sqrtf(float x);
float cosf(float x);
float acosf(float x);
float powf(float x, float y);

#endif
