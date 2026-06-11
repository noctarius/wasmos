#ifndef WASMOS_LIBUI_H
#define WASMOS_LIBUI_H

#include <stdint.h>
#include <stddef.h>
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

void *malloc(size_t size);
void free(void *ptr);

#define UI_PAGE_SIZE 4096
#define UI_REQ_BASE 0x7400
#define UI_COMPONENTS_INITIAL_CAP 16
#define UI_TEXT_INITIAL_CAP 32
#define UI_LIST_INITIAL_CAP 8

enum {
    UI_MSG_ERROR = -1,
    UI_MSG_IGNORED = 0,
    UI_MSG_CONSUMED = 1
};

typedef enum {
    UI_COMPONENT_NONE = 0,
    UI_COMPONENT_PANEL = 1,
    UI_COMPONENT_LABEL = 2,
    UI_COMPONENT_BUTTON = 3,
    UI_COMPONENT_CHECKBOX = 4,
    UI_COMPONENT_TEXT_INPUT = 5,
    UI_COMPONENT_SCROLL_VIEW = 6,
    UI_COMPONENT_LIST_VIEW = 7,
    UI_COMPONENT_DROPDOWN = 8,
    UI_COMPONENT_MENU_BAR = 9,
    UI_COMPONENT_MENU_ITEM = 10
} ui_component_type_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} ui_rect_t;

struct ui_context;
typedef void (*ui_button_click_cb_t)(struct ui_context *ctx, int32_t component_id, void *user);

/* Pure base component. All type-specific state lives in component_data.
 * This keeps the core struct small and stable as we add more widget kinds. */
typedef struct {
    int32_t in_use;
    int32_t id;
    int32_t parent_id;
    int32_t first_child_id;
    int32_t next_sibling_id;
    ui_component_type_t type;
    ui_rect_t bounds;
    int32_t preferred_h;
    uint32_t bg_color;
    uint32_t fg_color;
    uint32_t border_color;
    int32_t border_px;
    int32_t padding_px;
    int32_t gap_px;
    int32_t clickable;
    int32_t pressed;

    ui_button_click_cb_t on_click;
    void *on_click_user;

    void *component_data;   /* owned per-type data (see libui_*.h for the concrete structs) */
} ui_component_t;

/* Vtable for component behavior. Components register their implementations
 * so the core can dispatch without giant type switches.
 * Use struct tags to avoid typedef ordering issues (declared before full ui_context_t). */
typedef struct {
    void (*render)(struct ui_context *ctx, const ui_component_t *c,
                   ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y);
    void (*layout)(struct ui_context *ctx, ui_component_t *c);

    void (*handle_pointer_press)(struct ui_context *ctx, ui_component_t *c,
                                 int32_t x, int32_t y);
    void (*handle_pointer_release)(struct ui_context *ctx, ui_component_t *c);
    void (*handle_key)(struct ui_context *ctx, ui_component_t *c, uint32_t key);
    void (*handle_scroll_drag)(struct ui_context *ctx, ui_component_t *c, int32_t dy);

    /* Optional: for hit-testing popups/overlays owned by the component. */
    bool (*popup_contains)(const struct ui_context *ctx, const ui_component_t *c,
                           int32_t x, int32_t y);

    /* Free the component_data (and anything it owns). */
    void (*destroy_data)(ui_component_t *c);
} ui_component_ops_t;

/* Populated after all component headers are included. */
static ui_component_ops_t ui_component_ops[UI_COMPONENT_MENU_ITEM + 1];

/* Prototype so it can be called from ui_init / ui_menu_bar_init (defined after the component includes). */
static inline void ui_init_component_ops(void);

/* Small reusable data blocks for common component aspects.
 * Individual component headers may use these directly as their component_data
 * or define richer per-type structs that contain them. */
typedef struct {
    char *text;
    int32_t text_len;
    int32_t text_cap;
} ui_text_data_t;

typedef struct {
    char **items;
    int32_t count;
    int32_t capacity;
    int32_t selected;
} ui_list_data_t;

/* Per-type component data structs. ui_component_alloc() allocates the correct
 * struct eagerly so all code can cast component_data without size mismatches. */
typedef struct {
    ui_text_data_t text;
    int32_t checked;
} ui_checkbox_data_t;

typedef struct {
    int32_t scroll_y;
    int32_t scroll_max;
} ui_scroll_view_data_t;

typedef struct {
    ui_list_data_t list;
    int32_t scroll_y;
    int32_t scroll_max;
} ui_list_view_data_t;

typedef struct {
    ui_text_data_t text;
    ui_list_data_t list;
    int32_t dropdown_open;
} ui_dropdown_data_t;

typedef struct {
    ui_text_data_t text;
    ui_list_data_t list;
    int32_t dropdown_open;
} ui_menu_item_data_t;

typedef struct {
    char clock_text[24]; /* "YYYY-MM-DD HH:MM:SS\0" or empty */
} ui_menu_bar_data_t;

typedef struct ui_context {
    int32_t proc_endpoint;
    int32_t reply_endpoint;
    int32_t gfx_endpoint;
    int32_t req_id;
    int32_t window_id;
    int32_t width;
    int32_t height;
    int32_t stride_bytes;
    int32_t buffer_id;
    int32_t shmem_id;
    uint8_t *mapped_base;
    int32_t pointer_x;
    int32_t pointer_y;
    uint32_t pointer_buttons;
    int32_t dirty;
    int32_t close_requested;
    int32_t root_id;
    int32_t focused_component_id;
    int32_t active_scroll_component_id;
    int32_t font_reply_endpoint;
    int32_t font_endpoint;
    int32_t font_handle;
    int32_t font_px;
    int32_t font_text_shmem_id;
    uint8_t *font_text_ptr;
    int32_t font_text_cap;
    int32_t font_mask_shmem_id;
    uint8_t *font_mask_ptr;
    int32_t font_mask_cap;

    int32_t next_component_id;
    ui_component_t *components;
    int32_t component_count;
    int32_t component_capacity;
} ui_context_t;

static inline int32_t ui_u16_lo(int32_t packed) { return (packed & 0xFFFF); }
static inline int32_t ui_u16_hi(int32_t packed) { return ((packed >> 16) & 0xFFFF); }
static inline int32_t ui_i16_lo(int32_t packed) { return (int16_t)(packed & 0xFFFF); }
static inline int32_t ui_i16_hi(int32_t packed) { return (int16_t)((packed >> 16) & 0xFFFF); }

static inline void ui_mark_dirty(ui_context_t *ctx) { if (ctx) ctx->dirty = 1; }

static inline void
ui_fill_rect(uint8_t *base, int32_t bw, int32_t bh, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (!base || bw <= 0 || bh <= 0 || w <= 0 || h <= 0) return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    if (x0 >= x1 || y0 >= y1) return;
    const int32_t stride_px = bw;
    for (int32_t yy = y0; yy < y1; ++yy) {
        uint32_t *row = (uint32_t *)(void *)(base + (yy * stride_px * 4));
        for (int32_t xx = x0; xx < x1; ++xx) {
            row[xx] = color;
        }
    }
}

static inline ui_rect_t
ui_rect_intersect(ui_rect_t a, ui_rect_t b)
{
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
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static inline void
ui_fill_rect_clip(uint8_t *base, int32_t bw, int32_t bh,
                  int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color, ui_rect_t clip)
{
    ui_rect_t r = { x, y, w, h };
    ui_rect_t i = ui_rect_intersect(r, clip);
    if (i.w <= 0 || i.h <= 0) return;
    ui_fill_rect(base, bw, bh, i.x, i.y, i.w, i.h, color);
}

static inline void
ui_stroke_rect_clip(uint8_t *base, int32_t bw, int32_t bh, ui_rect_t r, int32_t border_px, uint32_t color, ui_rect_t clip)
{
    if (border_px <= 0) return;
    ui_fill_rect_clip(base, bw, bh, r.x, r.y, r.w, border_px, color, clip);
    ui_fill_rect_clip(base, bw, bh, r.x, r.y + r.h - border_px, r.w, border_px, color, clip);
    ui_fill_rect_clip(base, bw, bh, r.x, r.y, border_px, r.h, color, clip);
    ui_fill_rect_clip(base, bw, bh, r.x + r.w - border_px, r.y, border_px, r.h, color, clip);
}

static inline uint32_t
ui_blend_u8(uint32_t dst, uint32_t src, uint8_t alpha)
{
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

static inline int32_t
ui_utf8_encode(uint32_t cp, uint8_t out[4])
{
    if (!out) return 0;
    if (cp <= 0x7Fu) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;
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

static inline int32_t
ui_utf8_prev_boundary(const char *s, int32_t len)
{
    if (!s || len <= 0) return 0;
    int32_t i = len - 1;
    while (i > 0) {
        const uint8_t b = (uint8_t)s[i];
        if ((b & 0xC0u) != 0x80u) break;
        i -= 1;
    }
    return i;
}

static inline int32_t
ui_font_ensure_shmem_buffer(int32_t *shmem_id, uint8_t **mapped_ptr, int32_t *cap, int32_t need_bytes)
{
    if (!shmem_id || !mapped_ptr || !cap || need_bytes <= 0) return -1;
    if (*shmem_id > 0 && *mapped_ptr && *cap >= need_bytes) return 0;
    const int32_t pages = (need_bytes + (UI_PAGE_SIZE - 1)) / UI_PAGE_SIZE;
    const int32_t bytes = pages * UI_PAGE_SIZE;
    const int32_t new_id = wasmos_shmem_create(pages, 0);
    if (new_id <= 0) return -1;
    const int32_t mapped = wasmos_shmem_map_auto(new_id, bytes);
    if (mapped < 0) return -1;
    /* TODO(libui-font-shmem): old SHMEM IDs are not reclaimed on growth. */
    *shmem_id = new_id;
    *mapped_ptr = (uint8_t *)(uintptr_t)(uint32_t)mapped;
    *cap = bytes;
    return 0;
}

static inline int32_t
ui_font_measure_text(ui_context_t *ctx, const char *text, int32_t *out_w, int32_t *out_h, int32_t *out_x0, int32_t *out_y0, int32_t *out_adv)
{
    if (!ctx || !text || ctx->font_endpoint <= 0 || ctx->font_reply_endpoint <= 0 || ctx->font_handle <= 0) return -1;
    const int32_t text_len = (int32_t)strlen(text);
    if (text_len <= 0) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        if (out_x0) *out_x0 = 0;
        if (out_y0) *out_y0 = 0;
        if (out_adv) *out_adv = 0;
        return 0;
    }
    if (ui_font_ensure_shmem_buffer(&ctx->font_text_shmem_id, &ctx->font_text_ptr, &ctx->font_text_cap, text_len + 1) != 0) return -1;
    memcpy(ctx->font_text_ptr, text, (size_t)text_len);
    ctx->font_text_ptr[text_len] = '\0';
    if (wasmos_shmem_flush(ctx->font_text_shmem_id, (int32_t)(uintptr_t)ctx->font_text_ptr, text_len + 1) != 0) return -1;

    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(ctx->font_endpoint, ctx->font_reply_endpoint, FONT_IPC_MEASURE_GLYPH_REQ,
                        ctx->req_id++, ctx->font_handle, ctx->font_text_shmem_id, text_len, 0, &reply) != 0) {
        return -1;
    }
    if (reply.type != FONT_IPC_RESP || reply.arg0 != FONT_STATUS_OK) return -1;
    if (out_w) *out_w = ui_u16_lo(reply.arg1);
    if (out_h) *out_h = ui_u16_hi(reply.arg1);
    if (out_x0) *out_x0 = ui_i16_lo(reply.arg2);
    if (out_y0) *out_y0 = ui_i16_hi(reply.arg2);
    if (out_adv) *out_adv = reply.arg3;
    return 0;
}

static inline int32_t
ui_font_measure_and_raster_text(ui_context_t *ctx, const char *text, int32_t text_len,
                                int32_t *out_w, int32_t *out_h, int32_t *out_x0, int32_t *out_y0, int32_t *out_adv)
{
    if (!ctx || !text || text_len <= 0 || ctx->font_endpoint <= 0 || ctx->font_reply_endpoint <= 0 || ctx->font_handle <= 0) return -1;
    if (ui_font_measure_text(ctx, text, out_w, out_h, out_x0, out_y0, out_adv) != 0) return -1;
    if (!out_w || !out_h) return -1;
    if (*out_w <= 0 || *out_h <= 0) return 0;

    const int32_t bytes = (*out_w) * (*out_h);
    if (bytes <= 0) return -1;
    if (ui_font_ensure_shmem_buffer(&ctx->font_mask_shmem_id, &ctx->font_mask_ptr, &ctx->font_mask_cap, bytes) != 0) return -1;

    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(ctx->font_endpoint, ctx->font_reply_endpoint, FONT_IPC_RASTER_GLYPH_INTO_REQ,
                        ctx->req_id++, ctx->font_handle, ctx->font_text_shmem_id, text_len, ctx->font_mask_shmem_id, &reply) != 0) {
        return -1;
    }
    if (reply.type != FONT_IPC_RESP || reply.arg0 != FONT_STATUS_OK) return -1;
    if (wasmos_shmem_refresh(ctx->font_mask_shmem_id, (int32_t)(uintptr_t)ctx->font_mask_ptr, bytes) != 0) return -1;
    return 0;
}

static inline void
ui_draw_text_clip(ui_context_t *ctx, int32_t x, int32_t y, const char *text, uint32_t color, ui_rect_t clip)
{
    if (!ctx || !ctx->mapped_base || !text || ctx->font_endpoint <= 0 || ctx->font_reply_endpoint <= 0 || ctx->font_handle <= 0) return;
    const int32_t text_len = (int32_t)strlen(text);
    if (text_len <= 0) return;
    int32_t w = 0, h = 0, x0 = 0, y0 = 0, adv = 0;
    if (ui_font_measure_and_raster_text(ctx, text, text_len, &w, &h, &x0, &y0, &adv) != 0) return;
    if (w <= 0 || h <= 0) return;
    if (!ctx->font_mask_ptr) return;

    const uint8_t *mask = ctx->font_mask_ptr;
    for (int32_t gy = 0; gy < h; ++gy) {
        const int32_t py = y + gy;
        if (py < clip.y || py >= (clip.y + clip.h) || py < 0 || py >= ctx->height) continue;
        uint32_t *row = (uint32_t *)(void *)(ctx->mapped_base + ((size_t)py * (size_t)ctx->width * 4u));
        for (int32_t gx = 0; gx < w; ++gx) {
            const int32_t px = x + gx;
            if (px < clip.x || px >= (clip.x + clip.w) || px < 0 || px >= ctx->width) continue;
            const uint8_t a = mask[gy * w + gx];
            if (a == 0) continue;
            row[px] = ui_blend_u8(row[px], color, a);
        }
    }
}

static inline int32_t
ui_measure_text_width(ui_context_t *ctx, const char *text)
{
    int32_t w = 0, h = 0, x0 = 0, y0 = 0, adv = 0;
    if (ui_font_measure_text(ctx, text, &w, &h, &x0, &y0, &adv) != 0) return 0;
    return adv;
}

static inline int32_t
ui_send_gfx_raw(int32_t gfx_ep,
                int32_t reply_ep,
                int32_t req_id,
                int32_t opcode,
                int32_t arg0,
                int32_t arg1,
                int32_t arg2,
                int32_t arg3,
                wasmos_ipc_message_t *out_raw)
{
    if (!out_raw) return -1;
    if (wasmos_ipc_call(gfx_ep, reply_ep, opcode, req_id, arg0, arg1, arg2, arg3, out_raw) != 0) {
        return -1;
    }
    if (out_raw->type != GFX_IPC_RESP && out_raw->type != GFX_IPC_ERROR) {
        return -1;
    }
    return 0;
}

static inline int32_t
ui_send_gfx(int32_t gfx_ep,
            int32_t reply_ep,
            int32_t req_id,
            int32_t opcode,
            int32_t arg0,
            int32_t arg1,
            int32_t arg2,
            int32_t arg3,
            int32_t *out_status,
            int32_t *out_a1,
            int32_t *out_a2,
            int32_t *out_a3)
{
    wasmos_ipc_message_t msg;
    if (ui_send_gfx_raw(gfx_ep, reply_ep, req_id, opcode, arg0, arg1, arg2, arg3, &msg) != 0) {
        return -1;
    }
    if (out_status) *out_status = msg.arg0;
    if (out_a1) *out_a1 = msg.arg1;
    if (out_a2) *out_a2 = msg.arg2;
    if (out_a3) *out_a3 = msg.arg3;
    return 0;
}

static inline ui_component_t *
ui_component_by_id(ui_context_t *ctx, int32_t id)
{
    if (!ctx || id <= 0) return 0;
    for (int32_t i = 0; i < ctx->component_count; ++i) {
        if (ctx->components[i].in_use && ctx->components[i].id == id) return &ctx->components[i];
    }
    return 0;
}

static inline int32_t
ui_component_set_text_owned(ui_component_t *c, const char *text)
{
    if (!c) return -1;
    if (!text) text = "";
    const int32_t need = (int32_t)strlen(text) + 1;
    if (need <= 0) return -1;

    /* component_data is always pre-allocated by ui_component_alloc; text is the first
     * field for all text-bearing types so the cast is always valid at offset 0. */
    ui_text_data_t *td = (ui_text_data_t *)c->component_data;
    if (!td) return -1;

    if (td->text_cap < need) {
        int32_t new_cap = td->text_cap > 0 ? td->text_cap : UI_TEXT_INITIAL_CAP;
        while (new_cap < need) new_cap *= 2;
        char *new_text = (char *)malloc((size_t)new_cap);
        if (!new_text) return -1;
        if (td->text) memcpy(new_text, td->text, (size_t)td->text_len + 1);
        if (td->text) free(td->text);
        td->text = new_text;
        td->text_cap = new_cap;
    }
    memcpy(td->text, text, (size_t)need);
    td->text_len = need - 1;
    return 0;
}

static inline int32_t
ui_components_reserve(ui_context_t *ctx, int32_t target)
{
    if (!ctx || target <= ctx->component_capacity) return 0;
    int32_t cap = ctx->component_capacity > 0 ? ctx->component_capacity : UI_COMPONENTS_INITIAL_CAP;
    while (cap < target) cap *= 2;
    ui_component_t *new_arr = (ui_component_t *)malloc((size_t)cap * sizeof(ui_component_t));
    if (!new_arr) return -1;
    if (ctx->components && ctx->component_count > 0) {
        memcpy(new_arr, ctx->components, (size_t)ctx->component_count * sizeof(ui_component_t));
        free(ctx->components);
    }
    ctx->components = new_arr;
    ctx->component_capacity = cap;
    return 0;
}

static inline int32_t
ui_component_alloc(ui_context_t *ctx, ui_component_type_t type)
{
    if (!ctx) return -1;
    if (ui_components_reserve(ctx, ctx->component_count + 1) != 0) return -1;
    ui_component_t *c = &ctx->components[ctx->component_count];
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

#define UI_ALLOC_DATA(T) do { \
    T *_d = (T *)malloc(sizeof(T)); \
    if (!_d) return -1; \
    memset(_d, 0, sizeof(T)); \
    c->component_data = _d; \
} while (0)

    switch (type) {
    case UI_COMPONENT_LABEL:
    case UI_COMPONENT_BUTTON:
    case UI_COMPONENT_TEXT_INPUT: UI_ALLOC_DATA(ui_text_data_t);     break;
    case UI_COMPONENT_CHECKBOX:   UI_ALLOC_DATA(ui_checkbox_data_t); break;
    case UI_COMPONENT_SCROLL_VIEW: UI_ALLOC_DATA(ui_scroll_view_data_t); break;
    case UI_COMPONENT_LIST_VIEW:
        UI_ALLOC_DATA(ui_list_view_data_t);
        ((ui_list_view_data_t *)c->component_data)->list.selected = -1;
        break;
    case UI_COMPONENT_DROPDOWN:
        UI_ALLOC_DATA(ui_dropdown_data_t);
        ((ui_dropdown_data_t *)c->component_data)->list.selected = -1;
        break;
    case UI_COMPONENT_MENU_ITEM:
        UI_ALLOC_DATA(ui_menu_item_data_t);
        ((ui_menu_item_data_t *)c->component_data)->list.selected = -1;
        break;
    case UI_COMPONENT_MENU_BAR:   UI_ALLOC_DATA(ui_menu_bar_data_t);   break;
    default: break; /* PANEL needs no per-instance data */
    }
#undef UI_ALLOC_DATA

    ctx->component_count++;
    return c->id;
}

static inline int32_t
ui_component_append_child(ui_context_t *ctx, int32_t parent_id, int32_t child_id)
{
    ui_component_t *parent = ui_component_by_id(ctx, parent_id);
    ui_component_t *child = ui_component_by_id(ctx, child_id);
    if (!parent || !child || parent_id == child_id) return -1;
    child->parent_id = parent_id;
    child->next_sibling_id = 0;
    if (parent->first_child_id == 0) {
        parent->first_child_id = child_id;
        return 0;
    }
    int32_t cur_id = parent->first_child_id;
    while (cur_id > 0) {
        ui_component_t *cur = ui_component_by_id(ctx, cur_id);
        if (!cur) return -1;
        if (cur->next_sibling_id == 0) {
            cur->next_sibling_id = child_id;
            return 0;
        }
        cur_id = cur->next_sibling_id;
    }
    return -1;
}

static inline int32_t ui_component_create_panel(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_PANEL); }
static inline int32_t ui_component_create_label(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_LABEL); }
static inline int32_t ui_component_create_button(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_BUTTON); }
static inline int32_t ui_component_create_checkbox(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_CHECKBOX); }
static inline int32_t ui_component_create_text_input(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_TEXT_INPUT); }
static inline int32_t ui_component_create_scroll_view(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_SCROLL_VIEW); }
static inline int32_t ui_component_create_list_view(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_LIST_VIEW); }
static inline int32_t ui_component_create_dropdown(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_DROPDOWN); }
static inline int32_t ui_component_create_menu_bar(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_MENU_BAR); }
static inline int32_t ui_component_create_menu_item(ui_context_t *ctx) { return ui_component_alloc(ctx, UI_COMPONENT_MENU_ITEM); }

static inline void
ui_component_set_text(ui_context_t *ctx, int32_t id, const char *text)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return;
    (void)ui_component_set_text_owned(c, text ? text : "");
}

static inline void
ui_component_set_button_action(ui_context_t *ctx, int32_t id, ui_button_click_cb_t cb, void *user)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return;
    c->clickable = 1;
    c->on_click = cb;
    c->on_click_user = user;
}

static inline void
ui_component_set_checked(ui_context_t *ctx, int32_t id, int32_t checked)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c || c->type != UI_COMPONENT_CHECKBOX || !c->component_data) return;
    ((ui_checkbox_data_t *)c->component_data)->checked = checked ? 1 : 0;
}

static inline int32_t
ui_component_get_checked(const ui_component_t *c)
{
    if (!c || c->type != UI_COMPONENT_CHECKBOX || !c->component_data) return 0;
    return ((ui_checkbox_data_t *)c->component_data)->checked;
}

static inline int32_t
ui_component_list_append(ui_context_t *ctx, int32_t id, const char *item)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c || !item || !c->component_data) return -1;

    ui_list_data_t *ld;
    if (c->type == UI_COMPONENT_LIST_VIEW) {
        ld = &((ui_list_view_data_t *)c->component_data)->list;
    } else if (c->type == UI_COMPONENT_DROPDOWN) {
        ld = &((ui_dropdown_data_t *)c->component_data)->list;
    } else if (c->type == UI_COMPONENT_MENU_ITEM) {
        ld = &((ui_menu_item_data_t *)c->component_data)->list;
    } else {
        return -1;
    }

    if (ld->count >= ld->capacity) {
        int32_t cap = ld->capacity > 0 ? ld->capacity : UI_LIST_INITIAL_CAP;
        while (cap <= ld->count) cap *= 2;
        char **new_items = (char **)malloc((size_t)cap * sizeof(char *));
        if (!new_items) return -1;
        for (int32_t i = 0; i < cap; ++i) new_items[i] = 0;
        if (ld->items && ld->count > 0) {
            memcpy(new_items, ld->items, (size_t)ld->count * sizeof(char *));
            free(ld->items);
        }
        ld->items = new_items;
        ld->capacity = cap;
    }
    const size_t len = strlen(item) + 1;
    char *copy = (char *)malloc(len);
    if (!copy) return -1;
    memcpy(copy, item, len);
    ld->items[ld->count] = copy;
    ld->count += 1;
    return ld->count - 1;
}

static inline int32_t
ui_component_text_len(const ui_component_t *c)
{
    if (!c || !c->component_data) return 0;
    /* For components that use ui_text_data_t as (or starting as) their data. */
    ui_text_data_t *td = (ui_text_data_t *)c->component_data;
    return td->text_len;
}

static inline int32_t
ui_realloc_buffer(ui_context_t *ctx, int32_t new_w, int32_t new_h)
{
    int32_t status = 0, new_buffer_id = 0, new_shmem_id = 0, new_stride = 0;
    if (!ctx || new_w <= 0 || new_h <= 0) return -1;
    if (ctx->width == new_w && ctx->height == new_h && ctx->mapped_base) return 0;
    const int32_t prev_ptr_x = ctx->pointer_x;
    const int32_t prev_ptr_y = ctx->pointer_y;
    const int32_t first_alloc = (ctx->mapped_base == NULL);
    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_ALLOC_SHARED_BUFFER,
                    ctx->window_id, new_w, new_h, 0,
                    &status, &new_buffer_id, &new_shmem_id, &new_stride) != 0 || status != GFX_STATUS_OK) {
        return -1;
    }
    const int32_t bytes = (new_stride * new_h + (UI_PAGE_SIZE - 1)) & ~(UI_PAGE_SIZE - 1);
    const int32_t mapped_ptr = wasmos_shmem_map_auto(new_shmem_id, bytes);
    if (mapped_ptr < 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_RELEASE_SHARED_BUFFER,
                          new_buffer_id, 0, 0, 0, &status, 0, 0, 0);
        return -1;
    }
    if (ctx->shmem_id > 0) (void)wasmos_shmem_unmap(ctx->shmem_id);
    if (ctx->buffer_id > 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_RELEASE_SHARED_BUFFER,
                          ctx->buffer_id, 0, 0, 0, &status, 0, 0, 0);
    }
    ctx->buffer_id = new_buffer_id;
    ctx->shmem_id = new_shmem_id;
    ctx->stride_bytes = new_stride;
    ctx->width = new_w;
    ctx->height = new_h;
    ctx->mapped_base = (uint8_t *)(uintptr_t)(uint32_t)mapped_ptr;
    ctx->pointer_x = first_alloc ? ctx->width / 2 : prev_ptr_x;
    ctx->pointer_y = first_alloc ? ctx->height / 2 : prev_ptr_y;
    return 0;
}

static inline void
ui_destroy(ui_context_t *ctx);

static inline int32_t
ui_init_font(ui_context_t *ctx)
{
    wasmos_ipc_message_t reply;
    if (!ctx) return -1;
    ctx->font_reply_endpoint = wasmos_ipc_create_endpoint();
    if (ctx->font_reply_endpoint <= 0) return -1;
    for (int32_t spins = 0; spins < 2048; ++spins) {
        ctx->font_endpoint = wasmos_svc_lookup(ctx->proc_endpoint, ctx->font_reply_endpoint, "font", ctx->req_id++);
        if (ctx->font_endpoint >= 0) break;
        (void)wasmos_sched_yield();
    }
    if (ctx->font_endpoint < 0) return -1;
    if (wasmos_ipc_call(ctx->font_endpoint, ctx->font_reply_endpoint, FONT_IPC_OPEN_FONT_REQ,
                        ctx->req_id++, FONT_ID_ROBOTO, ctx->font_px, 0, 0, &reply) != 0) return -1;
    if (reply.type != FONT_IPC_RESP || reply.arg0 != FONT_STATUS_OK || reply.arg1 <= 0) return -1;
    ctx->font_handle = reply.arg1;
    return 0;
}

static inline int32_t
ui_init(ui_context_t *ctx, int32_t proc_endpoint, int32_t reply_endpoint, int32_t width, int32_t height)
{
    int32_t status = 0;
    int32_t a1 = 0;
    int32_t a2 = 0;
    int32_t a3 = 0;
    if (!ctx || proc_endpoint <= 0 || reply_endpoint < 0 || width <= 0 || height <= 0) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->proc_endpoint = proc_endpoint;
    ctx->reply_endpoint = reply_endpoint;
    ctx->req_id = UI_REQ_BASE;
    ctx->font_px = 14;
    ctx->next_component_id = 1;
    if (ui_components_reserve(ctx, UI_COMPONENTS_INITIAL_CAP) != 0) goto fail;

    for (int32_t spins = 0; spins < 2048; ++spins) {
        ctx->gfx_endpoint = wasmos_svc_lookup(proc_endpoint, reply_endpoint, "gfx", ctx->req_id++);
        if (ctx->gfx_endpoint >= 0) break;
        (void)wasmos_sched_yield();
    }
    if (ctx->gfx_endpoint < 0) goto fail;
    if (ui_init_font(ctx) != 0) goto fail;

    if (ui_send_gfx(ctx->gfx_endpoint, reply_endpoint, ctx->req_id++, GFX_IPC_CREATE_WINDOW,
                    width, height, (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status, &a1, &a2, &a3) != 0 || status != GFX_STATUS_OK) {
        goto fail;
    }
    ctx->window_id = a1;
    if (ui_realloc_buffer(ctx, width, height) != 0) goto fail;

    ui_init_component_ops();

    ctx->root_id = ui_component_create_panel(ctx);
    if (ctx->root_id < 0) goto fail;
    ui_component_t *root = ui_component_by_id(ctx, ctx->root_id);
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

static inline int32_t
ui_menu_bar_init(ui_context_t *ctx, int32_t proc_endpoint, int32_t reply_endpoint)
{
    int32_t status = 0, a1 = 0, a2 = 0, a3 = 0;
    if (!ctx || proc_endpoint <= 0 || reply_endpoint < 0) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->proc_endpoint = proc_endpoint;
    ctx->reply_endpoint = reply_endpoint;
    ctx->req_id = UI_REQ_BASE;
    ctx->font_px = 13;
    ctx->next_component_id = 1;
    if (ui_components_reserve(ctx, UI_COMPONENTS_INITIAL_CAP) != 0) goto mb_fail;

    for (int32_t spins = 0; spins < 2048; ++spins) {
        ctx->gfx_endpoint = wasmos_svc_lookup(proc_endpoint, reply_endpoint, "gfx", ctx->req_id++);
        if (ctx->gfx_endpoint >= 0) break;
        (void)wasmos_sched_yield();
    }
    if (ctx->gfx_endpoint < 0) goto mb_fail;
    if (ui_init_font(ctx) != 0) goto mb_fail;

    if (ui_send_gfx(ctx->gfx_endpoint, reply_endpoint, ctx->req_id++, GFX_IPC_GET_DISPLAY_INFO,
                    0, 0, 0, 0, &status, &a1, &a2, &a3) != 0 || status != GFX_STATUS_OK || a1 <= 0) goto mb_fail;
    {
        const int32_t screen_w = a1;
        const int32_t bar_h = 28;

        if (ui_send_gfx(ctx->gfx_endpoint, reply_endpoint, ctx->req_id++, GFX_IPC_CREATE_WINDOW,
                        screen_w, bar_h, (int32_t)GFX_IPC_ABI_MAGIC,
                        (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                        &status, &a1, &a2, &a3) != 0 || status != GFX_STATUS_OK) goto mb_fail;
        ctx->window_id = a1;

        if (ui_send_gfx(ctx->gfx_endpoint, reply_endpoint, ctx->req_id++, GFX_IPC_SET_WINDOW_FLAGS,
                        ctx->window_id, (int32_t)(GFX_WINDOW_FLAG_TOPMOST | GFX_WINDOW_FLAG_NO_CHROME), 0, 0,
                        &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) goto mb_fail;

        if (ui_realloc_buffer(ctx, screen_w, bar_h) != 0) goto mb_fail;

        ui_init_component_ops();
        ctx->root_id = ui_component_create_menu_bar(ctx);
        if (ctx->root_id < 0) goto mb_fail;
        {
            ui_component_t *mbroot = ui_component_by_id(ctx, ctx->root_id);
            mbroot->bounds.x = 0;
            mbroot->bounds.y = 0;
            mbroot->bounds.w = screen_w;
            mbroot->bounds.h = bar_h;
            mbroot->padding_px = 2;
            mbroot->gap_px = 0;
            mbroot->bg_color = 0xFF1A2233u;
        }
        ctx->dirty = 1;
        return 0;
    }
mb_fail:
    ui_destroy(ctx);
    return -1;
}

static inline void
ui_destroy(ui_context_t *ctx)
{
    int32_t status = 0;
    if (!ctx) return;
    if (ctx->window_id > 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_DESTROY_WINDOW,
                          ctx->window_id, 0, 0, 0, &status, 0, 0, 0);
    }
    if (ctx->buffer_id > 0) {
        (void)ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_RELEASE_SHARED_BUFFER,
                          ctx->buffer_id, 0, 0, 0, &status, 0, 0, 0);
    }
    if (ctx->shmem_id > 0) (void)wasmos_shmem_unmap(ctx->shmem_id);
    if (ctx->font_text_shmem_id > 0) (void)wasmos_shmem_unmap(ctx->font_text_shmem_id);
    if (ctx->font_mask_shmem_id > 0) (void)wasmos_shmem_unmap(ctx->font_mask_shmem_id);

    for (int32_t i = 0; i < ctx->component_count; ++i) {
        ui_component_t *c = &ctx->components[i];
        const ui_component_ops_t *ops = &ui_component_ops[c->type];
        if (ops->destroy_data) {
            ops->destroy_data(c);
        } else if (c->component_data) {
            free(c->component_data);
            c->component_data = NULL;
        }
    }
    if (ctx->components) free(ctx->components);
    memset(ctx, 0, sizeof(*ctx));
}

static inline int32_t
ui_point_in_bounds(int32_t x, int32_t y, ui_rect_t r)
{
    return x >= r.x && y >= r.y && x < (r.x + r.w) && y < (r.y + r.h);
}

/* Popup bounds moved to component headers (dropdown + menu_item). */

/* Layout prototype so component headers (inserted below) can call back for child recursion in containers. */
static inline void ui_layout_vertical(ui_context_t *ctx, int32_t parent_id);

/* Prototypes for main renderer so component headers can call back (e.g. scroll_view and
 * menu_bar render their children via the core clip walker; menu_item uses its own helpers). */
static inline void ui_render_component_clip(ui_context_t *ctx, int32_t id, ui_rect_t clip, int32_t offset_y);
static inline void ui_render_component(ui_context_t *ctx, int32_t id);

/* Component headers (render + now layout too) are included here so their per-type functions
 * are visible to the layout and render dispatchers below. */
#include "wasmos/libui_label.h"
#include "wasmos/libui_button.h"
#include "wasmos/libui_checkbox.h"
#include "wasmos/libui_text_input.h"
#include "wasmos/libui_list_view.h"
#include "wasmos/libui_dropdown.h"
#include "wasmos/libui_scroll_view.h"
#include "wasmos/libui_menu_bar.h"
#include "wasmos/libui_menu_item.h"

/* Register vtable implementations owned by the component headers.
 * This replaces the previous giant type switches with vtable dispatch. */
static inline void ui_init_component_ops(void)
{
    /* zero the table */
    memset(ui_component_ops, 0, sizeof(ui_component_ops));

    /* label */
    ui_component_ops[UI_COMPONENT_LABEL].render = ui_render_label;

    /* button */
    ui_component_ops[UI_COMPONENT_BUTTON].render = ui_render_button;
    ui_component_ops[UI_COMPONENT_BUTTON].handle_pointer_release = ui_button_handle_pointer_release;

    /* checkbox */
    ui_component_ops[UI_COMPONENT_CHECKBOX].render = ui_render_checkbox;
    ui_component_ops[UI_COMPONENT_CHECKBOX].handle_pointer_release = ui_checkbox_handle_pointer_release;

    /* text input */
    ui_component_ops[UI_COMPONENT_TEXT_INPUT].render = ui_render_text_input;
    ui_component_ops[UI_COMPONENT_TEXT_INPUT].handle_key = ui_text_input_handle_key;

    /* list view */
    ui_component_ops[UI_COMPONENT_LIST_VIEW].render = ui_render_list_view;
    ui_component_ops[UI_COMPONENT_LIST_VIEW].layout = ui_layout_list_view;
    ui_component_ops[UI_COMPONENT_LIST_VIEW].handle_pointer_press = ui_list_view_handle_pointer_press;
    ui_component_ops[UI_COMPONENT_LIST_VIEW].handle_scroll_drag = ui_list_view_handle_scroll_drag;

    /* dropdown */
    ui_component_ops[UI_COMPONENT_DROPDOWN].render = ui_render_dropdown;
    ui_component_ops[UI_COMPONENT_DROPDOWN].layout = ui_layout_dropdown;
    ui_component_ops[UI_COMPONENT_DROPDOWN].handle_pointer_press = ui_dropdown_handle_pointer_press;
    ui_component_ops[UI_COMPONENT_DROPDOWN].handle_key = ui_dropdown_handle_key;
    ui_component_ops[UI_COMPONENT_DROPDOWN].popup_contains = ui_dropdown_popup_contains;
    ui_component_ops[UI_COMPONENT_DROPDOWN].destroy_data = NULL; /* data freed via generic or explicit close */

    /* scroll view */
    ui_component_ops[UI_COMPONENT_SCROLL_VIEW].render = ui_render_scroll_view;
    ui_component_ops[UI_COMPONENT_SCROLL_VIEW].layout = ui_layout_scroll_view;
    ui_component_ops[UI_COMPONENT_SCROLL_VIEW].handle_scroll_drag = ui_scroll_view_handle_scroll_drag;

    /* menu bar */
    ui_component_ops[UI_COMPONENT_MENU_BAR].render = ui_render_menu_bar;
    ui_component_ops[UI_COMPONENT_MENU_BAR].layout = ui_layout_menu_bar;

    /* menu item */
    ui_component_ops[UI_COMPONENT_MENU_ITEM].render = ui_render_menu_item;
    ui_component_ops[UI_COMPONENT_MENU_ITEM].handle_pointer_release = NULL; /* handled via the global ui_menu_item_handle_pointer_release */
    ui_component_ops[UI_COMPONENT_MENU_ITEM].popup_contains = ui_menu_item_popup_contains;
}

static inline void
ui_layout_vertical(ui_context_t *ctx, int32_t parent_id)
{
    ui_component_t *p = ui_component_by_id(ctx, parent_id);
    if (!p) return;

    const ui_component_ops_t *ops = &ui_component_ops[p->type];
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
        ui_component_t *c = ui_component_by_id(ctx, child_id);
        if (!c) break;
        const int32_t h = c->preferred_h > 8 ? c->preferred_h : 8;
        c->bounds.x = x;
        c->bounds.y = y;
        c->bounds.w = w;
        c->bounds.h = h;
        y += h + p->gap_px;
        if (c->first_child_id > 0) ui_layout_vertical(ctx, c->id);
        child_id = c->next_sibling_id;
    }
}

static inline void
ui_render_component_clip(ui_context_t *ctx, int32_t id, ui_rect_t clip, int32_t offset_y)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c || !ctx->mapped_base) return;
    const int32_t draw_y = c->bounds.y - offset_y;
    const ui_rect_t draw_bounds = { c->bounds.x, draw_y, c->bounds.w, c->bounds.h };

    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                      draw_bounds.x, draw_bounds.y, draw_bounds.w, draw_bounds.h, c->bg_color, clip);

    const ui_component_ops_t *ops = &ui_component_ops[c->type];
    if (ops->render) {
        ops->render(ctx, c, draw_bounds, clip, offset_y);
    }

    /* Some components (containers with popups/children) return early from their render
     * after painting children themselves. For others we draw a border and recurse. */
    if (c->type == UI_COMPONENT_LIST_VIEW ||
        c->type == UI_COMPONENT_DROPDOWN ||
        c->type == UI_COMPONENT_SCROLL_VIEW ||
        c->type == UI_COMPONENT_MENU_BAR ||
        c->type == UI_COMPONENT_MENU_ITEM) {
        /* they handled their own children / no generic border */
        return;
    }

    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, c->border_px, c->border_color, clip);

    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        ui_render_component_clip(ctx, child_id, clip, offset_y);
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
}

static inline void
ui_render_component(ui_context_t *ctx, int32_t id)
{
    ui_rect_t clip = { 0, 0, ctx->width, ctx->height };
    ui_render_component_clip(ctx, id, clip, 0);
}

static inline int32_t
ui_find_component_at(ui_context_t *ctx, int32_t id, int32_t x, int32_t y)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        const int32_t hit = ui_find_component_at(ctx, child_id, x, y);
        if (hit > 0) return hit;
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
    const ui_component_ops_t *ops = &ui_component_ops[c->type];
    if (ops->popup_contains && ops->popup_contains(ctx, c, x, y)) return c->id;
    if (ui_point_in_bounds(x, y, c->bounds)) return c->id;
    return -1;
}

static inline int32_t
ui_find_scrollable_at(ui_context_t *ctx, int32_t id, int32_t x, int32_t y)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        const int32_t hit = ui_find_scrollable_at(ctx, child_id, x, y);
        if (hit > 0) return hit;
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
    if ((c->type == UI_COMPONENT_SCROLL_VIEW || c->type == UI_COMPONENT_LIST_VIEW) && ui_point_in_bounds(x, y, c->bounds)) return c->id;
    return -1;
}

static inline int32_t
ui_find_list_view_at(ui_context_t *ctx, int32_t id, int32_t x, int32_t y)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        const int32_t hit = ui_find_list_view_at(ctx, child_id, x, y);
        if (hit > 0) return hit;
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
    const ui_component_ops_t *ops = &ui_component_ops[c->type];
    if (c->type == UI_COMPONENT_LIST_VIEW && ui_point_in_bounds(x, y, c->bounds)) return c->id;
    if (ops->popup_contains && ops->popup_contains(ctx, c, x, y)) return c->id;
    return -1;
}

static inline int32_t
ui_find_clickable_at(ui_context_t *ctx, int32_t id, int32_t x, int32_t y)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        int32_t hit = ui_find_clickable_at(ctx, child_id, x, y);
        if (hit > 0) return hit;
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
    if (c->clickable && ui_point_in_bounds(x, y, c->bounds)) return c->id;

    const ui_component_ops_t *ops = &ui_component_ops[c->type];
    if (ops->popup_contains && ops->popup_contains(ctx, c, x, y)) return c->id;
    /* also allow clicking the bar item itself for dropdown/menu even without explicit clickable flag in some cases */
    if ((c->type == UI_COMPONENT_DROPDOWN || c->type == UI_COMPONENT_MENU_ITEM) &&
        ui_point_in_bounds(x, y, c->bounds)) return c->id;

    return -1;
}

static inline int32_t
ui_loop_handle_ipc(ui_context_t *ctx, const wasmos_ipc_message_t *msg)
{
    if (!ctx || !msg) return UI_MSG_ERROR;
    if (msg->type != GFX_IPC_RESP || msg->arg0 != GFX_STATUS_OK) return UI_MSG_IGNORED;
    if (msg->arg1 == GFX_EVENT_NONE) return UI_MSG_CONSUMED;

    if (msg->arg1 == GFX_EVENT_CLOSE_REQUEST && msg->arg2 == ctx->window_id) {
        ctx->close_requested = 1;
        return UI_MSG_CONSUMED;
    }

    if (msg->arg1 == GFX_EVENT_RESIZE && msg->arg2 == ctx->window_id) {
        const int32_t rw = ui_u16_lo(msg->arg3);
        const int32_t rh = ui_u16_hi(msg->arg3);
        if (rw > 0 && rh > 0 && ui_realloc_buffer(ctx, rw, rh) == 0) {
            ui_component_t *root = ui_component_by_id(ctx, ctx->root_id);
            if (root) {
                root->bounds.w = rw;
                root->bounds.h = rh;
            }
            ctx->dirty = 1;
        }
        return UI_MSG_CONSUMED;
    }

    if (msg->arg1 == GFX_EVENT_POINTER) {
        const int32_t new_x = ui_u16_lo(msg->arg2);
        const int32_t new_y = ui_u16_hi(msg->arg2);
        const int32_t dx = new_x - ctx->pointer_x;
        const int32_t dy = new_y - ctx->pointer_y;
        const uint32_t buttons = (uint32_t)msg->arg3;

        ctx->pointer_x = new_x;
        ctx->pointer_y = new_y;
        if (ctx->pointer_x < 0) ctx->pointer_x = 0;
        if (ctx->pointer_y < 0) ctx->pointer_y = 0;
        if (ctx->pointer_x >= ctx->width) ctx->pointer_x = ctx->width - 1;
        if (ctx->pointer_y >= ctx->height) ctx->pointer_y = ctx->height - 1;

        const int32_t left_now = ((buttons & 1u) != 0u);
        const int32_t left_prev = ((ctx->pointer_buttons & 1u) != 0u);

        if (left_now && !left_prev) {
            const int32_t focus_id = ui_find_component_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t *focus = ui_component_by_id(ctx, focus_id);
            if (focus && (focus->type == UI_COMPONENT_TEXT_INPUT || focus->type == UI_COMPONENT_DROPDOWN)) {
                ctx->focused_component_id = focus->id;
            } else {
                ctx->focused_component_id = 0;
            }

            const int32_t hit_id = ui_find_clickable_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t *hit = ui_component_by_id(ctx, hit_id);
            if (hit) {
                hit->pressed = 1;
                ui_mark_dirty(ctx);
            }

            ctx->active_scroll_component_id = ui_find_scrollable_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);

            const int32_t list_id = ui_find_list_view_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t *lv = ui_component_by_id(ctx, list_id);
            if (lv) {
                const ui_component_ops_t *ops = &ui_component_ops[lv->type];
                if (ops->handle_pointer_press) {
                    ops->handle_pointer_press(ctx, lv, ctx->pointer_x, ctx->pointer_y);
                }
            }
        } else if (!left_now && left_prev) {
            const int32_t hit_id = ui_find_clickable_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t *hit = ui_component_by_id(ctx, hit_id);
            if (hit && hit->pressed) {
                const ui_component_ops_t *ops = &ui_component_ops[hit->type];
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

            /* Menu system release reaction is owned by the menu_item component (via its global handler). */
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
        } else if (left_now && left_prev && ctx->active_scroll_component_id > 0 && dy != 0) {
            ui_component_t *sv = ui_component_by_id(ctx, ctx->active_scroll_component_id);
            if (sv) {
                const ui_component_ops_t *ops = &ui_component_ops[sv->type];
                if (ops->handle_scroll_drag) {
                    ops->handle_scroll_drag(ctx, sv, dy);
                }
            }
        }

        ctx->pointer_buttons = buttons;
        return UI_MSG_CONSUMED;
    }

    if (msg->arg1 == GFX_EVENT_KEY) {
        const uint32_t key = (uint32_t)msg->arg2;
        const uint32_t flags = (uint32_t)msg->arg3;
        const int32_t key_down = ((flags & 1u) != 0u);
        if (!key_down) return UI_MSG_CONSUMED;

        if (ctx->focused_component_id > 0) {
            ui_component_t *focus = ui_component_by_id(ctx, ctx->focused_component_id);
            if (focus) {
                const ui_component_ops_t *ops = &ui_component_ops[focus->type];
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

static inline int32_t
ui_loop_drain(ui_context_t *ctx)
{
    int32_t status = 0;
    if (!ctx) return -1;
    if (!ctx->dirty) return 0;
    ui_component_t *root = ui_component_by_id(ctx, ctx->root_id);
    if (!root) return -1;

    ui_layout_vertical(ctx, root->id);
    ui_render_component(ctx, root->id);

    if (wasmos_shmem_flush(ctx->shmem_id, (int32_t)(uintptr_t)ctx->mapped_base, ctx->stride_bytes * ctx->height) != 0) {
        return -1;
    }

    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_PRESENT_WINDOW,
                    ctx->window_id, ctx->buffer_id, 0, 0, &status, 0, 0, 0) != 0) {
        return -1;
    }
    if (status == GFX_STATUS_INVALID || status == GFX_STATUS_BUSY) {
        /* Window resized between render and present — RESIZE event is incoming. */
        ctx->dirty = 0;
        return 0;
    }
    if (status != GFX_STATUS_OK) {
        return -1;
    }

    ctx->dirty = 0;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
