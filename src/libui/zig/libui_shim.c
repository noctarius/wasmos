/* libui_shim.c
 * Freestanding WASM C shim for libui.
 *
 * Compiled by Zig's built-in Clang for wasm32-freestanding alongside the Zig
 * root source.  Provides:
 *   1. Arena malloc/free (static 12 KB buffer — sufficient for a full libui
 *      context + component arrays for typical GUI apps).
 *   2. The freestanding string/memory primitives libui's static-inline code
 *      and its headers reference (mem*, str*, strtol).
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
 *
 * Memory is never returned to the arena, so the whole process budget is
 * ARENA_SIZE bytes for the lifetime of the app.  Every growth libui performs
 * (component pool, list items, text buffers) allocates a fresh, larger block
 * and abandons the old one, so the true cost of an array that doubles N times
 * is the sum of all N sizes.  Exhaustion surfaces as a NULL from malloc, which
 * libui reports as a -1 from whichever call needed the memory.
 * ------------------------------------------------------------------------- */

/* Total bytes available to the app through malloc for its entire run. */
#define ARENA_SIZE 12288

static uint8_t g_arena[ARENA_SIZE];
static uint32_t g_arena_pos;

/* Allocate `size` bytes, 8-byte aligned and zero-filled.  Returns NULL for a
 * zero size and when the arena cannot satisfy the request; the arena is never
 * grown and the pointer stays valid until the process exits.
 *
 * Differs from the real libc: alignment is fixed at 8 bytes rather than
 * suitable for any type, and the block is zero-filled where C leaves it
 * indeterminate.  Code that relies on either property will not port back to a
 * full libc unchanged. */
void* malloc(size_t size) {
    if (!size)
        return 0;
    size = (size + 7u) & ~7u; /* 8-byte align */
    if (g_arena_pos + (uint32_t)size > ARENA_SIZE)
        return 0;
    void* ptr = &g_arena[g_arena_pos];
    g_arena_pos += (uint32_t)size;
    /* calloc-style zero-init so libui structs start clean */
    uint8_t* p = (uint8_t*)ptr;
    for (size_t i = 0; i < size; i++)
        p[i] = 0;
    return ptr;
}

/* Does nothing.  The arena has no per-block metadata and cannot reclaim, so a
 * freed block stays allocated for the rest of the run.  Accepting any pointer,
 * including NULL and one not from this allocator, is deliberate: callers must
 * still be able to pair every malloc with a free. */
void free(void* ptr) {
    (void)ptr; /* arena: no-op */
}

/* Allocate n * size zeroed bytes.  Weaker than the real libc: the product is
 * computed without an overflow check, so a pair of large arguments wraps and
 * yields a block smaller than requested rather than NULL. */
void* calloc(size_t n, size_t size) {
    return malloc(n * size); /* malloc already zeroes */
}

/* Allocate a new `size`-byte block and copy the old contents into it.  Returns
 * NULL when the arena is exhausted, leaving the old block untouched; a NULL
 * `old` behaves as a plain malloc.  The old block is never reclaimed.
 *
 * Weaker than the real libc: block sizes are not tracked, so the copy length is
 * `size` rather than the smaller of the two — see the FIXME below. */
void* realloc(void* old, size_t size) {
    void* n = malloc(size);
    if (!n || !old)
        return n;
    uint8_t* dst = (uint8_t*)n;
    const uint8_t* src = (const uint8_t*)old;
    /* Block sizes are not tracked, so `size` bytes are copied unconditionally.
     * FIXME: a grow reads past the end of the old block; the bytes past it are
     * arena memory (or, for a block at the arena's tail, past g_arena). */
    for (size_t i = 0; i < size; i++)
        dst[i] = src[i];
    return n;
}

/* Decimal only: `base` is ignored and `end` is never written.
 *
 * Skips leading spaces and tabs (not the full isspace set), accepts one
 * optional sign, then consumes ASCII digits.  Weaker than the real libc in
 * several ways: no other radix, no `end` output, no errno, and no
 * overflow/underflow detection — a value too large for long wraps silently.  A
 * string with no digits yields 0, indistinguishable from a parsed "0". */
long int strtol(const char* s, char** end, int base) {
    (void)end;
    (void)base;
    long int v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+')
        s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return neg ? -v : v;
}

/* -------------------------------------------------------------------------
 * String primitives (no recursion risk — manual loops, no libc calls)
 *
 * These follow the standard signatures and return values.  Common to all of
 * them, and weaker than a real libc: no argument is null-checked, no size is
 * validated, and nothing detects a destination too small — the caller owes
 * every precondition the C standard states.  There are no restrict-based
 * assumptions either way; only memmove() handles overlap.
 * ------------------------------------------------------------------------- */

void* memset(void* s, int c, size_t n) {
    /* Word-optimized (8 bytes/store, 32-byte unrolled stride); no libc calls or
     * __builtin_memset, so no recursion risk and no bulk-memory dependency. */
    uint8_t* d = (uint8_t*)s;
    const uint8_t v8 = (uint8_t)c;
    uint64_t v64 = v8;
    v64 |= v64 << 8;
    v64 |= v64 << 16;
    v64 |= v64 << 32;
    while (n && ((uintptr_t)d & 7u)) {
        *d++ = v8;
        n--;
    }
    uint64_t* q = (uint64_t*)d;
    while (n >= 32) {
        q[0] = v64;
        q[1] = v64;
        q[2] = v64;
        q[3] = v64;
        q += 4;
        n -= 32;
    }
    while (n >= 8) {
        *q++ = v64;
        n -= 8;
    }
    d = (uint8_t*)q;
    while (n--) {
        *d++ = v8;
    }
    return s;
}

/* Byte-at-a-time forward copy, returning `dst`.  Overlapping regions are not
 * supported; use memmove() for those. */
void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

/* Copy that tolerates overlap, choosing the direction from the pointer order,
 * and returns `dst`. */
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

/* Compare n bytes.  Returns 0 when equal, otherwise the difference of the first
 * differing bytes compared as unsigned char — the sign is meaningful, the
 * magnitude is not. */
int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* a = (const uint8_t*)s1;
    const uint8_t* b = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

/* Address of the first byte equal to (unsigned char)c within the first n bytes,
 * or NULL when it does not occur.  All n bytes are read regardless of any NUL
 * among them. */
void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c)
            return (void*)(p + i);
    }
    return 0;
}

/* Bytes before the terminating NUL, which must exist — there is no bound. */
size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Copy `src` including its NUL and return `dst`.  Unbounded: `dst` must already
 * be large enough. */
char* strcpy(char* dst, const char* src) {
    char* r = dst;
    while ((*dst++ = *src++)) {
    }
    return r;
}

/* Copy at most n bytes and pad the remainder of `dst` with NULs, returning
 * `dst`.  Follows the standard's trap: a `src` of n bytes or longer leaves
 * `dst` without a terminator. */
char* strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

/* Compare two NUL-terminated strings; 0 when equal, otherwise the signed
 * difference of the first differing bytes taken as unsigned char. */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/* strcmp() restricted to the first n bytes, stopping early at a shared NUL. */
int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i])
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (!s1[i])
            return 0;
    }
    return 0;
}

/* Append `src` (with its NUL) after the existing contents of `dst` and return
 * `dst`.  Unbounded: `dst` must have room for both strings. */
char* strcat(char* dst, const char* src) {
    char* r = dst;
    while (*dst)
        dst++;
    while ((*dst++ = *src++)) {
    }
    return r;
}

/* Append at most n bytes of `src`, always NUL-terminating, and return `dst`.
 * `n` bounds the appended bytes only, so `dst` needs room for its current
 * contents plus n + 1 bytes. */
char* strncat(char* dst, const char* src, size_t n) {
    char* r = dst;
    while (*dst)
        dst++;
    for (size_t i = 0; i < n && src[i]; i++)
        *dst++ = src[i];
    *dst = 0;
    return r;
}

/* First occurrence of (unsigned char)c in `s`, or NULL.  Searching for 0
 * returns the terminator's address, as the standard requires. */
char* strchr(const char* s, int c) {
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c)
            return (char*)s;
    if (!c)
        return (char*)s;
    return 0;
}

/* Last occurrence of (unsigned char)c in `s`, or NULL.  Searching for 0 returns
 * the terminator's address. */
char* strrchr(const char* s, int c) {
    const char* last = 0;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c)
            last = s;
    if (!c)
        return (char*)s;
    return (char*)last;
}

/* First occurrence of `needle` in `haystack`, or NULL.  An empty needle matches
 * at the start.  Naive O(n*m) scan — adequate for the short strings this shim
 * sees, not for bulk text. */
char* strstr(const char* haystack, const char* needle) {
    if (!*needle)
        return (char*)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char*)haystack;
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
 *
 * Every `void* ctx` here is a pointer from libui_zig_alloc_ctx() and is
 * dereferenced without a NULL check unless noted; the Zig Context type only
 * constructs one after a successful init, which is what upholds that.
 * ------------------------------------------------------------------------- */

/* Allocate a zeroed, uninitialised ui_context_t out of the arena.  Returns NULL
 * when the arena is exhausted.  The block is never freed — libui_zig_ui_destroy
 * releases the context's resources but the allocation itself is permanent. */
void* libui_zig_alloc_ctx(void) {
    return malloc(sizeof(ui_context_t));
}

/* Initialise the context as a `w` x `h` window.  Returns 0 on success and -1 on
 * failure, in which case the context has already been destroyed and zeroed. */
int32_t libui_zig_ui_init(void* ctx, int32_t proc_ep, int32_t reply_ep, int32_t w, int32_t h) {
    return ui_init((ui_context_t*)ctx, proc_ep, reply_ep, w, h);
}

/* Destroy the window and free everything the context owns, leaving it zeroed.
 * The ui_context_t allocation itself stays in the arena. */
void libui_zig_ui_destroy(void* ctx) {
    ui_destroy((ui_context_t*)ctx);
}

/* Set the window title, discarding the result — a title that is empty or longer
 * than 47 bytes is refused, and the caller cannot tell. */
void libui_zig_set_title(void* ctx, const char* title) {
    (void)ui_window_set_title((ui_context_t*)ctx, title);
}

/* Non-zero once the compositor has asked this window to close.  The flag is set
 * by event dispatch, so it only changes across a poll/drain call. */
int32_t libui_zig_close_requested(const void* ctx) {
    return ((const ui_context_t*)ctx)->close_requested;
}

/* Request a repaint on the next drain. */
void libui_zig_mark_dirty(void* ctx) {
    ui_mark_dirty((ui_context_t*)ctx);
}

/* Lay out, render and present if the tree is dirty.  Returns 0 on success or
 * when there was nothing to do, -1 when a step failed. */
int32_t libui_zig_drain(void* ctx) {
    return ui_loop_drain((ui_context_t*)ctx);
}

/* Block for one pushed GFX event, then layout+render if dirty. The blocking wait
 * is what lets an idle UI sleep instead of polling the compositor in a tight
 * loop.
 *
 * Exactly one event is consumed per call, so a burst is drained one loop
 * iteration at a time. Once close has been requested the wait is skipped and
 * only the drain runs, which keeps a shutdown loop from parking forever. Both
 * the event's own result and the drain's are discarded. */
void libui_zig_poll_and_drain(void* ctx) {
    ui_context_t* c = (ui_context_t*)ctx;
    if (!c->close_requested) {
        (void)ui_wait_and_handle(c);
    }
    ui_loop_drain(c);
}

/* Component id of the tree root, created by init: a PANEL with vertical
 * layout, sized to the window. */
int32_t libui_zig_root_id(const void* ctx) {
    return ((const ui_context_t*)ctx)->root_id;
}

/* Component creation.  Each returns the new component's id or -1 on failure,
 * and leaves it detached until libui_zig_append_child() links it in. */
int32_t libui_zig_create_panel(void* ctx) {
    return ui_component_create_panel((ui_context_t*)ctx);
}

int32_t libui_zig_create_label(void* ctx) {
    return ui_component_create_label((ui_context_t*)ctx);
}

int32_t libui_zig_create_button(void* ctx) {
    return ui_component_create_button((ui_context_t*)ctx);
}

/* A MENU_BAR outside a menu bar window is simply the toolkit's horizontal
 * container: children are placed left to right and their preferred_h is read as
 * a preferred WIDTH.  libui.zig exposes it under the name createRow. */
int32_t libui_zig_create_menu_bar(void* ctx) {
    return ui_component_create_menu_bar((ui_context_t*)ctx);
}

/* Component tree.  Appends `child_id` as the last child of `parent_id`,
 * discarding the result — an unknown id or a self-append leaves the tree
 * unchanged and is not reported. */
void libui_zig_append_child(void* ctx, int32_t parent_id, int32_t child_id) {
    (void)ui_component_append_child((ui_context_t*)ctx, parent_id, child_id);
}

/* Text.  Copies `text` into the component (LABEL, BUTTON, TEXT_INPUT, CHECKBOX,
 * DROPDOWN or MENU_ITEM); the caller keeps ownership of the string.  Does not
 * mark the context dirty, so a live update needs libui_zig_mark_dirty(). */
void libui_zig_set_text(void* ctx, int32_t id, const char* text) {
    ui_component_set_text((ui_context_t*)ctx, id, text);
}

/* Button action callback.  The signature matches ui_button_click_cb_t because
 * ui_context_t* and void* have identical representation in wasm32. */
typedef void (*libui_zig_cb_t)(void* ctx, int32_t id, void* user);

/* Install the click callback and mark the component clickable.  `user` is
 * stored as-is and handed back to the callback; it is neither copied nor freed,
 * so it must outlive the context.  The callback receives the context pointer as
 * its first argument. */
void libui_zig_set_button_action(void* ctx, int32_t id, libui_zig_cb_t cb, void* user) {
    ui_component_set_button_action((ui_context_t*)ctx, id, (ui_button_click_cb_t)cb, user);
}

/* Component property setters.  Each resolves `id` and does nothing when it is
 * unknown, and none of them marks the context dirty — call
 * libui_zig_mark_dirty() after changing a live component.  Colours are
 * 0xAARRGGBB. */
static ui_component_t* comp(void* ctx, int32_t id) {
    return ui_component_by_id((ui_context_t*)ctx, id);
}

void libui_zig_set_bg_color(void* ctx, int32_t id, uint32_t color) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->bg_color = color;
}

void libui_zig_set_fg_color(void* ctx, int32_t id, uint32_t color) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->fg_color = color;
}

void libui_zig_set_border_color(void* ctx, int32_t id, uint32_t color) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->border_color = color;
}

/* Preferred size along the parent's stacking axis: a height under a PANEL, a
 * WIDTH under a ROW or MENU_BAR.  Values of 8 or less are floored at 8 px by
 * the vertical layouts; a MENU_BAR reads 0 as "hidden". */
void libui_zig_set_preferred_h(void* ctx, int32_t id, int32_t h) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->preferred_h = h;
}

void libui_zig_set_padding_px(void* ctx, int32_t id, int32_t px) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->padding_px = px;
}

void libui_zig_set_gap_px(void* ctx, int32_t id, int32_t px) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->gap_px = px;
}

void libui_zig_set_border_px(void* ctx, int32_t id, int32_t px) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->border_px = px;
}

/* Non-zero makes the component answer click hit-tests.  Setting it without a
 * callback still consumes the click (drawing the pressed state) instead of
 * letting it fall through to whatever is behind. */
void libui_zig_set_clickable(void* ctx, int32_t id, int32_t val) {
    ui_component_t* c = comp(ctx, id);
    if (c)
        c->clickable = val;
}
