#ifndef WASMOS_LIBUI_H
#define WASMOS_LIBUI_H

#include <stdint.h>
#include <stddef.h>

#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAX_COMPONENTS 64
#define UI_TEXT_MAX 48
#define UI_PAGE_SIZE 4096
#define UI_REQ_BASE 0x7400

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
    UI_COMPONENT_SCROLL_VIEW = 6
} ui_component_type_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} ui_rect_t;

struct ui_context;

typedef void (*ui_button_click_cb_t)(struct ui_context *ctx, int32_t component_id, void *user);

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
    int32_t checked;
    int32_t scroll_y;
    int32_t scroll_max;
    char text[UI_TEXT_MAX];
    ui_button_click_cb_t on_click;
    void *on_click_user;
} ui_component_t;

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
    ui_component_t components[UI_MAX_COMPONENTS];
} ui_context_t;

static inline int32_t ui_u16_lo(int32_t packed) { return (packed & 0xFFFF); }
static inline int32_t ui_u16_hi(int32_t packed) { return ((packed >> 16) & 0xFFFF); }
static inline int32_t ui_i16_lo(int32_t packed) { return (int16_t)(packed & 0xFFFF); }
static inline int32_t ui_i16_hi(int32_t packed) { return (int16_t)((packed >> 16) & 0xFFFF); }

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
ui_stroke_rect(uint8_t *base, int32_t bw, int32_t bh, ui_rect_t r, int32_t border_px, uint32_t color)
{
    if (border_px <= 0) return;
    ui_fill_rect(base, bw, bh, r.x, r.y, r.w, border_px, color);
    ui_fill_rect(base, bw, bh, r.x, r.y + r.h - border_px, r.w, border_px, color);
    ui_fill_rect(base, bw, bh, r.x, r.y, border_px, r.h, color);
    ui_fill_rect(base, bw, bh, r.x + r.w - border_px, r.y, border_px, r.h, color);
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

static inline uint8_t
ui_glyph5x7(char ch, int32_t row)
{
    if (row < 0 || row > 6) return 0;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    switch (ch) {
        case 'a': { static const uint8_t g[7] = {0x00,0x0E,0x01,0x0F,0x11,0x0F,0x00}; return g[row]; }
        case 'b': { static const uint8_t g[7] = {0x10,0x10,0x1E,0x11,0x11,0x1E,0x00}; return g[row]; }
        case 'c': { static const uint8_t g[7] = {0x00,0x0E,0x11,0x10,0x11,0x0E,0x00}; return g[row]; }
        case 'd': { static const uint8_t g[7] = {0x01,0x01,0x0F,0x11,0x11,0x0F,0x00}; return g[row]; }
        case 'e': { static const uint8_t g[7] = {0x00,0x0E,0x11,0x1F,0x10,0x0E,0x00}; return g[row]; }
        case 'f': { static const uint8_t g[7] = {0x06,0x08,0x1E,0x08,0x08,0x08,0x00}; return g[row]; }
        case 'g': { static const uint8_t g[7] = {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}; return g[row]; }
        case 'h': { static const uint8_t g[7] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x00}; return g[row]; }
        case 'i': { static const uint8_t g[7] = {0x04,0x00,0x0C,0x04,0x04,0x0E,0x00}; return g[row]; }
        case 'j': { static const uint8_t g[7] = {0x02,0x00,0x06,0x02,0x12,0x12,0x0C}; return g[row]; }
        case 'k': { static const uint8_t g[7] = {0x10,0x12,0x14,0x18,0x14,0x12,0x00}; return g[row]; }
        case 'l': { static const uint8_t g[7] = {0x0C,0x04,0x04,0x04,0x04,0x0E,0x00}; return g[row]; }
        case 'm': { static const uint8_t g[7] = {0x00,0x1A,0x15,0x15,0x15,0x15,0x00}; return g[row]; }
        case 'n': { static const uint8_t g[7] = {0x00,0x1E,0x11,0x11,0x11,0x11,0x00}; return g[row]; }
        case 'o': { static const uint8_t g[7] = {0x00,0x0E,0x11,0x11,0x11,0x0E,0x00}; return g[row]; }
        case 'p': { static const uint8_t g[7] = {0x00,0x1E,0x11,0x11,0x1E,0x10,0x10}; return g[row]; }
        case 'q': { static const uint8_t g[7] = {0x00,0x0F,0x11,0x11,0x0F,0x01,0x01}; return g[row]; }
        case 'r': { static const uint8_t g[7] = {0x00,0x16,0x19,0x10,0x10,0x10,0x00}; return g[row]; }
        case 's': { static const uint8_t g[7] = {0x00,0x0F,0x10,0x0E,0x01,0x1E,0x00}; return g[row]; }
        case 't': { static const uint8_t g[7] = {0x08,0x1E,0x08,0x08,0x09,0x06,0x00}; return g[row]; }
        case 'u': { static const uint8_t g[7] = {0x00,0x11,0x11,0x11,0x13,0x0D,0x00}; return g[row]; }
        case 'v': { static const uint8_t g[7] = {0x00,0x11,0x11,0x11,0x0A,0x04,0x00}; return g[row]; }
        case 'w': { static const uint8_t g[7] = {0x00,0x11,0x15,0x15,0x15,0x0A,0x00}; return g[row]; }
        case 'x': { static const uint8_t g[7] = {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}; return g[row]; }
        case 'y': { static const uint8_t g[7] = {0x00,0x11,0x11,0x0F,0x01,0x11,0x0E}; return g[row]; }
        case 'z': { static const uint8_t g[7] = {0x00,0x1F,0x02,0x04,0x08,0x1F,0x00}; return g[row]; }
        case '0': { static const uint8_t g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g[row]; }
        case '1': { static const uint8_t g[7] = {0x04,0x0C,0x14,0x04,0x04,0x04,0x1F}; return g[row]; }
        case '2': { static const uint8_t g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; return g[row]; }
        case '3': { static const uint8_t g[7] = {0x1E,0x01,0x01,0x06,0x01,0x01,0x1E}; return g[row]; }
        case '4': { static const uint8_t g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g[row]; }
        case '5': { static const uint8_t g[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}; return g[row]; }
        case '6': { static const uint8_t g[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; return g[row]; }
        case '7': { static const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g[row]; }
        case '8': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g[row]; }
        case '9': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; return g[row]; }
        case '-': { static const uint8_t g[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g[row]; }
        case ':': { static const uint8_t g[7] = {0x00,0x04,0x00,0x00,0x04,0x00,0x00}; return g[row]; }
        case ' ': return 0x00;
        default:  { static const uint8_t g[7] = {0x1F,0x11,0x15,0x11,0x15,0x11,0x1F}; return g[row]; }
    }
}

static inline void
ui_draw_text(uint8_t *base, int32_t bw, int32_t bh, int32_t x, int32_t y, const char *text, uint32_t color)
{
    if (!base || !text) return;
    int32_t cx = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const char ch = text[i];
        for (int32_t row = 0; row < 7; ++row) {
            const uint8_t bits = ui_glyph5x7(ch, row);
            for (int32_t col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1u) {
                    ui_fill_rect(base, bw, bh, cx + col, y + row, 1, 1, color);
                }
            }
        }
        cx += 6;
    }
}

static inline void
ui_draw_text_clip(uint8_t *base, int32_t bw, int32_t bh, int32_t x, int32_t y, const char *text, uint32_t color, ui_rect_t clip)
{
    if (!base || !text) return;
    int32_t cx = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const char ch = text[i];
        for (int32_t row = 0; row < 7; ++row) {
            const uint8_t bits = ui_glyph5x7(ch, row);
            for (int32_t col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1u) {
                    ui_fill_rect_clip(base, bw, bh, cx + col, y + row, 1, 1, color, clip);
                }
            }
        }
        cx += 6;
    }
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
    if (!ctx || id <= 0 || id > UI_MAX_COMPONENTS) return 0;
    if (!ctx->components[id - 1].in_use) return 0;
    return &ctx->components[id - 1];
}

static inline int32_t
ui_component_alloc(ui_context_t *ctx, ui_component_type_t type)
{
    if (!ctx) return -1;
    for (int32_t i = 0; i < UI_MAX_COMPONENTS; ++i) {
        if (!ctx->components[i].in_use) {
            ui_component_t *c = &ctx->components[i];
            c->in_use = 1;
            c->id = i + 1;
            c->parent_id = 0;
            c->first_child_id = 0;
            c->next_sibling_id = 0;
            c->type = type;
            c->preferred_h = 24;
            c->bg_color = 0xFF2B3440u;
            c->fg_color = 0xFFFFFFFFu;
            c->border_color = 0xFF536271u;
            c->border_px = 1;
            c->padding_px = 6;
            c->gap_px = 6;
            c->clickable = 0;
            c->pressed = 0;
            c->checked = 0;
            c->scroll_y = 0;
            c->scroll_max = 0;
            c->text[0] = '\0';
            c->on_click = 0;
            c->on_click_user = 0;
            return c->id;
        }
    }
    return -1;
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

static inline void
ui_component_set_text(ui_context_t *ctx, int32_t id, const char *text)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return;
    if (!text) {
        c->text[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < UI_TEXT_MAX && text[i] != '\0'; ++i) c->text[i] = text[i];
    c->text[i] = '\0';
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
    if (!c || c->type != UI_COMPONENT_CHECKBOX) return;
    c->checked = checked ? 1 : 0;
}

static inline int32_t
ui_component_text_len(const ui_component_t *c)
{
    int32_t n = 0;
    if (!c) return 0;
    while (n < (UI_TEXT_MAX - 1) && c->text[n] != '\0') n++;
    return n;
}

static inline void ui_mark_dirty(ui_context_t *ctx) { if (ctx) ctx->dirty = 1; }

static inline int32_t
ui_realloc_buffer(ui_context_t *ctx, int32_t new_w, int32_t new_h)
{
    int32_t status = 0, new_buffer_id = 0, new_shmem_id = 0, new_stride = 0;
    if (!ctx || new_w <= 0 || new_h <= 0) return -1;
    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_ALLOC_SHARED_BUFFER,
                    ctx->window_id, new_w, new_h, 0,
                    &status, &new_buffer_id, &new_shmem_id, &new_stride) != 0 || status != GFX_STATUS_OK) {
        return -1;
    }
    const int32_t bytes = (new_stride * new_h + (UI_PAGE_SIZE - 1)) & ~(UI_PAGE_SIZE - 1);
    const int32_t mapped_ptr = wasmos_shmem_map_auto(new_shmem_id, bytes);
    if (mapped_ptr < 0) return -1;
    if (ctx->shmem_id > 0) {
        (void)wasmos_shmem_unmap(ctx->shmem_id);
    }
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
    ctx->pointer_x = ctx->width / 2;
    ctx->pointer_y = ctx->height / 2;
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
    for (size_t i = 0; i < sizeof(*ctx); ++i) ((uint8_t *)ctx)[i] = 0;
    ctx->proc_endpoint = proc_endpoint;
    ctx->reply_endpoint = reply_endpoint;
    ctx->req_id = UI_REQ_BASE;
    for (int32_t spins = 0; spins < 2048; ++spins) {
        ctx->gfx_endpoint = wasmos_svc_lookup(proc_endpoint, reply_endpoint, "gfx", ctx->req_id++);
        if (ctx->gfx_endpoint >= 0) break;
        (void)wasmos_sched_yield();
    }
    if (ctx->gfx_endpoint < 0) return -1;
    if (ui_send_gfx(ctx->gfx_endpoint, reply_endpoint, ctx->req_id++, GFX_IPC_CREATE_WINDOW,
                    width, height, (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status, &a1, &a2, &a3) != 0 || status != GFX_STATUS_OK) {
        return -1;
    }
    ctx->window_id = a1;
    if (ui_realloc_buffer(ctx, width, height) != 0) return -1;
    ctx->root_id = ui_component_create_panel(ctx);
    if (ctx->root_id < 0) return -1;
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
}

static inline int32_t
ui_point_in_bounds(int32_t x, int32_t y, ui_rect_t r)
{
    return x >= r.x && y >= r.y && x < (r.x + r.w) && y < (r.y + r.h);
}

static inline void
ui_layout_vertical(ui_context_t *ctx, int32_t parent_id)
{
    ui_component_t *p = ui_component_by_id(ctx, parent_id);
    if (!p) return;
    int32_t x = p->bounds.x + p->padding_px;
    int32_t y = p->bounds.y + p->padding_px;
    const int32_t w = p->bounds.w - (p->padding_px * 2);
    if (p->type == UI_COMPONENT_SCROLL_VIEW) {
        int32_t y_cur = p->bounds.y + p->padding_px;
        int32_t content_h = 0;
        int32_t child_id_sv = p->first_child_id;
        while (child_id_sv > 0) {
            ui_component_t *c_sv = ui_component_by_id(ctx, child_id_sv);
            if (!c_sv) break;
            const int32_t h_sv = c_sv->preferred_h > 8 ? c_sv->preferred_h : 8;
            c_sv->bounds.x = p->bounds.x + p->padding_px;
            c_sv->bounds.y = y_cur;
            c_sv->bounds.w = w;
            c_sv->bounds.h = h_sv;
            y_cur += h_sv + p->gap_px;
            content_h += h_sv + p->gap_px;
            if (c_sv->first_child_id > 0) ui_layout_vertical(ctx, c_sv->id);
            child_id_sv = c_sv->next_sibling_id;
        }
        if (content_h > 0) content_h -= p->gap_px;
        const int32_t viewport_h = p->bounds.h - (p->padding_px * 2);
        p->scroll_max = (content_h > viewport_h) ? (content_h - viewport_h) : 0;
        if (p->scroll_y < 0) p->scroll_y = 0;
        if (p->scroll_y > p->scroll_max) p->scroll_y = p->scroll_max;
        return;
    }
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
    if (c->type == UI_COMPONENT_LABEL) {
        const int32_t tx = draw_bounds.x + c->padding_px;
        const int32_t ty = draw_bounds.y + (draw_bounds.h - 7) / 2;
        ui_draw_text_clip(ctx->mapped_base, ctx->width, ctx->height, tx, ty, c->text, c->fg_color, clip);
    } else if (c->type == UI_COMPONENT_BUTTON) {
        const uint32_t inner = c->pressed ? 0xFF2B6AA0u : 0xFF4B91CCu;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                          draw_bounds.x + 2, draw_bounds.y + 2, draw_bounds.w - 4, draw_bounds.h - 4, inner, clip);
        ui_draw_text_clip(ctx->mapped_base, ctx->width, ctx->height,
                          draw_bounds.x + c->padding_px,
                          draw_bounds.y + (draw_bounds.h - 7) / 2,
                          c->text,
                          0xFFFFFFFFu,
                          clip);
    } else if (c->type == UI_COMPONENT_CHECKBOX) {
        const int32_t box = draw_bounds.h > 16 ? 16 : draw_bounds.h - 4;
        const int32_t bx = draw_bounds.x + c->padding_px;
        const int32_t by = draw_bounds.y + (draw_bounds.h - box) / 2;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, bx, by, box, box, 0xFF2B3440u, clip);
        ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, (ui_rect_t){bx, by, box, box}, 1, 0xFF9CB6CEu, clip);
        if (c->checked) {
            ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, bx + 4, by + 4, box - 8, box - 8, 0xFF66CC88u, clip);
        }
        ui_draw_text_clip(ctx->mapped_base, ctx->width, ctx->height,
                          bx + box + 8,
                          draw_bounds.y + (draw_bounds.h - 7) / 2,
                          c->text,
                          c->fg_color,
                          clip);
    } else if (c->type == UI_COMPONENT_TEXT_INPUT) {
        const int32_t active = (ctx->focused_component_id == c->id);
        const uint32_t inner = active ? 0xFF1F3148u : 0xFF1C2738u;
        const uint32_t outline = active ? 0xFF89C9FFu : c->border_color;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                          draw_bounds.x + 1, draw_bounds.y + 1, draw_bounds.w - 2, draw_bounds.h - 2, inner, clip);
        ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, 1, outline, clip);
        const int32_t tx = draw_bounds.x + c->padding_px;
        const int32_t ty = draw_bounds.y + (draw_bounds.h - 7) / 2;
        ui_draw_text_clip(ctx->mapped_base, ctx->width, ctx->height, tx, ty, c->text, 0xFFFFFFFFu, clip);
        if (active) {
            const int32_t len = ui_component_text_len(c);
            const int32_t caret_x = tx + (len * 6);
            ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, caret_x, ty - 1, 1, 9, 0xFFFFFFFFu, clip);
        }
    } else if (c->type == UI_COMPONENT_SCROLL_VIEW) {
        const ui_rect_t inner = {
            draw_bounds.x + c->padding_px,
            draw_bounds.y + c->padding_px,
            draw_bounds.w - (c->padding_px * 2),
            draw_bounds.h - (c->padding_px * 2)
        };
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, inner.x, inner.y, inner.w, inner.h, 0xFF1B2535u, clip);
        ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, c->border_px, c->border_color, clip);
        const ui_rect_t child_clip = ui_rect_intersect(clip, inner);
        int32_t child_id_sv = c->first_child_id;
        while (child_id_sv > 0) {
            ui_render_component_clip(ctx, child_id_sv, child_clip, offset_y + c->scroll_y);
            ui_component_t *child_sv = ui_component_by_id(ctx, child_id_sv);
            if (!child_sv) break;
            child_id_sv = child_sv->next_sibling_id;
        }
        if (c->scroll_max > 0 && inner.h > 8) {
            const int32_t track_h = inner.h;
            const int32_t thumb_h = (track_h * track_h) / (track_h + c->scroll_max);
            const int32_t th = thumb_h < 8 ? 8 : thumb_h;
            const int32_t ty = inner.y + ((track_h - th) * c->scroll_y) / c->scroll_max;
            ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, inner.x + inner.w - 4, ty, 3, th, 0xFF6C88A8u, clip);
        }
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
    if (ui_point_in_bounds(x, y, c->bounds)) return c->id;
    return -1;
}

static inline int32_t
ui_find_scroll_view_at(ui_context_t *ctx, int32_t id, int32_t x, int32_t y)
{
    ui_component_t *c = ui_component_by_id(ctx, id);
    if (!c) return -1;
    int32_t child_id = c->first_child_id;
    while (child_id > 0) {
        const int32_t hit = ui_find_scroll_view_at(ctx, child_id, x, y);
        if (hit > 0) return hit;
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
    if (c->type == UI_COMPONENT_SCROLL_VIEW && ui_point_in_bounds(x, y, c->bounds)) return c->id;
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
        const int32_t dx = ui_i16_lo(msg->arg2);
        const int32_t dy = ui_i16_hi(msg->arg2);
        const uint32_t buttons = (uint32_t)msg->arg3;
        ctx->pointer_x += dx;
        ctx->pointer_y += dy;
        if (ctx->pointer_x < 0) ctx->pointer_x = 0;
        if (ctx->pointer_y < 0) ctx->pointer_y = 0;
        if (ctx->pointer_x >= ctx->width) ctx->pointer_x = ctx->width - 1;
        if (ctx->pointer_y >= ctx->height) ctx->pointer_y = ctx->height - 1;
        const int32_t left_now = ((buttons & 1u) != 0u);
        const int32_t left_prev = ((ctx->pointer_buttons & 1u) != 0u);
        if (left_now && !left_prev) {
            const int32_t focus_id = ui_find_component_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t *focus = ui_component_by_id(ctx, focus_id);
            if (focus && focus->type == UI_COMPONENT_TEXT_INPUT) {
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
            ctx->active_scroll_component_id = ui_find_scroll_view_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
        } else if (!left_now && left_prev) {
            const int32_t hit_id = ui_find_clickable_at(ctx, ctx->root_id, ctx->pointer_x, ctx->pointer_y);
            ui_component_t *hit = ui_component_by_id(ctx, hit_id);
            if (hit && hit->pressed && hit->on_click) {
                if (hit->type == UI_COMPONENT_CHECKBOX) {
                    hit->checked = hit->checked ? 0 : 1;
                }
                hit->on_click(ctx, hit->id, hit->on_click_user);
            }
            for (int32_t i = 0; i < UI_MAX_COMPONENTS; ++i) {
                if (ctx->components[i].in_use && ctx->components[i].pressed) {
                    ctx->components[i].pressed = 0;
                }
            }
            ctx->active_scroll_component_id = 0;
            ui_mark_dirty(ctx);
        } else if (left_now && left_prev && ctx->active_scroll_component_id > 0 && dy != 0) {
            ui_component_t *sv = ui_component_by_id(ctx, ctx->active_scroll_component_id);
            if (sv && sv->type == UI_COMPONENT_SCROLL_VIEW && sv->scroll_max > 0) {
                sv->scroll_y -= dy;
                if (sv->scroll_y < 0) sv->scroll_y = 0;
                if (sv->scroll_y > sv->scroll_max) sv->scroll_y = sv->scroll_max;
                ui_mark_dirty(ctx);
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
            if (focus && focus->type == UI_COMPONENT_TEXT_INPUT) {
                int32_t len = ui_component_text_len(focus);
                if (key == 8u || key == 127u) {
                    if (len > 0) {
                        focus->text[len - 1] = '\0';
                        ui_mark_dirty(ctx);
                    }
                } else if (key >= 32u && key <= 126u) {
                    if (len < (UI_TEXT_MAX - 1)) {
                        focus->text[len] = (char)key;
                        focus->text[len + 1] = '\0';
                        ui_mark_dirty(ctx);
                    }
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
                    ctx->window_id, ctx->buffer_id, 0, 0, &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) {
        return -1;
    }
    ctx->dirty = 0;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
