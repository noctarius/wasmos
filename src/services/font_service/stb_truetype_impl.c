/* stb_truetype_impl.c - the single translation unit that instantiates the
 * vendored stb_truetype implementation, with the STBTT_* hooks bound to the
 * freestanding libc this service links.
 *
 * Allocation is a bump arena, not a heap: wasmos_stbtt_free is a no-op, so every
 * glyph rasterisation leaks into g_stbtt_alloc_buf until a caller calls
 * wasmos_stbtt_alloc_reset. A caller must therefore reset before each
 * self-contained stbtt operation, or the arena fills and stbtt allocations start
 * returning NULL. */
#include <stddef.h>
#include <stdint.h>
#include "string.h"
#include "math.h"

static uint8_t g_stbtt_alloc_buf[512 * 1024];
static size_t g_stbtt_alloc_off = 0;

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

/* STBTT_malloc hook.  Bump-allocates `size` bytes from the static arena, aligned
 * to a pointer boundary.  `user` is stbtt's opaque userdata and is ignored.
 * Returns NULL for a zero size and when the arena has no room left — stbtt
 * handles a NULL by abandoning the operation, so exhaustion shows up as a failed
 * rasterisation, not a crash.  The returned block stays valid until the next
 * wasmos_stbtt_alloc_reset, which invalidates every outstanding block at once. */
void* wasmos_stbtt_malloc(size_t size, void* user) {
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

/* STBTT_free hook.  A no-op: the arena has no per-block free.  Space is only
 * reclaimed by wasmos_stbtt_alloc_reset. */
void wasmos_stbtt_free(void* ptr, void* user) {
    (void)ptr;
    (void)user;
}

/* Rewind the arena to empty, invalidating every pointer wasmos_stbtt_malloc has
 * returned since the last reset.  Call it immediately BEFORE a self-contained
 * stbtt operation, never between allocating and using a glyph — the font service
 * calls it once per raster request.  Cheap (a single store) and always
 * succeeds. */
void wasmos_stbtt_alloc_reset(void) {
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
