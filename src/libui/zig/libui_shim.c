/* libui_shim.c
 * Freestanding WASM C shim for libui.
 *
 * Compiled by Zig's built-in Clang for wasm32-freestanding alongside the Zig
 * root source.  Provides:
 *   1. Arena malloc/free (static 12 KB buffer — sufficient for a full libui
 *      context + component arrays for typical GUI apps).
 *   2. memset / memcpy / memmove / strlen / strcmp / strncmp / strchr
 *      (used internally by libui's static-inline functions).
 *   3. libui_zig_* wrappers that expose libui as plain C functions with
 *      opaque context pointers so libui.zig needs no @cImport.
 *
 * Include-path ordering requirement (enforced by WasmosZigApp.cmake):
 *   -I src/libc/zig/compat   ← intercepts <stdlib.h> / <string.h>
 *   -I src/libui/include     ← wasmos/libui.h
 *   -I src/libc/include      ← wasmos/api.h, wasmos/ipc.h, …
 *   -I src/drivers/include   ← wasmos_driver_abi.h (pulled by wasmos/ipc.h)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Arena allocator (malloc / free / realloc / calloc)
 * Simple bump allocator backed by a 12 KB static buffer.  Sufficient for
 * one ui_context_t + up to ~30 components + label strings.  free() is a
 * no-op; realloc() allocates new space and leaks the old block (acceptable
 * given the fixed, small working set of a single-window GUI app).
 * ------------------------------------------------------------------------- */

#define ARENA_SIZE 12288

static uint8_t  g_arena[ARENA_SIZE];
static uint32_t g_arena_pos;

void *malloc(size_t size)
{
    if (!size) return 0;
    size = (size + 7u) & ~7u; /* 8-byte align */
    if (g_arena_pos + (uint32_t)size > ARENA_SIZE) return 0;
    void *ptr = &g_arena[g_arena_pos];
    g_arena_pos += (uint32_t)size;
    /* calloc-style zero-init so libui structs start clean */
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < size; i++) p[i] = 0;
    return ptr;
}

void free(void *ptr)
{
    (void)ptr; /* arena: no-op */
}

void *calloc(size_t n, size_t size)
{
    return malloc(n * size); /* malloc already zeroes */
}

void *realloc(void *old, size_t size)
{
    void *n = malloc(size);
    if (!n || !old) return n;
    /* Copy old content (conservatively – we don't track block sizes). */
    uint8_t *dst = (uint8_t *)n;
    const uint8_t *src = (const uint8_t *)old;
    /* Safe upper bound: new size. Caller must not have written past it. */
    for (size_t i = 0; i < size; i++) dst[i] = src[i];
    return n;
}

long int strtol(const char *s, char **end, int base)
{
    (void)end; (void)base;
    long int v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); }
    return neg ? -v : v;
}

/* -------------------------------------------------------------------------
 * String primitives (no recursion risk — manual loops, no libc calls)
 * ------------------------------------------------------------------------- */

void *memset(void *s, int c, size_t n)
{
    /* Word-optimized (8 bytes/store, 32-byte unrolled stride); no libc calls or
     * __builtin_memset, so no recursion risk and no bulk-memory dependency. */
    uint8_t *d = (uint8_t *)s;
    const uint8_t v8 = (uint8_t)c;
    uint64_t v64 = v8;
    v64 |= v64 << 8; v64 |= v64 << 16; v64 |= v64 << 32;
    while (n && ((uintptr_t)d & 7u)) { *d++ = v8; n--; }
    uint64_t *q = (uint64_t *)d;
    while (n >= 32) { q[0] = v64; q[1] = v64; q[2] = v64; q[3] = v64; q += 4; n -= 32; }
    while (n >= 8) { *q++ = v64; n -= 8; }
    d = (uint8_t *)q;
    while (n--) { *d++ = v8; }
    return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c) return (void *)(p + i);
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++)) {}
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (!s1[i]) return 0;
    }
    return 0;
}

char *strcat(char *dst, const char *src)
{
    char *r = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
    return r;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *r = dst;
    while (*dst) dst++;
    for (size_t i = 0; i < n && src[i]; i++) *dst++ = src[i];
    *dst = 0;
    return r;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    if (!c) return (char *)s;
    return 0;
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) last = s;
    if (!c) return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Include libui (all static-inline functions are compiled into this TU).
 * wasmos/api.h hostcall declarations use __attribute__((import_module,...))
 * which Clang for __wasm__ resolves to the correct WASM import namespace.
 * ------------------------------------------------------------------------- */

#include "wasmos/libui.h"

/* -------------------------------------------------------------------------
 * libui_zig_* wrappers
 * These are non-inline C functions that libui.zig calls via extern "C".
 * Using void* for the context avoids exposing the C struct layout to Zig.
 * ------------------------------------------------------------------------- */

void *libui_zig_alloc_ctx(void)
{
    return malloc(sizeof(ui_context_t));
}

int32_t libui_zig_ui_init(void *ctx, int32_t proc_ep, int32_t reply_ep,
                          int32_t w, int32_t h)
{
    return ui_init((ui_context_t *)ctx, proc_ep, reply_ep, w, h);
}

void libui_zig_ui_destroy(void *ctx)
{
    ui_destroy((ui_context_t *)ctx);
}

void libui_zig_set_title(void *ctx, const char *title)
{
    (void)ui_window_set_title((ui_context_t *)ctx, title);
}

int32_t libui_zig_close_requested(const void *ctx)
{
    return ((const ui_context_t *)ctx)->close_requested;
}

void libui_zig_mark_dirty(void *ctx)
{
    ui_mark_dirty((ui_context_t *)ctx);
}

int32_t libui_zig_drain(void *ctx)
{
    return ui_loop_drain((ui_context_t *)ctx);
}

/* Poll one GFX event then layout+render if dirty. */
void libui_zig_poll_and_drain(void *ctx)
{
    ui_context_t *c = (ui_context_t *)ctx;
    if (!c->close_requested) {
        wasmos_ipc_message_t ev;
        ui_send_gfx_raw(c->gfx_endpoint, c->reply_endpoint, c->req_id++,
                        GFX_IPC_POLL_EVENT, 0, 0, 0, 0, &ev);
        ui_loop_handle_ipc(c, &ev);
    }
    ui_loop_drain(c);
}

int32_t libui_zig_root_id(const void *ctx)
{
    return ((const ui_context_t *)ctx)->root_id;
}

/* Component creation */
int32_t libui_zig_create_panel(void *ctx)
{
    return ui_component_create_panel((ui_context_t *)ctx);
}

int32_t libui_zig_create_label(void *ctx)
{
    return ui_component_create_label((ui_context_t *)ctx);
}

int32_t libui_zig_create_button(void *ctx)
{
    return ui_component_create_button((ui_context_t *)ctx);
}

int32_t libui_zig_create_menu_bar(void *ctx)
{
    return ui_component_create_menu_bar((ui_context_t *)ctx);
}

/* Component tree */
void libui_zig_append_child(void *ctx, int32_t parent_id, int32_t child_id)
{
    (void)ui_component_append_child((ui_context_t *)ctx, parent_id, child_id);
}

/* Text */
void libui_zig_set_text(void *ctx, int32_t id, const char *text)
{
    ui_component_set_text((ui_context_t *)ctx, id, text);
}

/* Button action callback.  The signature matches ui_button_click_cb_t because
 * ui_context_t* and void* have identical representation in wasm32. */
typedef void (*libui_zig_cb_t)(void *ctx, int32_t id, void *user);

void libui_zig_set_button_action(void *ctx, int32_t id,
                                 libui_zig_cb_t cb, void *user)
{
    ui_component_set_button_action((ui_context_t *)ctx, id,
                                   (ui_button_click_cb_t)cb, user);
}

/* Component property setters */
static ui_component_t *comp(void *ctx, int32_t id)
{
    return ui_component_by_id((ui_context_t *)ctx, id);
}

void libui_zig_set_bg_color(void *ctx, int32_t id, uint32_t color)
{
    ui_component_t *c = comp(ctx, id); if (c) c->bg_color = color;
}

void libui_zig_set_fg_color(void *ctx, int32_t id, uint32_t color)
{
    ui_component_t *c = comp(ctx, id); if (c) c->fg_color = color;
}

void libui_zig_set_border_color(void *ctx, int32_t id, uint32_t color)
{
    ui_component_t *c = comp(ctx, id); if (c) c->border_color = color;
}

void libui_zig_set_preferred_h(void *ctx, int32_t id, int32_t h)
{
    ui_component_t *c = comp(ctx, id); if (c) c->preferred_h = h;
}

void libui_zig_set_padding_px(void *ctx, int32_t id, int32_t px)
{
    ui_component_t *c = comp(ctx, id); if (c) c->padding_px = px;
}

void libui_zig_set_gap_px(void *ctx, int32_t id, int32_t px)
{
    ui_component_t *c = comp(ctx, id); if (c) c->gap_px = px;
}

void libui_zig_set_border_px(void *ctx, int32_t id, int32_t px)
{
    ui_component_t *c = comp(ctx, id); if (c) c->border_px = px;
}

void libui_zig_set_clickable(void *ctx, int32_t id, int32_t val)
{
    ui_component_t *c = comp(ctx, id); if (c) c->clickable = val;
}
