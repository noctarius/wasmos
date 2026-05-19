#include <stddef.h>
#include <stdint.h>
#include "string.h"
#include "math.h"

static uint8_t g_stbtt_alloc_buf[512 * 1024];
static size_t g_stbtt_alloc_off = 0;

static size_t align_up(size_t v, size_t a)
{
    return (v + (a - 1u)) & ~(a - 1u);
}

void *wasmos_stbtt_malloc(size_t size, void *user)
{
    (void)user;
    if (size == 0) {
        return NULL;
    }
    size_t off = align_up(g_stbtt_alloc_off, sizeof(uintptr_t));
    if (off + size > sizeof(g_stbtt_alloc_buf)) {
        return NULL;
    }
    g_stbtt_alloc_off = off + size;
    return &g_stbtt_alloc_buf[off];
}

void wasmos_stbtt_free(void *ptr, void *user)
{
    (void)ptr;
    (void)user;
}

void wasmos_stbtt_alloc_reset(void)
{
    g_stbtt_alloc_off = 0;
}

#define STBTT_ifloor(x) ((int)floorf(x))
#define STBTT_iceil(x) ((int)ceilf(x))
#define STBTT_sqrt(x) sqrtf(x)
#define STBTT_pow(x, y) powf((x), (y))
#define STBTT_fmod(x, y) fmodf((x), (y))
#define STBTT_cos(x) cosf(x)
#define STBTT_acos(x) acosf(x)
#define STBTT_fabs(x) fabsf(x)
#define STBTT_malloc(x, u) wasmos_stbtt_malloc((x), (u))
#define STBTT_free(x, u) wasmos_stbtt_free((x), (u))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../../libs/stb/stb_truetype.h"
