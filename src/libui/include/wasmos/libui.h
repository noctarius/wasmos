#ifndef WASMOS_LIBUI_H
#define WASMOS_LIBUI_H

/* libui — retained-mode widget toolkit for WASMOS guest apps.
 *
 * The whole toolkit is header-only static-inline C: including this header
 * compiles the implementation into the including translation unit, so there is
 * no libui object to link against. Component headers are included in the middle
 * of this file (after the core helpers they call, before the dispatchers that
 * call them); include order there is load-bearing.
 *
 * Component model: every widget is a ui_component_t, a generic base whose
 * component_data points at a per-kind struct chosen by ui_component_alloc().
 * That pointer is type-punned, so an accessor is only valid for the kinds whose
 * data actually carries the field it reads — each function below states which
 * kinds it accepts. Components are addressed by int32_t id, never by pointer
 * across calls: ui_components_reserve() reallocates the component array, which
 * invalidates every ui_component_t* previously returned by ui_component_by_id().
 *
 * Threading: a ui_context_t is owned by one process/thread. Nothing here takes
 * a lock; concurrent use from two threads is not supported.
 */
#include "wasmos_cast.h"

/* Compile-time trace switch, defaulting to the build-wide WASMOS_TRACE (the
 * -DWASMOS_TRACE cmake flag). Define UI_TRACE=1 before including this header to
 * force libui tracing on without turning it on tree-wide. */
#ifndef UI_TRACE
#define UI_TRACE WASMOS_TRACE
#endif

/* Write a string literal to the console when tracing is on, and compile to
 * nothing when it is off. `msg` must be a literal or char array — the length is
 * taken as sizeof(msg) - 1, so a `const char*` would measure the pointer. */
#if UI_TRACE
#define UI_DBG(msg)                                                                                \
    ((void)wasmos_console_write(addr_cast(int32_t, (msg)), (int32_t)(sizeof(msg) - 1)))
#else
#define UI_DBG(msg) ((void)0)
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "wasmos/api.h"
#include "wasmos/font_ipc.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The allocator libui builds on. Redeclared here because a freestanding guest
 * (the Zig binding's libui_shim.c, for one) supplies its own pair rather than a
 * full <stdlib.h>. Every libui allocation returns NULL-checked memory and libui
 * propagates the failure as -1 rather than aborting. */
void* malloc(size_t size);
void free(void* ptr);

/* UI_PAGE_SIZE      shmem granularity: every shared mapping libui creates is
 *                   rounded up to a multiple of this, matching the kernel's
 *                   4 KiB page.
 * UI_REQ_BASE       first value of ctx->req_id. Request ids only have to be
 *                   unique within one endpoint's in-flight window; a distinct
 *                   base per subsystem keeps libui's traffic recognisable in
 *                   traces next to the app's own requests.
 * *_INITIAL_CAP     first allocation size of the three growable arrays
 *                   (component pool, component text, list items). Each doubles
 *                   on overflow, so these only set the smallest allocation. */
#define UI_PAGE_SIZE 4096
#define UI_REQ_BASE 0x7400
#define UI_COMPONENTS_INITIAL_CAP 16
#define UI_TEXT_INITIAL_CAP 32
#define UI_LIST_INITIAL_CAP 8

/* Result of ui_loop_handle_ipc() / ui_wait_and_handle(): ERROR for a null
 * argument, IGNORED for a message libui does not own (the app may handle it),
 * CONSUMED when libui processed it and the app must not. */
enum { UI_MSG_ERROR = -1, UI_MSG_IGNORED = 0, UI_MSG_CONSUMED = 1 };

/* Widget kind. Selects which struct ui_component_alloc() puts in
 * component_data, and which ui_component_ops entry the dispatchers use.
 * NONE (0) is the value of a zeroed component and has no ops of its own; PANEL
 * carries no per-instance data at all. */
typedef enum {
    UI_COMPONENT_NONE = 0,
    UI_COMPONENT_PANEL = 1,
    UI_COMPONENT_ROW = 2,
    UI_COMPONENT_LABEL = 3,
    UI_COMPONENT_BUTTON = 4,
    UI_COMPONENT_CHECKBOX = 5,
    UI_COMPONENT_TEXT_INPUT = 6,
    UI_COMPONENT_SCROLL_VIEW = 7,
    UI_COMPONENT_LIST_VIEW = 8,
    UI_COMPONENT_TREE_VIEW = 9,
    UI_COMPONENT_DROPDOWN = 10,
    UI_COMPONENT_MENU_BAR = 11,
    UI_COMPONENT_MENU_ITEM = 12
} ui_component_type_t;

/* Axis-aligned rectangle in pixels. Coordinates are relative to the window the
 * component belongs to (the popup windows in libui_menu_item.h are their own
 * coordinate space). A rect with w <= 0 or h <= 0 is empty and every drawing
 * helper skips it. */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} ui_rect_t;

struct ui_context;
/* Click/activation callback. `component_id` is the component that fired (for a
 * menu popup row that is the child row, not the item that owns the popup), and
 * `user` is the pointer registered alongside the callback — libui never
 * dereferences or frees it. Callbacks run inside event dispatch, so they may
 * mutate the component tree, but any ui_component_t* held across a call that
 * creates a component is invalidated. */
typedef void (*ui_button_click_cb_t)(struct ui_context* ctx, int32_t component_id, void* user);
/* List/tree row callback. `item_index` is the zero-based row in the component's
 * ui_list_data_t, already stored as the selection before the call. */
typedef void (*ui_list_view_item_cb_t)(struct ui_context* ctx, int32_t component_id,
                                       int32_t item_index, void* user);

/* Pure base component. All type-specific state lives in component_data.
 * This keeps the core struct small and stable across added widget kinds. */
typedef struct {
    int32_t in_use;         /* 0 marks a retired slot the walkers skip */
    int32_t id;             /* >= 1, unique within the context, never reused */
    int32_t parent_id;      /* 0 for the root */
    int32_t first_child_id; /* 0 when childless; siblings chain via next_sibling_id */
    int32_t next_sibling_id;
    ui_component_type_t type;
    ui_rect_t bounds; /* window coordinates, rewritten by the layout pass */
    /* Preferred height in pixels, used by the vertical layouts. MENU_BAR
     * reinterprets a child's preferred_h as its preferred WIDTH, and treats 0
     * as "hidden" (the item is skipped and keeps its stale bounds). */
    int32_t preferred_h;
    uint32_t bg_color; /* 0xAARRGGBB; the alpha byte is stored, not blended */
    uint32_t fg_color;
    uint32_t border_color;
    int32_t border_px;  /* generic border width; <= 0 draws no border */
    int32_t padding_px; /* inset applied on all four sides by layout and render */
    int32_t gap_px;     /* spacing a container leaves between children */
    int32_t clickable;  /* non-zero makes the component answer hit-tests for clicks */
    int32_t pressed;    /* 1 between button-down and button-up over this component */

    ui_button_click_cb_t on_click;
    void* on_click_user;

    void* component_data; /* owned per-type data (see libui_*.h for the concrete structs) */
} ui_component_t;

/* Vtable for component behavior. Components register their implementations
 * so the core can dispatch without giant type switches.
 * Use struct tags to avoid typedef ordering issues (declared before full ui_context_t). */
typedef struct {
    /* Paint the component. `draw_bounds` is c->bounds already shifted by the
     * enclosing scroll offset; `clip` is the rectangle outside which nothing may
     * be drawn; `offset_y` is that same scroll offset, to be passed on when the
     * implementation renders children itself. The background fill has already
     * happened; the generic border and child descent happen afterwards unless
     * the kind is one of the self-contained painters listed in
     * ui_render_component_clip(). */
    void (*render)(struct ui_context* ctx, const ui_component_t* c, ui_rect_t draw_bounds,
                   ui_rect_t clip, int32_t offset_y);
    /* Assign bounds to this component's children. When set, it fully replaces
     * the core's generic vertical layout for that kind and is responsible for
     * recursing into grandchildren. c->bounds is already final on entry. */
    void (*layout)(struct ui_context* ctx, ui_component_t* c);

    /* Pointer went down at (x, y) in window coordinates, on a component this
     * kind's finder selected (list/tree: ui_find_list_view_at). */
    void (*handle_pointer_press)(struct ui_context* ctx, ui_component_t* c, int32_t x, int32_t y);
    /* Pointer came up over a component that was pressed. When set it replaces
     * the core's plain on_click invocation, so an implementation that wants the
     * application callback must call c->on_click itself. */
    void (*handle_pointer_release)(struct ui_context* ctx, ui_component_t* c);
    /* `key` is the packed GFX_EVENT_KEY code: decode it with ui_key_char() /
     * ui_key_scancode(), never compare it against a character directly. */
    void (*handle_key)(struct ui_context* ctx, ui_component_t* c, uint32_t key);
    /* Pointer dragged `dy` pixels vertically while this component was the
     * active scroll target. `dy` is thumb travel, not content travel — convert
     * with ui_scroll_drag_delta(). */
    void (*handle_scroll_drag)(struct ui_context* ctx, ui_component_t* c, int32_t dy);

    /* Optional: for hit-testing popups/overlays owned by the component.
     * (x, y) are in the coordinates of the window the core is walking, so a
     * component whose popup lives in a separate compositor window must answer
     * false — see ui_menu_item_popup_contains(). */
    bool (*popup_contains)(const struct ui_context* ctx, const ui_component_t* c, int32_t x,
                           int32_t y);

    /* Free the component_data (and anything it owns). Called once per component
     * from ui_destroy(); when unset, ui_destroy() free()s component_data
     * directly, which is only correct for data that owns no further pointers. */
    void (*destroy_data)(ui_component_t* c);
} ui_component_ops_t;

/* Indexed by ui_component_type_t; filled by ui_init_component_ops(), which is
 * defined once the component headers below have been included. */
static ui_component_ops_t ui_component_ops[UI_COMPONENT_MENU_ITEM + 1];

/* Prototype so it can be called from ui_init / ui_menu_bar_init (defined after the component
 * includes). */
static inline void ui_init_component_ops(void);

/* Small reusable data blocks for common component aspects.
 * Individual component headers may use these directly as their component_data
 * or define richer per-type structs that contain them. */
/* Owned NUL-terminated string. text_len excludes the terminator; text_cap is
 * the malloc'd size including it. text is NULL until the first set, and the
 * text-bearing kinds all place this struct at offset 0 of their data so the
 * generic accessors can reach it: LABEL, BUTTON, TEXT_INPUT, CHECKBOX,
 * DROPDOWN, MENU_ITEM. */
typedef struct {
    char* text;
    int32_t text_len;
    int32_t text_cap;
} ui_text_data_t;

/* Owned array of NUL-terminated strings. Each entry is an individually
 * malloc'd copy of the string the caller appended. `selected` is -1 for "no
 * selection" as allocated, but the layout passes clamp it into [0, count) as
 * soon as the component is laid out with a non-empty list. */
typedef struct {
    char** items;
    int32_t count;
    int32_t capacity;
    int32_t selected;
} ui_list_data_t;

/* Per-type component data structs. ui_component_alloc() allocates the correct
 * struct eagerly so all code can cast component_data without size mismatches. */
typedef struct {
    ui_text_data_t text;
    int32_t checked; /* 0 or 1; toggled by ui_checkbox_toggle() before on_click */
} ui_checkbox_data_t;

/* scroll_y is the content offset in pixels, always clamped to
 * [0, scroll_max]; scroll_max is recomputed each layout as
 * max(0, content_height - viewport_height) and is 0 when everything fits. */
typedef struct {
    int32_t scroll_y;
    int32_t scroll_max;
} ui_scroll_view_data_t;

/* Rows are a flat list of 20 px items. on_activate fires on a left
 * double-click, on_secondary_click on a right click; both are optional and
 * receive the row index. */
typedef struct {
    ui_list_data_t list;
    int32_t scroll_y;
    int32_t scroll_max;
    ui_list_view_item_cb_t on_activate;
    void* on_activate_user;
    ui_list_view_item_cb_t on_secondary_click;
    void* on_secondary_click_user;
} ui_list_view_data_t;

/* Like ui_list_view_data_t plus one indentation level per row. `depths` is a
 * parallel array grown to list.capacity by ui_component_tree_append(); it stays
 * NULL while no row has been appended. Nesting is presentational only — rows
 * are never collapsed or filtered by depth. */
typedef struct {
    ui_list_data_t list;
    int32_t* depths;
    int32_t depth_capacity;
    int32_t scroll_y;
    int32_t scroll_max;
    ui_list_view_item_cb_t on_activate;
    void* on_activate_user;
    ui_list_view_item_cb_t on_secondary_click;
    void* on_secondary_click_user;
} ui_tree_view_data_t;

/* `text` is the placeholder shown while no item is selected. The popup is drawn
 * into the owning window's framebuffer (unlike a menu item's), so dropdown_open
 * only affects painting and hit-testing inside this window. */
typedef struct {
    ui_text_data_t text;
    ui_list_data_t list;
    int32_t dropdown_open;
} ui_dropdown_data_t;

typedef struct {
    ui_text_data_t text;
    /* Menu entries are children in the component tree (first_child_id /
     * next_sibling_id), not a flat list: a leaf carries an on_click callback,
     * a non-leaf opens its own sub-popup on hover and takes focus on click. */
    int32_t hovered_child_id; /* component id of the child currently highlighted, or 0 */
    /* Requested popup state. Setting it does not open anything: the window is
     * reconciled on the next ui_loop_drain() by ui_menu_item_sync_popup(). */
    int32_t dropdown_open;
    /* popup window — managed by ui_menu_item_sync_popup (libui_menu_item.h) */
    int32_t popup_win_id;   /* 0 when no popup window exists */
    int32_t popup_buf_id;   /* compositor shared-buffer id backing popup_base */
    int32_t popup_shmem_id; /* shmem id of that buffer, mapped at popup_base */
    uint8_t* popup_base;    /* BGRA32 pixels, popup_w * popup_h * 4 bytes */
    int32_t popup_w;
    int32_t popup_h;
    /* popup_hovered is the row index under the pointer, -1 for none;
     * popup_prev_buttons is the previous pointer button mask, kept so the popup
     * handler can detect press and release edges. */
    int32_t popup_hovered;
    uint32_t popup_prev_buttons;
    int32_t popup_flushing;  /* 1 while discarding pre-open stale button-down events */
    int32_t popup_has_focus; /* 1 when compositor focus is currently on this popup window */
} ui_menu_item_data_t;

/* Right-aligned status text painted by the menu bar. Purely presentational:
 * libui never reads a clock itself, the application pushes the string with
 * ui_menu_bar_set_clock(). */
typedef struct {
    char clock_text[24]; /* "YYYY-MM-DD HH:MM:SS\0" or empty */
} ui_menu_bar_data_t;

/* One libui window plus its component tree. Zero-initialised and populated by
 * ui_init() or ui_menu_bar_init(); torn down by ui_destroy(), which also zeroes
 * it, so a context may be re-initialised afterwards. Applications read fields
 * such as close_requested, root_id, event_endpoint and req_id directly; the
 * gfx/font/buffer members are libui's own bookkeeping. */
typedef struct ui_context {
    int32_t proc_endpoint;  /* process-manager endpoint, used for service lookups */
    int32_t reply_endpoint; /* endpoint synchronous request replies come back on */
    /* Dedicated endpoint the compositor pushes GFX_IPC_PUSH_EVENT to. It is the
     * window's owner endpoint (source of CREATE_WINDOW); the loop blocks on it.
     * Synchronous requests use reply_endpoint so their replies never mix with
     * pushed events (compositor ownership is per-process, not per-endpoint). */
    int32_t event_endpoint;
    int32_t gfx_endpoint; /* compositor service endpoint */
    int32_t req_id;       /* monotonically increasing request id, seeded at UI_REQ_BASE */
    int32_t window_id;    /* compositor window id, 0 before ui_init succeeds */
    int32_t width;        /* content size in pixels; also the framebuffer stride in pixels */
    int32_t height;
    int32_t stride_bytes; /* bytes per row as reported by the compositor */
    int32_t buffer_id;    /* compositor shared-buffer id currently presented */
    int32_t shmem_id;     /* shmem id of that buffer */
    uint8_t* mapped_base; /* BGRA32 pixels; invalidated by every ui_realloc_buffer() */
    int32_t pointer_x;    /* last pointer position, clamped into the window */
    int32_t pointer_y;
    uint32_t pointer_buttons;     /* button mask from the last pointer event; bit 0 is left */
    uint32_t pointer_drag_button; /* button held during an in-progress drag, 0 when none */
    int32_t pointer_drag_x;       /* position of the previous drag sample */
    int32_t pointer_drag_y;
    int32_t dirty;                /* non-zero asks the next ui_loop_drain() to repaint */
    int32_t close_requested;      /* set once the compositor asks this window to close */
    int32_t root_id;              /* component id of the tree root (PANEL, or MENU_BAR for a bar) */
    int32_t focused_component_id; /* keyboard target; TEXT_INPUT / DROPDOWN only, 0 for none */
    int32_t active_scroll_component_id; /* scroll target for the current drag, 0 when none */
    int32_t font_reply_endpoint;        /* reply endpoint for font-service requests */
    int32_t font_endpoint;
    int32_t font_handle; /* open font handle; > 0 once ui_init_font() succeeded */
    int32_t font_px;     /* requested pixel size, also used as the text line height */
    /* Scratch shmem regions shared with the font service: the UTF-8 string to
     * measure/raster, and the 8-bit coverage mask it writes back. Both grow on
     * demand via ui_font_ensure_shmem_buffer(). */
    int32_t font_text_shmem_id;
    uint8_t* font_text_ptr;
    int32_t font_text_cap;
    int32_t font_mask_shmem_id;
    uint8_t* font_mask_ptr;
    int32_t font_mask_cap;

    int32_t next_component_id;  /* id handed to the next allocated component */
    ui_component_t* components; /* pool; reallocation invalidates every element pointer */
    int32_t component_count;    /* slots in use, including retired (in_use == 0) ones */
    int32_t component_capacity;
} ui_context_t;

/* Field extractors for the packed 32-bit payloads the compositor sends.
 * u16_lo/u16_hi split a pair of unsigned halves (RESIZE carries width, height);
 * i16_lo/i16_hi do the same sign-extended (font metrics carry bearings). */
static inline int32_t ui_u16_lo(int32_t packed) {
    return (packed & 0xFFFF);
}
static inline int32_t ui_u16_hi(int32_t packed) {
    return ((packed >> 16) & 0xFFFF);
}
static inline int32_t ui_i16_lo(int32_t packed) {
    return (int16_t)(packed & 0xFFFF);
}
static inline int32_t ui_i16_hi(int32_t packed) {
    return (int16_t)((packed >> 16) & 0xFFFF);
}
/* GFX_EVENT_POINTER payload: 12-bit x, 12-bit y, then an 8-bit button mask.
 * Coordinates are relative to the window the event names, so a popup window's
 * events are already popup-local. */
static inline int32_t ui_ptr_evt_x(int32_t packed) {
    return (packed & 0xFFF);
}
static inline int32_t ui_ptr_evt_y(int32_t packed) {
    return ((packed >> 12) & 0xFFF);
}
static inline uint32_t ui_ptr_evt_buttons(int32_t packed) {
    return (uint32_t)((packed >> 24) & 0xFF);
}
/* GFX_EVENT_POINTER_GESTURE payload: 12-bit x, 12-bit y, a 4-bit button
 * (GFX_POINTER_BUTTON_*), then a 4-bit gesture kind (GFX_POINTER_GESTURE_*). */
static inline int32_t ui_ptr_gesture_x(int32_t packed) {
    return (packed & 0xFFF);
}
static inline int32_t ui_ptr_gesture_y(int32_t packed) {
    return ((packed >> 12) & 0xFFF);
}
static inline uint32_t ui_ptr_gesture_button(int32_t packed) {
    return (uint32_t)((packed >> 24) & 0xF);
}
static inline uint32_t ui_ptr_gesture_kind(int32_t packed) {
    return (uint32_t)((packed >> 28) & 0xF);
}

/* Request a repaint on the next ui_loop_drain(). Tolerates a NULL context.
 * Nothing is drawn here — the flag is only consumed by the drain. */
static inline void ui_mark_dirty(ui_context_t* ctx) {
    if (ctx)
        ctx->dirty = 1;
}

/* Convert `dy` pixels of scrollbar-thumb travel into content pixels, given the
 * viewport height and the scrollable range. Returns 0 when nothing can scroll
 * (dy == 0, viewport_h <= 8, or scroll_max <= 0), and never returns 0 for a
 * non-zero dy that could scroll: a delta that rounds to zero is rounded away
 * from zero to +/-1 so slow drags still move. The caller must clamp the
 * resulting scroll offset into [0, scroll_max]. */
static inline int32_t ui_scroll_drag_delta(int32_t dy, int32_t viewport_h, int32_t scroll_max) {
    if (dy == 0 || viewport_h <= 8 || scroll_max <= 0)
        return 0;
    int32_t thumb_h = (viewport_h * viewport_h) / (viewport_h + scroll_max);
    if (thumb_h < 12)
        thumb_h = 12;
    if (thumb_h > viewport_h - 12)
        thumb_h = viewport_h - 12;
    if (thumb_h < 8)
        thumb_h = 8;
    if (thumb_h > viewport_h)
        thumb_h = viewport_h;
    const int32_t travel = viewport_h - thumb_h;
    if (travel <= 0)
        return dy;
    int32_t delta = (dy * scroll_max) / travel;
    if (delta == 0)
        delta = (dy > 0) ? 1 : -1;
    return delta;
}

/* Fill a rectangle in an ARGB32 framebuffer. `base` is the pixel origin, `bw` /
 * `bh` its size in pixels, and rows are assumed packed at bw * 4 bytes — a
 * padded stride is not supported. The rectangle is clipped to the framebuffer;
 * a NULL base, a non-positive size or a fully off-screen rectangle draws
 * nothing. `color` is written verbatim, with no blending. */
static inline void ui_fill_rect(uint8_t* base, int32_t bw, int32_t bh, int32_t x, int32_t y,
                                int32_t w, int32_t h, uint32_t color) {
    if (!base || bw <= 0 || bh <= 0 || w <= 0 || h <= 0)
        return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > bw)
        x1 = bw;
    if (y1 > bh)
        y1 = bh;
    if (x0 >= x1 || y0 >= y1)
        return;
    const int32_t stride_px = bw;
    for (int32_t yy = y0; yy < y1; ++yy) {
        uint32_t* row = (uint32_t*)(void*)(base + (yy * stride_px * 4));
        for (int32_t xx = x0; xx < x1; ++xx) {
            row[xx] = color;
        }
    }
}

/* Intersection of two rectangles. A disjoint pair yields w and h clamped to 0
 * (x and y are then meaningless), which every drawing helper treats as empty. */
static inline ui_rect_t ui_rect_intersect(ui_rect_t a, ui_rect_t b) {
    ui_rect_t r;
    const int32_t x0 = (a.x > b.x) ? a.x : b.x;
    const int32_t y0 = (a.y > b.y) ? a.y : b.y;
    const int32_t x1a = a.x + a.w;
    const int32_t y1a = a.y + a.h;
    const int32_t x1b = b.x + b.w;
    const int32_t y1b = b.y + b.h;
    const int32_t x1 = (x1a < x1b) ? x1a : x1b;
    const int32_t y1 = (y1a < y1b) ? y1a : y1b;
    r.x = x0;
    r.y = y0;
    r.w = x1 - x0;
    r.h = y1 - y0;
    if (r.w < 0)
        r.w = 0;
    if (r.h < 0)
        r.h = 0;
    return r;
}

/* ui_fill_rect() restricted to `clip` as well as to the framebuffer. This is
 * the form the component renderers use, since every render callback receives
 * the clip rectangle it must respect. */
static inline void ui_fill_rect_clip(uint8_t* base, int32_t bw, int32_t bh, int32_t x, int32_t y,
                                     int32_t w, int32_t h, uint32_t color, ui_rect_t clip) {
    ui_rect_t r = {x, y, w, h};
    ui_rect_t i = ui_rect_intersect(r, clip);
    if (i.w <= 0 || i.h <= 0)
        return;
    ui_fill_rect(base, bw, bh, i.x, i.y, i.w, i.h, color);
}

/* Draw a `border_px`-wide border inset inside `r` (the four edges are painted
 * within the rectangle, not around it). A border_px <= 0 draws nothing; a
 * border_px larger than half the rectangle overdraws the middle. */
static inline void ui_stroke_rect_clip(uint8_t* base, int32_t bw, int32_t bh, ui_rect_t r,
                                       int32_t border_px, uint32_t color, ui_rect_t clip) {
    if (border_px <= 0)
        return;
    ui_fill_rect_clip(base, bw, bh, r.x, r.y, r.w, border_px, color, clip);
    ui_fill_rect_clip(base, bw, bh, r.x, r.y + r.h - border_px, r.w, border_px, color, clip);
    ui_fill_rect_clip(base, bw, bh, r.x, r.y, border_px, r.h, color, clip);
    ui_fill_rect_clip(base, bw, bh, r.x + r.w - border_px, r.y, border_px, r.h, color, clip);
}

/* Paint a vertical scrollbar into the track rectangle (x, y, w, h). The thumb
 * is sized proportionally to the visible fraction, floored at 12 px (8 px when
 * the track is too short for that), and positioned from scroll_y / scroll_max.
 * Draws nothing when there is nothing to scroll (scroll_max <= 0), when the
 * track is degenerate (w <= 2 or h <= 8), or when base is NULL. */
static inline void ui_draw_v_scrollbar(uint8_t* base, int32_t bw, int32_t bh, int32_t x, int32_t y,
                                       int32_t w, int32_t h, int32_t scroll_y, int32_t scroll_max,
                                       uint32_t track_color, uint32_t thumb_color,
                                       uint32_t thumb_border_color, ui_rect_t clip) {
    if (!base || w <= 2 || h <= 8 || scroll_max <= 0)
        return;
    ui_fill_rect_clip(base, bw, bh, x, y, w, h, track_color, clip);

    int32_t thumb_h = (h * h) / (h + scroll_max);
    if (thumb_h < 12)
        thumb_h = 12;
    if (thumb_h > h - 12)
        thumb_h = h - 12;
    if (thumb_h < 8)
        thumb_h = 8;
    if (thumb_h > h)
        thumb_h = h;

    const int32_t travel = h - thumb_h;
    const int32_t thumb_y = y + ((travel > 0) ? ((travel * scroll_y) / scroll_max) : 0);
    ui_fill_rect_clip(base, bw, bh, x + 1, thumb_y, w - 2, thumb_h, thumb_color, clip);
    ui_stroke_rect_clip(
        base, bw, bh, (ui_rect_t){x + 1, thumb_y, w - 2, thumb_h}, 1, thumb_border_color, clip);
}

/* Source-over blend of `src` onto `dst` at coverage `alpha` (0 = keep dst,
 * 255 = replace with src). Only the RGB bytes of both operands participate;
 * the result is always returned fully opaque (alpha byte 0xFF). */
static inline uint32_t ui_blend_u8(uint32_t dst, uint32_t src, uint8_t alpha) {
    const uint32_t a = (uint32_t)alpha;
    const uint32_t inv = 255u - a;
    const uint32_t dr = (dst >> 16) & 0xFFu;
    const uint32_t dg = (dst >> 8) & 0xFFu;
    const uint32_t db = dst & 0xFFu;
    const uint32_t sr = (src >> 16) & 0xFFu;
    const uint32_t sg = (src >> 8) & 0xFFu;
    const uint32_t sb = src & 0xFFu;
    const uint32_t r = (sr * a + dr * inv) / 255u;
    const uint32_t g = (sg * a + dg * inv) / 255u;
    const uint32_t b = (sb * a + db * inv) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* GFX_EVENT_KEY packs two values into one code: the low byte is the character
 * the vt decoded with the active keymap (0 for keys with no character, such as
 * the arrows and function keys), the high byte is the raw set-1 scancode.
 * Handlers must never use the packed code as a character — 'a' arrives as
 * 0x1E61 (scancode 0x1E, 'a'), which as a codepoint is an unrelated glyph. */
static inline uint32_t ui_key_char(uint32_t key) {
    return key & 0xFFu;
}

static inline uint32_t ui_key_scancode(uint32_t key) {
    return (key >> 8) & 0xFFu;
}

/* Encode one code point into `out` and return the byte count (1..4). Returns 0
 * without writing anything for a NULL out, a surrogate (U+D800..U+DFFF) or a
 * value above U+10FFFF. `out` is not NUL-terminated. */
static inline int32_t ui_utf8_encode(uint32_t cp, uint8_t out[4]) {
    if (!out)
        return 0;
    if (cp <= 0x7Fu) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu)
        return 0;
    if (cp <= 0xFFFFu) {
        out[0] = (uint8_t)(0xE0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (uint8_t)(0xF0u | (cp >> 18));
        out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/* Byte offset where the last code point of s[0..len) starts, i.e. the length
 * that remains after deleting one character. Returns 0 for NULL or len <= 0.
 * Scans back over continuation bytes and stops at index 0, so a malformed
 * sequence truncates to empty rather than running off the front. */
static inline int32_t ui_utf8_prev_boundary(const char* s, int32_t len) {
    if (!s || len <= 0)
        return 0;
    int32_t i = len - 1;
    while (i > 0) {
        const uint8_t b = (uint8_t)s[i];
        if ((b & 0xC0u) != 0x80u)
            break;
        i -= 1;
    }
    return i;
}

/* Make sure *shmem_id names a mapped shmem region of at least `need_bytes`,
 * creating and mapping a new one (rounded up to whole UI_PAGE_SIZE pages) when
 * the current one is absent or too small. On success the three out-parameters
 * describe the region and 0 is returned; on failure they are left untouched and
 * -1 is returned. Growing replaces the region, so any pointer previously read
 * out of *mapped_ptr is stale and the old contents are not carried over. */
static inline int32_t ui_font_ensure_shmem_buffer(int32_t* shmem_id, uint8_t** mapped_ptr,
                                                  int32_t* cap, int32_t need_bytes) {
    if (!shmem_id || !mapped_ptr || !cap || need_bytes <= 0)
        return -1;
    if (*shmem_id > 0 && *mapped_ptr && *cap >= need_bytes)
        return 0;
    const int32_t pages = (need_bytes + (UI_PAGE_SIZE - 1)) / UI_PAGE_SIZE;
    const int32_t bytes = pages * UI_PAGE_SIZE;
    const int32_t new_id = wasmos_shmem_create(pages, 0);
    if (new_id <= 0)
        return -1;
    const int32_t mapped = wasmos_shmem_map_auto(new_id, bytes);
    if (mapped < 0)
        return -1;
    /* TODO(libui-font-shmem): old SHMEM IDs are not reclaimed on growth. */
    *shmem_id = new_id;
    *mapped_ptr = ptr_cast(uint8_t, (uint32_t)mapped);
    *cap = bytes;
    return 0;
}

/* Ask the font service for the metrics of `text` at the context's font size.
 * Returns 0 on success and -1 on failure (no font opened, allocation or IPC
 * failure, or an error reply). All five out-parameters are optional.
 *
 * out_w / out_h  size of the rendered bitmap in pixels
 * out_x0 / out_y0 signed bearing of that bitmap relative to the pen position
 * out_adv        pen advance, which is the value to use for text width
 *
 * An empty string is a success that zeroes every out-parameter without any IPC.
 * Blocks on the font service for one request/reply round trip. */
static inline int32_t ui_font_measure_text(ui_context_t* ctx, const char* text, int32_t* out_w,
                                           int32_t* out_h, int32_t* out_x0, int32_t* out_y0,
                                           int32_t* out_adv) {
    if (!ctx || !text || ctx->font_endpoint <= 0 || ctx->font_reply_endpoint <= 0 ||
        ctx->font_handle <= 0)
        return -1;
    const int32_t text_len = (int32_t)strlen(text);
    if (text_len <= 0) {
        if (out_w)
            *out_w = 0;
        if (out_h)
            *out_h = 0;
        if (out_x0)
            *out_x0 = 0;
        if (out_y0)
            *out_y0 = 0;
        if (out_adv)
            *out_adv = 0;
        return 0;
    }
    if (ui_font_ensure_shmem_buffer(
            &ctx->font_text_shmem_id, &ctx->font_text_ptr, &ctx->font_text_cap, text_len + 1) != 0)
        return -1;
    memcpy(ctx->font_text_ptr, text, (size_t)text_len);
    ctx->font_text_ptr[text_len] = '\0';
    if (wasmos_shmem_flush(
            ctx->font_text_shmem_id, addr_cast(int32_t, ctx->font_text_ptr), text_len + 1) != 0)
        return -1;

    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(ctx->font_endpoint,
                        ctx->font_reply_endpoint,
                        FONT_IPC_MEASURE_GLYPH_REQ,
                        ctx->req_id++,
                        ctx->font_handle,
                        ctx->font_text_shmem_id,
                        text_len,
                        0,
                        &reply) != 0) {
        return -1;
    }
    if (reply.type != FONT_IPC_RESP || reply.arg0 != WASMOS_ERR_NONE)
        return -1;
    if (out_w)
        *out_w = ui_u16_lo(reply.arg1);
    if (out_h)
        *out_h = ui_u16_hi(reply.arg1);
    if (out_x0)
        *out_x0 = ui_i16_lo(reply.arg2);
    if (out_y0)
        *out_y0 = ui_i16_hi(reply.arg2);
    if (out_adv)
        *out_adv = reply.arg3;
    return 0;
}

/* Measure `text` and rasterise it into ctx->font_mask_ptr as an 8-bit coverage
 * mask of out_w * out_h bytes, row-major with no padding. Returns 0 on success
 * and -1 on failure; out_w and out_h are mandatory here (a NULL for either is
 * an error), the other three follow ui_font_measure_text().
 *
 * A string that measures to an empty bitmap returns 0 with no mask written, so
 * callers must check *out_w and *out_h before reading the mask. The mask buffer
 * is context-wide scratch: the next call overwrites it, and growing it moves it.
 * Blocks for two font-service round trips. */
static inline int32_t ui_font_measure_and_raster_text(ui_context_t* ctx, const char* text,
                                                      int32_t text_len, int32_t* out_w,
                                                      int32_t* out_h, int32_t* out_x0,
                                                      int32_t* out_y0, int32_t* out_adv) {
    if (!ctx || !text || text_len <= 0 || ctx->font_endpoint <= 0 ||
        ctx->font_reply_endpoint <= 0 || ctx->font_handle <= 0)
        return -1;
    if (ui_font_measure_text(ctx, text, out_w, out_h, out_x0, out_y0, out_adv) != 0)
        return -1;
    if (!out_w || !out_h)
        return -1;
    if (*out_w <= 0 || *out_h <= 0)
        return 0;

    const int32_t bytes = (*out_w) * (*out_h);
    if (bytes <= 0)
        return -1;
    if (ui_font_ensure_shmem_buffer(
            &ctx->font_mask_shmem_id, &ctx->font_mask_ptr, &ctx->font_mask_cap, bytes) != 0)
        return -1;

    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(ctx->font_endpoint,
                        ctx->font_reply_endpoint,
                        FONT_IPC_RASTER_GLYPH_INTO_REQ,
                        ctx->req_id++,
                        ctx->font_handle,
                        ctx->font_text_shmem_id,
                        text_len,
                        ctx->font_mask_shmem_id,
                        &reply) != 0) {
        return -1;
    }
    if (reply.type != FONT_IPC_RESP || reply.arg0 != WASMOS_ERR_NONE)
        return -1;
    if (wasmos_shmem_refresh(
            ctx->font_mask_shmem_id, addr_cast(int32_t, ctx->font_mask_ptr), bytes) != 0)
        return -1;
    return 0;
}

/* Draw `text` into ctx->mapped_base with its top-left at (x, y), alpha-blended
 * with `color` through the font coverage mask and clipped to both `clip` and the
 * context's width/height. Silently draws nothing when no font is open, when the
 * string is empty, or when rasterisation fails.
 *
 * The destination is always ctx->mapped_base at ctx->width x ctx->height, never
 * a caller-supplied surface — painting into a different framebuffer means
 * temporarily repointing those three fields (ui_menu_item_popup_render() does
 * exactly that). Each call costs two font-service round trips. */
static inline void ui_draw_text_clip(ui_context_t* ctx, int32_t x, int32_t y, const char* text,
                                     uint32_t color, ui_rect_t clip) {
    if (!ctx || !ctx->mapped_base || !text || ctx->font_endpoint <= 0 ||
        ctx->font_reply_endpoint <= 0 || ctx->font_handle <= 0)
        return;
    const int32_t text_len = (int32_t)strlen(text);
    if (text_len <= 0)
        return;
    int32_t w = 0, h = 0, x0 = 0, y0 = 0, adv = 0;
    if (ui_font_measure_and_raster_text(ctx, text, text_len, &w, &h, &x0, &y0, &adv) != 0)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!ctx->font_mask_ptr)
        return;

    const uint8_t* mask = ctx->font_mask_ptr;
    for (int32_t gy = 0; gy < h; ++gy) {
        const int32_t py = y + gy;
        if (py < clip.y || py >= (clip.y + clip.h) || py < 0 || py >= ctx->height)
            continue;
        uint32_t* row =
            (uint32_t*)(void*)(ctx->mapped_base + ((size_t)py * (size_t)ctx->width * 4u));
        for (int32_t gx = 0; gx < w; ++gx) {
            const int32_t px = x + gx;
            if (px < clip.x || px >= (clip.x + clip.w) || px < 0 || px >= ctx->width)
                continue;
            const uint8_t a = mask[gy * w + gx];
            if (a == 0)
                continue;
            row[px] = ui_blend_u8(row[px], color, a);
        }
    }
}

/* Advance width of `text` in pixels, for centring and caret placement. Returns
 * 0 both for an empty string and for any failure (no font, IPC error), so it
 * cannot distinguish the two. */
static inline int32_t ui_measure_text_width(ui_context_t* ctx, const char* text) {
    int32_t w = 0, h = 0, x0 = 0, y0 = 0, adv = 0;
    if (ui_font_measure_text(ctx, text, &w, &h, &x0, &y0, &adv) != 0)
        return 0;
    return adv;
}

/* Send one compositor request and block for its reply on `reply_ep`. Returns 0
 * when a GFX_IPC_RESP or GFX_IPC_ERROR message came back — including an error
 * reply, whose status is in out_raw->arg0 — and -1 when the call itself failed
 * or the reply was some other message type. Callers must therefore check the
 * status separately; `out_raw` is only valid on a 0 return. */
static inline int32_t ui_send_gfx_raw(int32_t gfx_ep, int32_t reply_ep, int32_t req_id,
                                      int32_t opcode, int32_t arg0, int32_t arg1, int32_t arg2,
                                      int32_t arg3, wasmos_ipc_message_t* out_raw) {
    if (!out_raw)
        return -1;
    if (wasmos_ipc_call(gfx_ep, reply_ep, opcode, req_id, arg0, arg1, arg2, arg3, out_raw) != 0) {
        return -1;
    }
    if (out_raw->type != GFX_IPC_RESP && out_raw->type != GFX_IPC_ERROR) {
        return -1;
    }
    return 0;
}

/* ui_send_gfx_raw() with the reply unpacked into optional out-parameters.
 * *out_status receives arg0, which is a packed abi/errors.yaml code
 * (WASMOS_ERR_NONE for success) — a 0 return only means the round trip
 * completed, so both the return value and the status have to be checked. */
static inline int32_t ui_send_gfx(int32_t gfx_ep, int32_t reply_ep, int32_t req_id, int32_t opcode,
                                  int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
                                  int32_t* out_status, int32_t* out_a1, int32_t* out_a2,
                                  int32_t* out_a3) {
    wasmos_ipc_message_t msg;
    if (ui_send_gfx_raw(gfx_ep, reply_ep, req_id, opcode, arg0, arg1, arg2, arg3, &msg) != 0) {
        return -1;
    }
    if (out_status)
        *out_status = msg.arg0;
    if (out_a1)
        *out_a1 = msg.arg1;
    if (out_a2)
        *out_a2 = msg.arg2;
    if (out_a3)
        *out_a3 = msg.arg3;
    return 0;
}

/* Resolve a component id to its slot, or NULL for a NULL context, a
 * non-positive id, or an id that is retired or unknown. The search is a linear
 * scan of the pool.
 *
 * Lifetime: the returned pointer is only valid until the next call that can
 * allocate a component (ui_component_alloc() and everything built on it), since
 * growing the pool moves every element. Hold the id, not the pointer. */
static inline ui_component_t* ui_component_by_id(ui_context_t* ctx, int32_t id) {
    if (!ctx || id <= 0)
        return 0;
    for (int32_t i = 0; i < ctx->component_count; ++i) {
        if (ctx->components[i].in_use && ctx->components[i].id == id)
            return &ctx->components[i];
    }
    return 0;
}

/* Copy `text` into the component's own ui_text_data_t, growing it as needed.
 * Returns 0 on success, -1 for a NULL component, absent component_data or a
 * failed allocation. A NULL `text` is treated as the empty string. The caller
 * keeps ownership of `text`; libui stores a copy. */
static inline int32_t ui_component_set_text_owned(ui_component_t* c, const char* text) {
    if (!c)
        return -1;
    if (!text)
        text = "";
    const int32_t need = (int32_t)strlen(text) + 1;
    if (need <= 0)
        return -1;

    /* Valid only for the text-bearing types, which all place ui_text_data_t at
     * offset 0 of their pre-allocated component_data: LABEL, BUTTON, TEXT_INPUT,
     * CHECKBOX, DROPDOWN, MENU_ITEM.
     * FIXME: no type check happens here, so setting text on a LIST_VIEW,
     * TREE_VIEW, SCROLL_VIEW or MENU_BAR reinterprets that component's data as
     * a string record and corrupts it. */
    ui_text_data_t* td = (ui_text_data_t*)c->component_data;
    if (!td)
        return -1;

    if (td->text_cap < need) {
        int32_t new_cap = td->text_cap > 0 ? td->text_cap : UI_TEXT_INITIAL_CAP;
        while (new_cap < need)
            new_cap *= 2;
        char* new_text = (char*)malloc((size_t)new_cap);
        if (!new_text)
            return -1;
        if (td->text)
            memcpy(new_text, td->text, (size_t)td->text_len + 1);
        if (td->text)
            free(td->text);
        td->text = new_text;
        td->text_cap = new_cap;
    }
    memcpy(td->text, text, (size_t)need);
    td->text_len = need - 1;
    return 0;
}

/* Grow the component pool to hold at least `target` slots, doubling from
 * UI_COMPONENTS_INITIAL_CAP. Returns 0 when the capacity is already sufficient
 * or the growth succeeded, -1 for a NULL context or a failed allocation.
 * A successful growth moves the pool, invalidating every outstanding
 * ui_component_t*. */
static inline int32_t ui_components_reserve(ui_context_t* ctx, int32_t target) {
    if (!ctx || target <= ctx->component_capacity)
        return 0;
    int32_t cap = ctx->component_capacity > 0 ? ctx->component_capacity : UI_COMPONENTS_INITIAL_CAP;
    while (cap < target)
        cap *= 2;
    ui_component_t* new_arr = (ui_component_t*)malloc((size_t)cap * sizeof(ui_component_t));
    if (!new_arr)
        return -1;
    if (ctx->components && ctx->component_count > 0) {
        memcpy(new_arr, ctx->components, (size_t)ctx->component_count * sizeof(ui_component_t));
        free(ctx->components);
    }
    ctx->components = new_arr;
    ctx->component_capacity = cap;
    return 0;
}

/* Allocate a component of `type` and its matching component_data (PANEL and
 * ROW get none), and return its id, or -1 on failure. The component starts
 * detached — it is not part of the tree until ui_component_append_child() links
 * it — and carries the shared defaults: 24 px preferred height, the dark theme
 * colours, 1 px border, 6 px padding and gap. LIST_VIEW, TREE_VIEW and DROPDOWN
 * additionally start with no selection (-1).
 *
 * May grow the pool, so it invalidates outstanding ui_component_t*. A failed
 * component_data allocation returns -1 before component_count is incremented,
 * so the half-filled slot is simply reused by the next allocation; the id it
 * consumed is not. */
static inline int32_t ui_component_alloc(ui_context_t* ctx, ui_component_type_t type) {
    if (!ctx)
        return -1;
    if (ui_components_reserve(ctx, ctx->component_count + 1) != 0)
        return -1;
    ui_component_t* c = &ctx->components[ctx->component_count];
    memset(c, 0, sizeof(*c));
    c->in_use = 1;
    c->id = ctx->next_component_id++;
    c->type = type;
    c->preferred_h = 24;
    c->bg_color = 0xFF2B3440u;
    c->fg_color = 0xFFFFFFFFu;
    c->border_color = 0xFF536271u;
    c->border_px = 1;
    c->padding_px = 6;
    c->gap_px = 6;

#define UI_ALLOC_DATA(T)                                                                           \
    do {                                                                                           \
        T* _d = (T*)malloc(sizeof(T));                                                             \
        if (!_d)                                                                                   \
            return -1;                                                                             \
        memset(_d, 0, sizeof(T));                                                                  \
        c->component_data = _d;                                                                    \
    } while (0)

    switch (type) {
    case UI_COMPONENT_LABEL:
    case UI_COMPONENT_BUTTON:
    case UI_COMPONENT_TEXT_INPUT:
        UI_ALLOC_DATA(ui_text_data_t);
        break;
    case UI_COMPONENT_CHECKBOX:
        UI_ALLOC_DATA(ui_checkbox_data_t);
        break;
    case UI_COMPONENT_SCROLL_VIEW:
        UI_ALLOC_DATA(ui_scroll_view_data_t);
        break;
    case UI_COMPONENT_LIST_VIEW:
        UI_ALLOC_DATA(ui_list_view_data_t);
        ((ui_list_view_data_t*)c->component_data)->list.selected = -1;
        break;
    case UI_COMPONENT_TREE_VIEW:
        UI_ALLOC_DATA(ui_tree_view_data_t);
        ((ui_tree_view_data_t*)c->component_data)->list.selected = -1;
        break;
    case UI_COMPONENT_DROPDOWN:
        UI_ALLOC_DATA(ui_dropdown_data_t);
        ((ui_dropdown_data_t*)c->component_data)->list.selected = -1;
        break;
    case UI_COMPONENT_MENU_ITEM:
        UI_ALLOC_DATA(ui_menu_item_data_t);
        ((ui_menu_item_data_t*)c->component_data)->hovered_child_id = 0;
        break;
    case UI_COMPONENT_MENU_BAR:
        UI_ALLOC_DATA(ui_menu_bar_data_t);
        break;
    default:
        break; /* PANEL needs no per-instance data */
    }
#undef UI_ALLOC_DATA

    ctx->component_count++;
    return c->id;
}

/* Append `child_id` as the last child of `parent_id`. Returns 0 on success and
 * -1 when either id does not resolve or the two are the same. The child is
 * re-parented unconditionally: it is not unlinked from a previous parent first,
 * so appending an already-linked component leaves the old parent's sibling
 * chain pointing at it. Cycles are not detected beyond the self-append check. */
static inline int32_t ui_component_append_child(ui_context_t* ctx, int32_t parent_id,
                                                int32_t child_id) {
    ui_component_t* parent = ui_component_by_id(ctx, parent_id);
    ui_component_t* child = ui_component_by_id(ctx, child_id);
    if (!parent || !child || parent_id == child_id)
        return -1;
    child->parent_id = parent_id;
    child->next_sibling_id = 0;
    if (parent->first_child_id == 0) {
        parent->first_child_id = child_id;
        return 0;
    }
    int32_t cur_id = parent->first_child_id;
    while (cur_id > 0) {
        ui_component_t* cur = ui_component_by_id(ctx, cur_id);
        if (!cur)
            return -1;
        if (cur->next_sibling_id == 0) {
            cur->next_sibling_id = child_id;
            return 0;
        }
        cur_id = cur->next_sibling_id;
    }
    return -1;
}

/* Typed wrappers over ui_component_alloc(): each returns the new component's
 * id, or -1 on failure, and leaves it detached for ui_component_append_child().
 * PANEL stacks its children vertically; ROW and MENU_BAR stack them
 * horizontally (MENU_BAR reading each child's preferred_h as a width). */
static inline int32_t ui_component_create_panel(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_PANEL);
}
static inline int32_t ui_component_create_row(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_ROW);
}
static inline int32_t ui_component_create_label(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_LABEL);
}
static inline int32_t ui_component_create_button(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_BUTTON);
}
static inline int32_t ui_component_create_checkbox(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_CHECKBOX);
}
static inline int32_t ui_component_create_text_input(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_TEXT_INPUT);
}
static inline int32_t ui_component_create_scroll_view(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_SCROLL_VIEW);
}
static inline int32_t ui_component_create_list_view(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_LIST_VIEW);
}
static inline int32_t ui_component_create_tree_view(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_TREE_VIEW);
}
static inline int32_t ui_component_create_dropdown(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_DROPDOWN);
}
static inline int32_t ui_component_create_menu_bar(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_MENU_BAR);
}
static inline int32_t ui_component_create_menu_item(ui_context_t* ctx) {
    return ui_component_alloc(ctx, UI_COMPONENT_MENU_ITEM);
}

/* Set the label of a text-bearing component (LABEL, BUTTON, TEXT_INPUT,
 * CHECKBOX, DROPDOWN, MENU_ITEM). A NULL `text` clears it to the empty string.
 * Silently does nothing for an unknown id, and silently ignores an allocation
 * failure. Does not mark the context dirty — call ui_mark_dirty() to repaint.
 *
 * FIXME: no type check happens (see ui_component_set_text_owned), so calling
 * this on a LIST_VIEW, TREE_VIEW, SCROLL_VIEW or MENU_BAR corrupts that
 * component's data instead of failing. */
static inline void ui_component_set_text(ui_context_t* ctx, int32_t id, const char* text) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c)
        return;
    (void)ui_component_set_text_owned(c, text ? text : "");
}

/* Create a child MENU_ITEM under parent_id with the given text.
 * Returns the new child's component id, or -1 on failure.
 * Equivalent to create_menu_item + set_text + append_child. */
static inline int32_t ui_menu_item_add_item(ui_context_t* ctx, int32_t parent_id,
                                            const char* text) {
    const int32_t child_id = ui_component_create_menu_item(ctx);
    if (child_id < 0)
        return -1;
    ui_component_set_text(ctx, child_id, text ? text : "");
    ui_component_append_child(ctx, parent_id, child_id);
    return child_id;
}

/* Install the click callback on any component kind and mark it clickable, so it
 * starts answering hit-tests. `user` is stored verbatim and handed back to the
 * callback; libui neither copies nor frees it, so it must outlive the context.
 * A NULL `cb` clears the callback but still leaves the component clickable.
 * Does nothing for an unknown id. */
static inline void ui_component_set_button_action(ui_context_t* ctx, int32_t id,
                                                  ui_button_click_cb_t cb, void* user) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c)
        return;
    c->clickable = 1;
    c->on_click = cb;
    c->on_click_user = user;
}

/* Install the row-activation callback, fired on a left double-click over a row.
 * Accepts LIST_VIEW and TREE_VIEW only; any other kind (or an unknown id) is
 * silently ignored rather than reinterpreting its data. */
static inline void ui_component_set_list_view_activate_action(ui_context_t* ctx, int32_t id,
                                                              ui_list_view_item_cb_t cb,
                                                              void* user) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c || !c->component_data)
        return;
    if (c->type == UI_COMPONENT_LIST_VIEW) {
        ((ui_list_view_data_t*)c->component_data)->on_activate = cb;
        ((ui_list_view_data_t*)c->component_data)->on_activate_user = user;
    } else if (c->type == UI_COMPONENT_TREE_VIEW) {
        ((ui_tree_view_data_t*)c->component_data)->on_activate = cb;
        ((ui_tree_view_data_t*)c->component_data)->on_activate_user = user;
    }
}

/* Install the secondary-click callback, fired on a right click over a row.
 * Accepts LIST_VIEW and TREE_VIEW only; other kinds are silently ignored. */
static inline void ui_component_set_list_view_secondary_click_action(ui_context_t* ctx, int32_t id,
                                                                     ui_list_view_item_cb_t cb,
                                                                     void* user) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c || !c->component_data)
        return;
    if (c->type == UI_COMPONENT_LIST_VIEW) {
        ((ui_list_view_data_t*)c->component_data)->on_secondary_click = cb;
        ((ui_list_view_data_t*)c->component_data)->on_secondary_click_user = user;
    } else if (c->type == UI_COMPONENT_TREE_VIEW) {
        ((ui_tree_view_data_t*)c->component_data)->on_secondary_click = cb;
        ((ui_tree_view_data_t*)c->component_data)->on_secondary_click_user = user;
    }
}

/* Set a CHECKBOX's state; any non-zero `checked` stores 1. Type-checked: a
 * non-CHECKBOX id does nothing. Does not mark the context dirty. */
static inline void ui_component_set_checked(ui_context_t* ctx, int32_t id, int32_t checked) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c || c->type != UI_COMPONENT_CHECKBOX || !c->component_data)
        return;
    ((ui_checkbox_data_t*)c->component_data)->checked = checked ? 1 : 0;
}

/* Read a CHECKBOX's state: 1 when checked, 0 when unchecked and also 0 for
 * NULL, a non-CHECKBOX component or missing data — the two cases are not
 * distinguishable. Unlike the setters this takes the component pointer, so it
 * is usable from inside a callback that already resolved it. */
static inline int32_t ui_component_get_checked(const ui_component_t* c) {
    if (!c || c->type != UI_COMPONENT_CHECKBOX || !c->component_data)
        return 0;
    return ((ui_checkbox_data_t*)c->component_data)->checked;
}

/* Append an entry to a collection component. Accepts LIST_VIEW, TREE_VIEW,
 * DROPDOWN — which store a copy of `item` in their flat list and return its
 * zero-based index — and MENU_ITEM, which instead creates a child MENU_ITEM and
 * returns that child's component id (see the note below). Returns -1 for any
 * other kind, an unknown id, a NULL `item`, or a failed allocation.
 *
 * The caller keeps ownership of `item`; the copy is freed by
 * ui_component_collection_clear() or by the component's destroy_data. */
static inline int32_t ui_component_list_append(ui_context_t* ctx, int32_t id, const char* item) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c || !item || !c->component_data)
        return -1;

    /* MENU_ITEM has no flat list: the entry becomes a child component, and the
     * return value is that child's component id rather than a list index.
     * Callers that need a callback set the child's on_click after this call. */
    if (c->type == UI_COMPONENT_MENU_ITEM) {
        const int32_t child_id = ui_component_alloc(ctx, UI_COMPONENT_MENU_ITEM);
        if (child_id < 0)
            return -1;
        ui_component_set_text(ctx, child_id, item);
        ui_component_append_child(ctx, id, child_id);
        return child_id;
    }

    ui_list_data_t* ld;
    if (c->type == UI_COMPONENT_LIST_VIEW) {
        ld = &((ui_list_view_data_t*)c->component_data)->list;
    } else if (c->type == UI_COMPONENT_TREE_VIEW) {
        ld = &((ui_tree_view_data_t*)c->component_data)->list;
    } else if (c->type == UI_COMPONENT_DROPDOWN) {
        ld = &((ui_dropdown_data_t*)c->component_data)->list;
    } else {
        return -1;
    }

    if (ld->count >= ld->capacity) {
        int32_t cap = ld->capacity > 0 ? ld->capacity : UI_LIST_INITIAL_CAP;
        while (cap <= ld->count)
            cap *= 2;
        char** new_items = (char**)malloc((size_t)cap * sizeof(char*));
        if (!new_items)
            return -1;
        for (int32_t i = 0; i < cap; ++i)
            new_items[i] = 0;
        if (ld->items && ld->count > 0) {
            memcpy(new_items, ld->items, (size_t)ld->count * sizeof(char*));
            free(ld->items);
        }
        ld->items = new_items;
        ld->capacity = cap;
    }
    const size_t len = strlen(item) + 1;
    char* copy = (char*)malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, item, len);
    ld->items[ld->count] = copy;
    ld->count += 1;
    return ld->count - 1;
}

/* Append a row to a TREE_VIEW at indentation level `depth` (negative depths are
 * stored as 0). Returns the row index, or -1 for a non-TREE_VIEW id or a failed
 * allocation. The depth array is grown to match the item array, so a failure
 * here can leave the row appended without its depth recorded. */
static inline int32_t ui_component_tree_append(ui_context_t* ctx, int32_t id, const char* item,
                                               int32_t depth) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    ui_tree_view_data_t* td;
    int32_t idx;
    int32_t cap;
    int32_t* new_depths;

    if (!c || c->type != UI_COMPONENT_TREE_VIEW || !c->component_data)
        return -1;
    td = (ui_tree_view_data_t*)c->component_data;
    idx = ui_component_list_append(ctx, id, item);
    if (idx < 0)
        return -1;

    if (td->list.capacity <= 0)
        return -1;
    if (!td->depths || td->depth_capacity < td->list.capacity) {
        cap = td->list.capacity;
        new_depths = (int32_t*)malloc((size_t)cap * sizeof(int32_t));
        if (!new_depths)
            return -1;
        for (int32_t i = 0; i < cap; ++i)
            new_depths[i] = 0;
        if (td->depths && td->depth_capacity > 0) {
            memcpy(new_depths, td->depths, (size_t)td->depth_capacity * sizeof(int32_t));
            free(td->depths);
        }
        td->depths = new_depths;
        td->depth_capacity = cap;
    }
    td->depths[idx] = depth > 0 ? depth : 0;
    return idx;
}

/* Free every item of a LIST_VIEW, TREE_VIEW or DROPDOWN and reset the selection
 * to -1; for the two scrolling kinds the scroll offset and range are reset too.
 * The items array itself is kept for reuse, so the capacity survives. Other
 * kinds (including MENU_ITEM, whose entries are child components) and unknown
 * ids are ignored. A TREE_VIEW's depth array is not cleared — stale depths are
 * overwritten as rows are appended again. */
static inline void ui_component_collection_clear(ui_context_t* ctx, int32_t id) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    ui_list_data_t* ld = NULL;
    int32_t* scroll_y = NULL;
    int32_t* scroll_max = NULL;

    if (!c || !c->component_data)
        return;
    if (c->type == UI_COMPONENT_LIST_VIEW) {
        ui_list_view_data_t* d = (ui_list_view_data_t*)c->component_data;
        ld = &d->list;
        scroll_y = &d->scroll_y;
        scroll_max = &d->scroll_max;
    } else if (c->type == UI_COMPONENT_TREE_VIEW) {
        ui_tree_view_data_t* d = (ui_tree_view_data_t*)c->component_data;
        ld = &d->list;
        scroll_y = &d->scroll_y;
        scroll_max = &d->scroll_max;
    } else if (c->type == UI_COMPONENT_DROPDOWN) {
        ld = &((ui_dropdown_data_t*)c->component_data)->list;
    } else {
        return;
    }

    for (int32_t i = 0; i < ld->count; ++i) {
        if (ld->items && ld->items[i]) {
            free(ld->items[i]);
            ld->items[i] = NULL;
        }
    }
    ld->count = 0;
    ld->selected = -1;
    if (scroll_y)
        *scroll_y = 0;
    if (scroll_max)
        *scroll_max = 0;
}

/* Length in bytes (not code points) of a text-bearing component's string,
 * excluding the terminator. Returns 0 for NULL or missing data.
 *
 * Valid only for the kinds whose data starts with ui_text_data_t (LABEL,
 * BUTTON, TEXT_INPUT, CHECKBOX, DROPDOWN, MENU_ITEM); like the setter it does
 * not check the type, so any other kind yields a misread field. */
static inline int32_t ui_component_text_len(const ui_component_t* c) {
    if (!c || !c->component_data)
        return 0;
    /* For components that use ui_text_data_t as (or starting as) their data. */
    ui_text_data_t* td = (ui_text_data_t*)c->component_data;
    return td->text_len;
}

/* Commit a freshly allocated framebuffer into the context. `mapped_ptr` is the
 * guest address the shmem mapping returned. On the first allocation the pointer
 * position is centred in the new window; afterwards the caller's saved position
 * is restored, so a resize does not teleport the cursor. */
__attribute__((noinline)) static void
ui_apply_realloc_state(ui_context_t* ctx, int32_t new_buffer_id, int32_t new_shmem_id,
                       int32_t new_stride, int32_t new_w, int32_t new_h, int32_t mapped_ptr,
                       int32_t first_alloc, int32_t prev_ptr_x, int32_t prev_ptr_y) {
    if (!ctx)
        return;
    ctx->buffer_id = new_buffer_id;
    ctx->shmem_id = new_shmem_id;
    ctx->stride_bytes = new_stride;
    ctx->width = new_w;
    ctx->height = new_h;
    ctx->mapped_base = ptr_cast(uint8_t, (uint32_t)mapped_ptr);
    ctx->pointer_x = first_alloc ? ctx->width / 2 : prev_ptr_x;
    ctx->pointer_y = first_alloc ? ctx->height / 2 : prev_ptr_y;
}

/* Allocate a new shared framebuffer of new_w x new_h for the context's window,
 * map it, and release the previous one. Returns 0 on success (including the
 * no-op case where the size already matches and a buffer is mapped) and -1 on
 * failure, in which case the old buffer stays in place except when the
 * compositor allocation itself succeeded but the mapping failed — that path
 * releases the new buffer and leaves the old one current.
 *
 * On success ctx->mapped_base, ctx->width, ctx->height and ctx->stride_bytes
 * all change, so any pixel pointer the caller cached is stale. The new buffer's
 * contents are undefined until painted. Blocks for up to three compositor round
 * trips. */
static inline int32_t ui_realloc_buffer(ui_context_t* ctx, int32_t new_w, int32_t new_h) {
    int32_t status = 0, new_buffer_id = 0, new_shmem_id = 0, new_stride = 0;
    if (!ctx || new_w <= 0 || new_h <= 0)
        return -1;
    if (ctx->width == new_w && ctx->height == new_h && ctx->mapped_base)
        return 0;
    const int32_t prev_ptr_x = ctx->pointer_x;
    const int32_t prev_ptr_y = ctx->pointer_y;
    const int32_t first_alloc = (ctx->mapped_base == NULL);
    if (ui_send_gfx(ctx->gfx_endpoint,
                    ctx->reply_endpoint,
                    ctx->req_id++,
                    GFX_IPC_ALLOC_SHARED_BUFFER,
                    ctx->window_id,
                    new_w,
                    new_h,
                    0,
                    &status,
                    &new_buffer_id,
                    &new_shmem_id,
                    &new_stride) != 0 ||
        status != WASMOS_ERR_NONE) {
        return -1;
    }
    const int32_t bytes = (new_stride * new_h + (UI_PAGE_SIZE - 1)) & ~(UI_PAGE_SIZE - 1);
    const int32_t mapped_ptr = wasmos_shmem_map_auto(new_shmem_id, bytes);
    if (mapped_ptr < 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint,
                          ctx->reply_endpoint,
                          ctx->req_id++,
                          GFX_IPC_RELEASE_SHARED_BUFFER,
                          new_buffer_id,
                          0,
                          0,
                          0,
                          &status,
                          0,
                          0,
                          0);
        return -1;
    }
    if (ctx->shmem_id > 0)
        (void)wasmos_shmem_unmap(ctx->shmem_id);
    if (ctx->buffer_id > 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint,
                          ctx->reply_endpoint,
                          ctx->req_id++,
                          GFX_IPC_RELEASE_SHARED_BUFFER,
                          ctx->buffer_id,
                          0,
                          0,
                          0,
                          &status,
                          0,
                          0,
                          0);
    }
    ui_apply_realloc_state(ctx,
                           new_buffer_id,
                           new_shmem_id,
                           new_stride,
                           new_w,
                           new_h,
                           mapped_ptr,
                           first_alloc,
                           prev_ptr_x,
                           prev_ptr_y);
    return 0;
}

static inline void ui_destroy(ui_context_t* ctx);

/* Create the font reply endpoint, look up the "font" service and open Roboto at
 * ctx->font_px, storing the handle in ctx->font_handle. Returns 0 on success
 * and -1 on failure. The lookup retries up to 2048 times, yielding the
 * scheduler between attempts, so it tolerates the font service still starting
 * up but gives up rather than waiting forever. */
static inline int32_t ui_init_font(ui_context_t* ctx) {
    wasmos_ipc_message_t reply;
    if (!ctx)
        return -1;
    ctx->font_reply_endpoint = wasmos_ipc_create_endpoint();
    if (ctx->font_reply_endpoint <= 0)
        return -1;
    for (int32_t spins = 0; spins < 2048; ++spins) {
        ctx->font_endpoint =
            wasmos_svc_lookup(ctx->proc_endpoint, ctx->font_reply_endpoint, "font", ctx->req_id++);
        if (ctx->font_endpoint >= 0)
            break;
        (void)wasmos_sched_yield();
    }
    if (ctx->font_endpoint < 0)
        return -1;
    if (wasmos_ipc_call(ctx->font_endpoint,
                        ctx->font_reply_endpoint,
                        FONT_IPC_OPEN_FONT_REQ,
                        ctx->req_id++,
                        FONT_ID_ROBOTO,
                        ctx->font_px,
                        0,
                        0,
                        &reply) != 0)
        return -1;
    if (reply.type != FONT_IPC_RESP || reply.arg0 != WASMOS_ERR_NONE || reply.arg1 <= 0)
        return -1;
    ctx->font_handle = reply.arg1;
    return 0;
}

/* Bring up a libui window: zero the context, create the pushed-event endpoint,
 * resolve the gfx and font services, create a `width` x `height` window,
 * allocate its framebuffer, register the component vtables, and create the root
 * PANEL sized to the window. Returns 0 on success, -1 on any failure — the
 * failure path runs ui_destroy(), so the context is left zeroed and safe to
 * discard or re-initialise.
 *
 * `proc_endpoint` must be > 0 (the process-manager endpoint from startup arg 0)
 * and `reply_endpoint` an endpoint the caller owns, used for libui's
 * synchronous requests. The context takes no ownership of either. Service
 * lookups retry with sched_yield up to 2048 times.
 *
 * After this returns, ctx->root_id is the tree root and ctx->dirty is set, so
 * the first ui_loop_drain() paints. */
static inline int32_t ui_init(ui_context_t* ctx, int32_t proc_endpoint, int32_t reply_endpoint,
                              int32_t width, int32_t height) {
    int32_t status = 0;
    int32_t a1 = 0;
    int32_t a2 = 0;
    int32_t a3 = 0;
    if (!ctx || proc_endpoint <= 0 || reply_endpoint < 0 || width <= 0 || height <= 0)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->proc_endpoint = proc_endpoint;
    ctx->reply_endpoint = reply_endpoint;
    ctx->event_endpoint = wasmos_ipc_create_endpoint();
    if (ctx->event_endpoint < 0)
        return -1;
    ctx->req_id = UI_REQ_BASE;
    ctx->font_px = 14;
    ctx->next_component_id = 1;
    if (ui_components_reserve(ctx, UI_COMPONENTS_INITIAL_CAP) != 0)
        goto fail;

    for (int32_t spins = 0; spins < 2048; ++spins) {
        ctx->gfx_endpoint = wasmos_svc_lookup(proc_endpoint, reply_endpoint, "gfx", ctx->req_id++);
        if (ctx->gfx_endpoint >= 0)
            break;
        (void)wasmos_sched_yield();
    }
    if (ctx->gfx_endpoint < 0)
        goto fail;
    if (ui_init_font(ctx) != 0)
        goto fail;

    if (ui_send_gfx(ctx->gfx_endpoint,
                    ctx->event_endpoint,
                    ctx->req_id++,
                    GFX_IPC_CREATE_WINDOW,
                    width,
                    height,
                    (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status,
                    &a1,
                    &a2,
                    &a3) != 0 ||
        status != WASMOS_ERR_NONE) {
        goto fail;
    }
    ctx->window_id = a1;
    if (ui_realloc_buffer(ctx, width, height) != 0)
        goto fail;
    ui_init_component_ops();
    ctx->root_id = ui_component_create_panel(ctx);
    if (ctx->root_id < 0)
        goto fail;
    ui_component_t* root = ui_component_by_id(ctx, ctx->root_id);
    if (!root)
        goto fail;
    root->bounds.x = 0;
    root->bounds.y = 0;
    root->bounds.w = width;
    root->bounds.h = height;
    root->padding_px = 8;
    root->gap_px = 8;
    root->bg_color = 0xFF202833u;
    ctx->dirty = 1;
    return 0;
fail:
    ui_destroy(ctx);
    return -1;
}

/* Set the window title shown in the compositor chrome and task list. Returns 0
 * on success and -1 on failure, including a `title` that is empty or longer
 * than 47 bytes — the limit is a refusal, not a truncation. The string is
 * handed over through a one-page shmem region that is unmapped again before
 * returning, so the caller keeps ownership of `title`. Blocks for one
 * compositor round trip. */
static inline int32_t ui_window_set_title(ui_context_t* ctx, const char* title) {
    if (!ctx || !title || ctx->gfx_endpoint <= 0 || ctx->window_id <= 0)
        return -1;
    const int32_t len = (int32_t)strlen(title);
    if (len <= 0 || len > 47)
        return -1;
    const int32_t shmem_id = wasmos_shmem_create(1, 0);
    if (shmem_id <= 0)
        return -1;
    const int32_t mapped = wasmos_shmem_map_auto(shmem_id, UI_PAGE_SIZE);
    if (mapped < 0) {
        (void)wasmos_shmem_unmap(shmem_id);
        return -1;
    }
    uint8_t* ptr = ptr_cast(uint8_t, (uint32_t)mapped);
    memcpy(ptr, title, (size_t)len);
    ptr[len] = '\0';
    if (wasmos_shmem_flush(shmem_id, addr_cast(int32_t, ptr), len + 1) != 0) {
        (void)wasmos_shmem_unmap(shmem_id);
        return -1;
    }
    int32_t status = 0;
    ui_send_gfx(ctx->gfx_endpoint,
                ctx->reply_endpoint,
                ctx->req_id++,
                GFX_IPC_SET_WINDOW_TITLE,
                ctx->window_id,
                shmem_id,
                len,
                0,
                &status,
                0,
                0,
                0);
    (void)wasmos_shmem_unmap(shmem_id);
    return (status == WASMOS_ERR_NONE) ? 0 : -1;
}

/* ui_init() variant for the system menu bar: sizes the window to the full
 * display width by a fixed 28 px height, pins it to the top-left, and marks it
 * TOPMOST | NO_CHROME | NO_TASK_LIST so it is not itself a manageable window.
 * The root is a MENU_BAR (horizontal layout) rather than a PANEL, and the font
 * is 13 px instead of 14. Returns 0 on success, -1 on failure with the context
 * destroyed and zeroed.
 *
 * Only one menu bar makes sense per display; nothing here enforces that. */
static inline int32_t ui_menu_bar_init(ui_context_t* ctx, int32_t proc_endpoint,
                                       int32_t reply_endpoint) {
    int32_t status = 0, a1 = 0, a2 = 0, a3 = 0;
    if (!ctx || proc_endpoint <= 0 || reply_endpoint < 0)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->proc_endpoint = proc_endpoint;
    ctx->reply_endpoint = reply_endpoint;
    ctx->event_endpoint = wasmos_ipc_create_endpoint();
    if (ctx->event_endpoint < 0)
        return -1;
    ctx->req_id = UI_REQ_BASE;
    ctx->font_px = 13;
    ctx->next_component_id = 1;
    if (ui_components_reserve(ctx, UI_COMPONENTS_INITIAL_CAP) != 0)
        goto mb_fail;

    for (int32_t spins = 0; spins < 2048; ++spins) {
        ctx->gfx_endpoint = wasmos_svc_lookup(proc_endpoint, reply_endpoint, "gfx", ctx->req_id++);
        if (ctx->gfx_endpoint >= 0)
            break;
        (void)wasmos_sched_yield();
    }
    if (ctx->gfx_endpoint < 0)
        goto mb_fail;
    if (ui_init_font(ctx) != 0)
        goto mb_fail;

    if (ui_send_gfx(ctx->gfx_endpoint,
                    reply_endpoint,
                    ctx->req_id++,
                    GFX_IPC_GET_DISPLAY_INFO,
                    0,
                    0,
                    0,
                    0,
                    &status,
                    &a1,
                    &a2,
                    &a3) != 0 ||
        status != WASMOS_ERR_NONE || a1 <= 0)
        goto mb_fail;

    const int32_t screen_w = a1;
    const int32_t bar_h = 28;

    if (ui_send_gfx(ctx->gfx_endpoint,
                    ctx->event_endpoint,
                    ctx->req_id++,
                    GFX_IPC_CREATE_WINDOW,
                    screen_w,
                    bar_h,
                    (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status,
                    &a1,
                    &a2,
                    &a3) != 0 ||
        status != WASMOS_ERR_NONE)
        goto mb_fail;
    ctx->window_id = a1;

    if (ui_send_gfx(ctx->gfx_endpoint,
                    reply_endpoint,
                    ctx->req_id++,
                    GFX_IPC_SET_WINDOW_FLAGS,
                    ctx->window_id,
                    (int32_t)(GFX_WINDOW_FLAG_TOPMOST | GFX_WINDOW_FLAG_NO_CHROME |
                              GFX_WINDOW_FLAG_NO_TASK_LIST),
                    0,
                    0,
                    &status,
                    0,
                    0,
                    0) != 0 ||
        status != WASMOS_ERR_NONE)
        goto mb_fail;

    if (ui_send_gfx(ctx->gfx_endpoint,
                    reply_endpoint,
                    ctx->req_id++,
                    GFX_IPC_MOVE_WINDOW,
                    ctx->window_id,
                    0,
                    0,
                    0,
                    &status,
                    0,
                    0,
                    0) != 0 ||
        status != WASMOS_ERR_NONE)
        goto mb_fail;

    if (ui_realloc_buffer(ctx, screen_w, bar_h) != 0)
        goto mb_fail;

    ui_init_component_ops();
    ctx->root_id = ui_component_create_menu_bar(ctx);
    if (ctx->root_id < 0)
        goto mb_fail;

    ui_component_t* mbroot = ui_component_by_id(ctx, ctx->root_id);
    mbroot->bounds.x = 0;
    mbroot->bounds.y = 0;
    mbroot->bounds.w = screen_w;
    mbroot->bounds.h = bar_h;
    mbroot->padding_px = 2;
    mbroot->gap_px = 0;
    mbroot->bg_color = 0xFF1A2233u;

    ctx->dirty = 1;
    return 0;

mb_fail:
    ui_destroy(ctx);
    return -1;
}

/* Tear the context down: destroy the window, release and unmap the framebuffer
 * and the font scratch regions, free every component's data through its
 * destroy_data (or plain free() when it has none), free the pool, and zero the
 * context. Safe on a NULL pointer, on a partially initialised context (each
 * step is guarded by its own id check) and when called twice.
 *
 * The endpoints passed to ui_init() are not destroyed — the caller owns those.
 * Menu-item popups still open are only partly reclaimed; see the FIXME in
 * ui_menu_item_destroy_data(). */
static inline void ui_destroy(ui_context_t* ctx) {
    int32_t status = 0;
    if (!ctx)
        return;
    if (ctx->window_id > 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint,
                          ctx->reply_endpoint,
                          ctx->req_id++,
                          GFX_IPC_DESTROY_WINDOW,
                          ctx->window_id,
                          0,
                          0,
                          0,
                          &status,
                          0,
                          0,
                          0);
    }
    if (ctx->buffer_id > 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint,
                          ctx->reply_endpoint,
                          ctx->req_id++,
                          GFX_IPC_RELEASE_SHARED_BUFFER,
                          ctx->buffer_id,
                          0,
                          0,
                          0,
                          &status,
                          0,
                          0,
                          0);
    }
    if (ctx->shmem_id > 0)
        (void)wasmos_shmem_unmap(ctx->shmem_id);
    if (ctx->font_text_shmem_id > 0)
        (void)wasmos_shmem_unmap(ctx->font_text_shmem_id);
    if (ctx->font_mask_shmem_id > 0)
        (void)wasmos_shmem_unmap(ctx->font_mask_shmem_id);

    for (int32_t i = 0; i < ctx->component_count; ++i) {
        ui_component_t* c = &ctx->components[i];
        const ui_component_ops_t* ops = &ui_component_ops[c->type];
        if (ops->destroy_data) {
            ops->destroy_data(c);
        } else if (c->component_data) {
            free(c->component_data);
            c->component_data = NULL;
        }
    }
    if (ctx->components)
        free(ctx->components);
    memset(ctx, 0, sizeof(*ctx));
}

/* Non-zero when (x, y) is inside `r`, treating the left/top edges as inside and
 * the right/bottom edges as outside. An empty rect contains nothing. */
static inline int32_t ui_point_in_bounds(int32_t x, int32_t y, ui_rect_t r) {
    return x >= r.x && y >= r.y && x < (r.x + r.w) && y < (r.y + r.h);
}

/* Layout prototype so component headers (inserted below) can call back for child recursion in
 * containers. */
static inline void ui_layout_vertical(ui_context_t* ctx, int32_t parent_id);

/* Prototypes for main renderer so component headers can call back (e.g. scroll_view and
 * menu_bar render their children via the core clip walker; menu_item uses its own helpers). */
static inline void ui_render_component_clip(ui_context_t* ctx, int32_t id, ui_rect_t clip,
                                            int32_t offset_y);
static inline void ui_render_component(ui_context_t* ctx, int32_t id);

/* Component headers are included here, after the core helpers they call and
 * before the layout/render dispatchers that call them. */
#include "wasmos/libui_label.h"
#include "wasmos/libui_row.h"
#include "wasmos/libui_button.h"
#include "wasmos/libui_checkbox.h"
#include "wasmos/libui_text_input.h"
#include "wasmos/libui_list_view.h"
#include "wasmos/libui_tree_view.h"
#include "wasmos/libui_dropdown.h"
#include "wasmos/libui_scroll_view.h"
#include "wasmos/libui_menu_bar.h"
#include "wasmos/libui_menu_item.h"

/* Register the vtable implementations owned by the component headers. Called
 * from ui_init / ui_menu_bar_init before any component is created; an unset
 * entry means the core's generic behaviour applies for that component kind. */
static inline void ui_init_component_ops(void) {
    memset(ui_component_ops, 0, sizeof(ui_component_ops));

    ui_component_ops[UI_COMPONENT_LABEL].render = ui_render_label;

    ui_component_ops[UI_COMPONENT_ROW].layout = ui_layout_row;

    ui_component_ops[UI_COMPONENT_BUTTON].render = ui_render_button;
    ui_component_ops[UI_COMPONENT_BUTTON].handle_pointer_release = ui_button_handle_pointer_release;

    ui_component_ops[UI_COMPONENT_CHECKBOX].render = ui_render_checkbox;
    ui_component_ops[UI_COMPONENT_CHECKBOX].handle_pointer_release =
        ui_checkbox_handle_pointer_release;

    ui_component_ops[UI_COMPONENT_TEXT_INPUT].render = ui_render_text_input;
    ui_component_ops[UI_COMPONENT_TEXT_INPUT].handle_key = ui_text_input_handle_key;

    ui_component_ops[UI_COMPONENT_LIST_VIEW].render = ui_render_list_view;
    ui_component_ops[UI_COMPONENT_LIST_VIEW].layout = ui_layout_list_view;
    ui_component_ops[UI_COMPONENT_LIST_VIEW].handle_pointer_press =
        ui_list_view_handle_pointer_press;
    ui_component_ops[UI_COMPONENT_LIST_VIEW].handle_scroll_drag = ui_list_view_handle_scroll_drag;

    ui_component_ops[UI_COMPONENT_TREE_VIEW].render = ui_render_tree_view;
    ui_component_ops[UI_COMPONENT_TREE_VIEW].layout = ui_layout_tree_view;
    ui_component_ops[UI_COMPONENT_TREE_VIEW].handle_pointer_press =
        ui_tree_view_handle_pointer_press;
    ui_component_ops[UI_COMPONENT_TREE_VIEW].handle_scroll_drag = ui_tree_view_handle_scroll_drag;
    ui_component_ops[UI_COMPONENT_TREE_VIEW].destroy_data = ui_tree_view_destroy_data;

    ui_component_ops[UI_COMPONENT_DROPDOWN].render = ui_render_dropdown;
    ui_component_ops[UI_COMPONENT_DROPDOWN].layout = ui_layout_dropdown;
    ui_component_ops[UI_COMPONENT_DROPDOWN].handle_pointer_press = ui_dropdown_handle_pointer_press;
    ui_component_ops[UI_COMPONENT_DROPDOWN].handle_key = ui_dropdown_handle_key;
    ui_component_ops[UI_COMPONENT_DROPDOWN].popup_contains = ui_dropdown_popup_contains;
    ui_component_ops[UI_COMPONENT_DROPDOWN].destroy_data =
        NULL; /* data freed via generic or explicit close */

    ui_component_ops[UI_COMPONENT_SCROLL_VIEW].render = ui_render_scroll_view;
    ui_component_ops[UI_COMPONENT_SCROLL_VIEW].layout = ui_layout_scroll_view;
    ui_component_ops[UI_COMPONENT_SCROLL_VIEW].handle_scroll_drag =
        ui_scroll_view_handle_scroll_drag;

    ui_component_ops[UI_COMPONENT_MENU_BAR].render = ui_render_menu_bar;
    ui_component_ops[UI_COMPONENT_MENU_BAR].layout = ui_layout_menu_bar;

    ui_component_ops[UI_COMPONENT_MENU_ITEM].render = ui_render_menu_item;
    ui_component_ops[UI_COMPONENT_MENU_ITEM].handle_pointer_release =
        NULL; /* handled via the global ui_menu_item_handle_pointer_release */
    ui_component_ops[UI_COMPONENT_MENU_ITEM].popup_contains = ui_menu_item_popup_contains;
    ui_component_ops[UI_COMPONENT_MENU_ITEM].destroy_data = ui_menu_item_destroy_data;
}

/* Lay out `parent_id`'s children and recurse into theirs. When the parent's
 * kind registered a layout op, that op takes over completely; otherwise
 * children are stacked top to bottom inside the parent's padding, each taking
 * the full inner width and its own preferred_h (floored at 8 px), separated by
 * gap_px. The parent's own bounds are an input, never modified here, and
 * children may be laid out past the parent's bottom edge — clipping, not
 * layout, keeps them inside. */
static inline void ui_layout_vertical(ui_context_t* ctx, int32_t parent_id) {
    ui_component_t* p = ui_component_by_id(ctx, parent_id);
    if (!p)
        return;

    const ui_component_ops_t* ops = &ui_component_ops[p->type];
    if (ops->layout) {
        ops->layout(ctx, p);
        return;
    }

    /* Generic vertical layout for panels, labels, buttons, etc. */
    int32_t x = p->bounds.x + p->padding_px;
    int32_t y = p->bounds.y + p->padding_px;
    const int32_t w = p->bounds.w - (p->padding_px * 2);

    int32_t child_id = p->first_child_id;
    while (child_id > 0) {
        ui_component_t* c = ui_component_by_id(ctx, child_id);
        if (!c)
            break;
        const int32_t h = c->preferred_h > 8 ? c->preferred_h : 8;
        c->bounds.x = x;
        c->bounds.y = y;
        c->bounds.w = w;
        c->bounds.h = h;

        const ui_component_ops_t* child_ops = &ui_component_ops[c->type];
        if (child_ops->layout) {
            child_ops->layout(ctx, c);
        } else if (c->first_child_id > 0) {
            ui_layout_vertical(ctx, c->id);
        }

        y += h + p->gap_px;
        child_id = c->next_sibling_id;
    }
}

/* Paint one component and, for most kinds, its subtree.
 *
 * The pass is: shift the component's bounds up by `offset_y` (the accumulated
 * scroll offset of the enclosing scroll views), fill that rectangle with
 * bg_color, run the kind's render op, then — unless the kind paints its own
 * children — stroke the generic border and recurse into the children with the
 * same clip and offset. Nothing is drawn outside `clip`, which callers intersect
 * as they descend, and nothing is drawn at all without a mapped framebuffer.
 *
 * Draw order is parent first, then children in sibling order, so a later
 * sibling overdraws an earlier one where they overlap. */
static inline void ui_render_component_clip(ui_context_t* ctx, int32_t id, ui_rect_t clip,
                                            int32_t offset_y) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c || !ctx->mapped_base)
        return;
    const int32_t draw_y = c->bounds.y - offset_y;
    const ui_rect_t draw_bounds = {c->bounds.x, draw_y, c->bounds.w, c->bounds.h};

    ui_fill_rect_clip(ctx->mapped_base,
                      ctx->width,
                      ctx->height,
                      draw_bounds.x,
                      draw_bounds.y,
                      draw_bounds.w,
                      draw_bounds.h,
                      c->bg_color,
                      clip);

    const ui_component_ops_t* ops = &ui_component_ops[c->type];
    if (ops->render) {
        ops->render(ctx, c, draw_bounds, clip, offset_y);
    }

    /* These kinds are self-contained painters: they stroke their own border
     * where they want one, and they own what is inside them — rows and a
     * scrollbar (list/tree), a scrolled child pass (scroll view), bar items
     * (menu bar), or a popup drawn into a separate window (dropdown, menu
     * item). Adding the generic border and child descent below would
     * double-stroke them, and for a menu item would paint its popup rows into
     * this window. Everything else gets both here. */
    if (c->type == UI_COMPONENT_LIST_VIEW || c->type == UI_COMPONENT_TREE_VIEW ||
        c->type == UI_COMPONENT_DROPDOWN || c->type == UI_COMPONENT_SCROLL_VIEW ||
        c->type == UI_COMPONENT_MENU_BAR || c->type == UI_COMPONENT_MENU_ITEM) {
        return;
    }

    ui_stroke_rect_clip(ctx->mapped_base,
                        ctx->width,
                        ctx->height,
                        draw_bounds,
                        c->border_px,
                        c->border_color,
                        clip);

    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        ui_render_component_clip(ctx, child_id, clip, offset_y);
        ui_component_t* child = ui_component_by_id(ctx, child_id);
        if (!child)
            break;
        child_id = child->next_sibling_id;
    }
}

/* Render a subtree against the whole window: ui_render_component_clip() with
 * the clip set to the full framebuffer and no scroll offset. */
static inline void ui_render_component(ui_context_t* ctx, int32_t id) {
    ui_rect_t clip = {0, 0, ctx->width, ctx->height};
    ui_render_component_clip(ctx, id, clip, 0);
}

/* Deepest-last-child-first hit test over the subtree rooted at `id`, returning
 * the component id under (x, y) or -1 when nothing matches. Children are tried
 * in sibling order and the first hit wins, so an earlier sibling shadows a
 * later one where they overlap — the opposite of the paint order. A component
 * answers if its popup_contains op says so, or if the point is inside its
 * bounds; hidden and retired components have stale or zero bounds and are
 * matched or missed accordingly. */
static inline int32_t ui_find_component_at(ui_context_t* ctx, int32_t id, int32_t x, int32_t y) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c)
        return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        const int32_t hit = ui_find_component_at(ctx, child_id, x, y);
        if (hit > 0)
            return hit;
        ui_component_t* child = ui_component_by_id(ctx, child_id);
        if (!child)
            break;
        child_id = child->next_sibling_id;
    }
    const ui_component_ops_t* ops = &ui_component_ops[c->type];
    if (ops->popup_contains && ops->popup_contains(ctx, c, x, y))
        return c->id;
    if (ui_point_in_bounds(x, y, c->bounds))
        return c->id;
    return -1;
}

/* Like ui_find_component_at() but only SCROLL_VIEW, LIST_VIEW and TREE_VIEW
 * answer, and only on their own bounds — popups are not consulted. Used to pick
 * the component a drag scrolls. Returns -1 when the point is over nothing
 * scrollable. */
static inline int32_t ui_find_scrollable_at(ui_context_t* ctx, int32_t id, int32_t x, int32_t y) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c)
        return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        const int32_t hit = ui_find_scrollable_at(ctx, child_id, x, y);
        if (hit > 0)
            return hit;
        ui_component_t* child = ui_component_by_id(ctx, child_id);
        if (!child)
            break;
        child_id = child->next_sibling_id;
    }
    if ((c->type == UI_COMPONENT_SCROLL_VIEW || c->type == UI_COMPONENT_LIST_VIEW ||
         c->type == UI_COMPONENT_TREE_VIEW) &&
        ui_point_in_bounds(x, y, c->bounds))
        return c->id;
    return -1;
}

/* Like ui_find_component_at() but restricted to the row-bearing kinds:
 * LIST_VIEW and TREE_VIEW on their bounds, plus any component whose
 * popup_contains op claims the point. Used to route presses, double-clicks and
 * right-clicks to row handlers. Returns -1 when nothing matches. */
static inline int32_t ui_find_list_view_at(ui_context_t* ctx, int32_t id, int32_t x, int32_t y) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c)
        return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        const int32_t hit = ui_find_list_view_at(ctx, child_id, x, y);
        if (hit > 0)
            return hit;
        ui_component_t* child = ui_component_by_id(ctx, child_id);
        if (!child)
            break;
        child_id = child->next_sibling_id;
    }
    const ui_component_ops_t* ops = &ui_component_ops[c->type];
    if ((c->type == UI_COMPONENT_LIST_VIEW || c->type == UI_COMPONENT_TREE_VIEW) &&
        ui_point_in_bounds(x, y, c->bounds))
        return c->id;
    if (ops->popup_contains && ops->popup_contains(ctx, c, x, y))
        return c->id;
    return -1;
}

/* Find the component that should receive a click at (x, y): one the application
 * marked clickable, one whose popup_contains op claims the point, or a DROPDOWN
 * or MENU_ITEM on its own bounds regardless of the clickable flag. Returns -1
 * when the click misses everything, which the event loop treats as an
 * outside-click and uses to close open dropdowns. */
static inline int32_t ui_find_clickable_at(ui_context_t* ctx, int32_t id, int32_t x, int32_t y) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c)
        return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        int32_t hit = ui_find_clickable_at(ctx, child_id, x, y);
        if (hit > 0)
            return hit;
        ui_component_t* child = ui_component_by_id(ctx, child_id);
        if (!child)
            break;
        child_id = child->next_sibling_id;
    }
    if (c->clickable && ui_point_in_bounds(x, y, c->bounds))
        return c->id;

    const ui_component_ops_t* ops = &ui_component_ops[c->type];
    if (ops->popup_contains && ops->popup_contains(ctx, c, x, y))
        return c->id;
    /* A DROPDOWN or MENU_ITEM answers to a press on its own bounds whether or
     * not the application marked it clickable: opening the popup is intrinsic
     * behaviour, not an application-installed action. */
    if ((c->type == UI_COMPONENT_DROPDOWN || c->type == UI_COMPONENT_MENU_ITEM) &&
        ui_point_in_bounds(x, y, c->bounds))
        return c->id;

    return -1;
}

/* Dispatch one received IPC message into the UI.
 *
 * Returns UI_MSG_CONSUMED for anything libui handled, UI_MSG_IGNORED for a
 * message that is not a compositor event or is an event kind libui does not
 * act on, and UI_MSG_ERROR for a NULL argument. Applications that share the
 * event endpoint with their own traffic should feed every message here and only
 * handle the ones that come back IGNORED.
 *
 * Never blocks — the caller does the receiving. Handling an event can call
 * application callbacks, open or close popup windows (which issues compositor
 * requests and blocks for their replies), and reallocate the framebuffer on a
 * RESIZE. Events for a window id that is neither this window nor one of its
 * popups are consumed and dropped.
 *
 * Pointer and focus events are routed to a menu popup first when any popup is
 * open, by window id for pointer events and by focus ownership otherwise, so
 * popup coordinates are never hit-tested against the bar window's tree. */
static inline int32_t ui_loop_handle_ipc(ui_context_t* ctx, const wasmos_ipc_message_t* msg) {
    if (!ctx || !msg)
        return UI_MSG_ERROR;
    /* Events arrive as compositor-pushed GFX_IPC_PUSH_EVENT: arg1=event_type,
     * arg2=window_id, arg3=packed payload (decoded by the ui_ptr_evt_* /
     * ui_u16_* helpers per event type). */
    if (msg->type != GFX_IPC_PUSH_EVENT)
        return UI_MSG_IGNORED;
    if (msg->arg1 == GFX_EVENT_NONE)
        return UI_MSG_CONSUMED;

    if (msg->arg1 == GFX_EVENT_CLOSE_REQUEST && msg->arg2 == ctx->window_id) {
        ctx->close_requested = 1;
        return UI_MSG_CONSUMED;
    }

    if (msg->arg1 == GFX_EVENT_RESIZE && msg->arg2 == ctx->window_id) {
        const int32_t rw = ui_u16_lo(msg->arg3);
        const int32_t rh = ui_u16_hi(msg->arg3);
        if (rw > 0 && rh > 0 && ui_realloc_buffer(ctx, rw, rh) == 0) {
            ui_component_t* root = ui_component_by_id(ctx, ctx->root_id);
            if (root) {
                root->bounds.w = rw;
                root->bounds.h = rh;
            }
            ctx->dirty = 1;
        }
        return UI_MSG_CONSUMED;
    }

    /* Route events to the popup window that currently owns focus.
     * Pointer coordinates are relative to the focused popup, not necessarily
     * the deepest open submenu. Hover-preview submenus can be open without
     * taking focus, so routing by hierarchy depth misinterprets coordinates. */
    if (msg->arg1 == GFX_EVENT_POINTER || msg->arg1 == GFX_EVENT_FOCUS_GAINED ||
        msg->arg1 == GFX_EVENT_FOCUS_LOST) {
        ui_component_t* popup_target = NULL;
        ui_component_t* popup_focused = NULL;
        ui_component_t* popup_for_window = NULL;
        for (int32_t i = 0; i < ctx->component_count; ++i) {
            ui_component_t* mc = &ctx->components[i];
            if (!mc->in_use || mc->type != UI_COMPONENT_MENU_ITEM)
                continue;
            ui_menu_item_data_t* mpd = (ui_menu_item_data_t*)mc->component_data;
            if (!mpd || mpd->popup_win_id == 0)
                continue;
            if (!popup_target)
                popup_target = mc;
            if (mpd->popup_has_focus)
                popup_focused = mc;
            if (msg->arg1 == GFX_EVENT_POINTER && mpd->popup_win_id == (int32_t)msg->arg2) {
                popup_for_window = mc;
            }
        }
        if (msg->arg1 == GFX_EVENT_FOCUS_GAINED || msg->arg1 == GFX_EVENT_FOCUS_LOST) {
            const int32_t win_id = (int32_t)msg->arg2;
            for (int32_t i = 0; i < ctx->component_count; ++i) {
                ui_component_t* mc = &ctx->components[i];
                if (!mc->in_use || mc->type != UI_COMPONENT_MENU_ITEM)
                    continue;
                ui_menu_item_data_t* mpd = (ui_menu_item_data_t*)mc->component_data;
                if (!mpd || mpd->popup_win_id == 0)
                    continue;
                if (mpd->popup_win_id == win_id) {
                    mpd->popup_has_focus = (msg->arg1 == GFX_EVENT_FOCUS_GAINED) ? 1 : 0;
                } else if (msg->arg1 == GFX_EVENT_FOCUS_GAINED) {
                    mpd->popup_has_focus = 0;
                }
            }
            if (msg->arg1 == GFX_EVENT_FOCUS_GAINED)
                return UI_MSG_CONSUMED;
        }
        if (popup_target) {
            ui_component_t* route_target = (msg->arg1 == GFX_EVENT_POINTER && popup_for_window)
                                               ? popup_for_window
                                               : (popup_focused ? popup_focused : popup_target);
            ui_menu_item_data_t* mpd = (ui_menu_item_data_t*)route_target->component_data;
            if (msg->arg1 == GFX_EVENT_POINTER) {
                ctx->pointer_buttons = ui_ptr_evt_buttons(msg->arg3);
                ui_menu_item_handle_popup_event(ctx, route_target, msg);
                return UI_MSG_CONSUMED;
            }
            if (msg->arg1 == GFX_EVENT_FOCUS_LOST && (int32_t)msg->arg2 == mpd->popup_win_id) {
                /* Suppress dismiss if focus moved to a child's own popup window
                 * (e.g. a sub-menu opened from hover).  Only dismiss when focus
                 * truly leaves the whole menu hierarchy. */
                int32_t child_popup_open = 0;

                int32_t cid = route_target->first_child_id;
                while (cid > 0) {
                    const ui_component_t* ch = ui_component_by_id(ctx, cid);
                    if (!ch)
                        break;
                    const ui_menu_item_data_t* cd = (const ui_menu_item_data_t*)ch->component_data;
                    if (cd && cd->popup_win_id > 0) {
                        child_popup_open = 1;
                        break;
                    }
                    cid = ch->next_sibling_id;
                }

                if (!child_popup_open) {
                    ui_menu_item_dismiss_popup(ctx, route_target);
                    return UI_MSG_CONSUMED;
                }
                return UI_MSG_CONSUMED;
            }
        }
    }

    if (msg->arg1 == GFX_EVENT_POINTER) {
        if ((int32_t)msg->arg2 != ctx->window_id)
            return UI_MSG_CONSUMED;
        const int32_t new_x = ui_ptr_evt_x(msg->arg3);
        const int32_t new_y = ui_ptr_evt_y(msg->arg3);
        const uint32_t buttons = ui_ptr_evt_buttons(msg->arg3);

        ctx->pointer_x = new_x;
        ctx->pointer_y = new_y;
        if (ctx->pointer_x < 0)
            ctx->pointer_x = 0;
        if (ctx->pointer_y < 0)
            ctx->pointer_y = 0;
        if (ctx->pointer_x >= ctx->width)
            ctx->pointer_x = ctx->width - 1;
        if (ctx->pointer_y >= ctx->height)
            ctx->pointer_y = ctx->height - 1;

        const int32_t left_now = ((buttons & 1u) != 0u);
        const int32_t left_prev = ((ctx->pointer_buttons & 1u) != 0u);

        if (left_now && !left_prev) {
            const int32_t focus_id =
                ui_find_component_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t* focus = ui_component_by_id(ctx, focus_id);
            if (focus &&
                (focus->type == UI_COMPONENT_TEXT_INPUT || focus->type == UI_COMPONENT_DROPDOWN)) {
                ctx->focused_component_id = focus->id;
            } else {
                ctx->focused_component_id = 0;
            }

            const int32_t hit_id =
                ui_find_clickable_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t* hit = ui_component_by_id(ctx, hit_id);
            if (hit) {
                hit->pressed = 1;
                ui_mark_dirty(ctx);
            }

            ctx->active_scroll_component_id =
                ui_find_scrollable_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);

            const int32_t list_id =
                ui_find_list_view_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t* lv = ui_component_by_id(ctx, list_id);
            if (lv) {
                const ui_component_ops_t* ops = &ui_component_ops[lv->type];
                if (ops->handle_pointer_press) {
                    ops->handle_pointer_press(ctx, lv, ctx->pointer_x, ctx->pointer_y);
                }
            }
        } else if (!left_now && left_prev) {
            const int32_t hit_id =
                ui_find_clickable_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t* hit = ui_component_by_id(ctx, hit_id);
            if (hit && hit->pressed) {
                const ui_component_ops_t* ops = &ui_component_ops[hit->type];
                if (ops->handle_pointer_release) {
                    ops->handle_pointer_release(ctx, hit);
                } else if (hit->on_click && hit->type != UI_COMPONENT_MENU_ITEM) {
                    /* fallback for components that only registered an on_click.
                     * MENU_ITEM is excluded: its on_click fires via pick_and_invoke
                     * inside ui_menu_item_handle_pointer_release (below) after the
                     * correct selected index has been set. */
                    hit->on_click(ctx, hit->id, hit->on_click_user);
                }
            }

            /* Menu system release reaction is owned by the menu_item component (via its global
             * handler). */
            ui_menu_item_handle_pointer_release(ctx, ctx->pointer_x, ctx->pointer_y);

            /* Dropdown outside-click close is owned by the dropdown component. */
            if (!hit) {
                ui_dropdown_close_all_open(ctx);
            }
            for (int32_t i = 0; i < ctx->component_count; ++i) {
                if (ctx->components[i].in_use && ctx->components[i].pressed) {
                    ctx->components[i].pressed = 0;
                }
            }
            ctx->active_scroll_component_id = 0;
            ui_mark_dirty(ctx);
        }

        ctx->pointer_buttons = buttons;
        return UI_MSG_CONSUMED;
    }

    if (msg->arg1 == GFX_EVENT_POINTER_GESTURE) {
        if ((int32_t)msg->arg2 != ctx->window_id)
            return UI_MSG_CONSUMED;
        const int32_t x = ui_ptr_gesture_x(msg->arg3);
        const int32_t y = ui_ptr_gesture_y(msg->arg3);
        const uint32_t button = ui_ptr_gesture_button(msg->arg3);
        const uint32_t kind = ui_ptr_gesture_kind(msg->arg3);

        ctx->pointer_x = x;
        ctx->pointer_y = y;

        if (kind == GFX_POINTER_GESTURE_DOUBLE_CLICK && button == GFX_POINTER_BUTTON_LEFT) {
            const int32_t list_id = ui_find_list_view_at(ctx, ctx->root_id, x, y);
            ui_component_t* lv = ui_component_by_id(ctx, list_id);
            if (lv) {
                if (lv->type == UI_COMPONENT_TREE_VIEW) {
                    ui_tree_view_handle_activate(ctx, lv, x, y);
                } else {
                    ui_list_view_handle_activate(ctx, lv, x, y);
                }
            }
            return UI_MSG_CONSUMED;
        }

        if (kind == GFX_POINTER_GESTURE_CLICK && button == GFX_POINTER_BUTTON_RIGHT) {
            const int32_t list_id = ui_find_list_view_at(ctx, ctx->root_id, x, y);
            ui_component_t* lv = ui_component_by_id(ctx, list_id);
            if (lv) {
                if (lv->type == UI_COMPONENT_TREE_VIEW) {
                    ui_tree_view_handle_secondary_click(ctx, lv, x, y);
                } else {
                    ui_list_view_handle_secondary_click(ctx, lv, x, y);
                }
            }
            return UI_MSG_CONSUMED;
        }

        if (button == GFX_POINTER_BUTTON_LEFT && kind == GFX_POINTER_GESTURE_DRAG_START) {
            ctx->pointer_drag_button = button;
            ctx->pointer_drag_x = x;
            ctx->pointer_drag_y = y;
            return UI_MSG_CONSUMED;
        }

        if (button == GFX_POINTER_BUTTON_LEFT && kind == GFX_POINTER_GESTURE_DRAG_MOVE) {
            const int32_t dy = y - ctx->pointer_drag_y;
            ctx->pointer_drag_button = button;
            ctx->pointer_drag_x = x;
            ctx->pointer_drag_y = y;
            if (ctx->active_scroll_component_id > 0 && dy != 0) {
                ui_component_t* sv = ui_component_by_id(ctx, ctx->active_scroll_component_id);
                if (sv) {
                    const ui_component_ops_t* ops = &ui_component_ops[sv->type];
                    if (ops->handle_scroll_drag) {
                        ops->handle_scroll_drag(ctx, sv, dy);
                    }
                }
            }
            return UI_MSG_CONSUMED;
        }

        if (kind == GFX_POINTER_GESTURE_DRAG_END || kind == GFX_POINTER_GESTURE_UP) {
            if (ctx->pointer_drag_button == button) {
                ctx->pointer_drag_button = 0;
            }
            return UI_MSG_CONSUMED;
        }

        return UI_MSG_CONSUMED;
    }

    if (msg->arg1 == GFX_EVENT_KEY) {
        const uint32_t key = (uint32_t)msg->arg2;
        const uint32_t flags = (uint32_t)msg->arg3;
        const int32_t key_down = ((flags & 1u) != 0u);
        if (!key_down)
            return UI_MSG_CONSUMED;

        if (ctx->focused_component_id > 0) {
            ui_component_t* focus = ui_component_by_id(ctx, ctx->focused_component_id);
            if (focus) {
                const ui_component_ops_t* ops = &ui_component_ops[focus->type];
                if (ops->handle_key) {
                    ops->handle_key(ctx, focus, key);
                }
            }
        }
        return UI_MSG_CONSUMED;
    }

    if (msg->arg1 == GFX_EVENT_FOCUS_GAINED || msg->arg1 == GFX_EVENT_FOCUS_LOST) {
        return UI_MSG_CONSUMED;
    }

    return UI_MSG_IGNORED;
}

/* Block until the compositor pushes an event on the event endpoint, then
 * dispatch it. The thread sleeps in the kernel until a real event arrives, so
 * an idle UI does not spin the scheduler. Returns the ui_loop_handle_ipc
 * result, or UI_MSG_IGNORED when the wait itself failed (invalid endpoint or
 * receive error; the hostcall already retries spurious wakes internally). */
static inline int32_t ui_wait_and_handle(ui_context_t* ctx) {
    wasmos_ipc_message_t msg;
    if (!ctx)
        return UI_MSG_ERROR;
    if (wasmos_ipc_select_one(ctx->event_endpoint) != 1)
        return UI_MSG_IGNORED;
    wasmos_ipc_message_read_last(&msg);
    return ui_loop_handle_ipc(ctx, &msg);
}

/* Repaint if the tree is dirty: sync menu popup windows, lay out, render into
 * the shared buffer, flush it, and present. A no-op returning 0 while nothing
 * is dirty. Returns -1 when a step fails; a present refused with GFX_INVALID or
 * GFX_BUSY is not a failure — the window resized underneath this frame and the
 * pending RESIZE event will drive the next one. */
static inline int32_t ui_loop_drain(ui_context_t* ctx) {
    int32_t status = 0;
    if (!ctx)
        return -1;
    if (!ctx->dirty)
        return 0;
    ui_component_t* root = ui_component_by_id(ctx, ctx->root_id);
    if (!root)
        return -1;

    /* Sync popup windows for all menu items (open/close/resize on state change). */
    for (int32_t i = 0; i < ctx->component_count; ++i) {
        ui_component_t* mc = &ctx->components[i];
        if (mc->in_use && mc->type == UI_COMPONENT_MENU_ITEM)
            ui_menu_item_sync_popup(ctx, mc);
    }

    ui_layout_vertical(ctx, root->id);
    ui_render_component(ctx, root->id);

    if (wasmos_shmem_flush(ctx->shmem_id,
                           addr_cast(int32_t, ctx->mapped_base),
                           ctx->stride_bytes * ctx->height) != 0) {
        return -1;
    }

    if (ui_send_gfx(ctx->gfx_endpoint,
                    ctx->reply_endpoint,
                    ctx->req_id++,
                    GFX_IPC_PRESENT_WINDOW,
                    ctx->window_id,
                    ctx->buffer_id,
                    0,
                    0,
                    &status,
                    0,
                    0,
                    0) != 0) {
        return -1;
    }
    if (status == WASMOS_ERR_GFX_INVALID || status == WASMOS_ERR_GFX_BUSY) {
        /* Window resized between render and present — RESIZE event is incoming. */
        ctx->dirty = 0;
        return 0;
    }
    if (status != WASMOS_ERR_NONE) {
        return -1;
    }

    ctx->dirty = 0;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
