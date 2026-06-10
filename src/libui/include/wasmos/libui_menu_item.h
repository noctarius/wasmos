#ifndef WASMOS_LIBUI_MENU_ITEM_H
#define WASMOS_LIBUI_MENU_ITEM_H

/* libui_menu_item.h - Menu item component specific rendering (including popup). */

/* Prototype for helper used by render (defined later in this header). */
static inline ui_rect_t ui_menu_item_popup_bounds(const ui_context_t *ctx, const ui_component_t *c);

static inline void
ui_render_menu_item(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    if (c->dropdown_open || c->pressed) {
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                          draw_bounds.x + 1, draw_bounds.y + 1,
                          draw_bounds.w - 2, draw_bounds.h - 2, 0xFF2A4060u, clip);
    }
    ui_draw_text_clip(ctx,
                      draw_bounds.x + (c->padding_px > 0 ? c->padding_px : 8),
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      c->text ? c->text : "",
                      c->fg_color ? c->fg_color : 0xFFDDE8F0u,
                      clip);
    if (c->dropdown_open && c->item_count > 0) {
        const int32_t item_h = 22;
        const ui_rect_t popup = ui_menu_item_popup_bounds(ctx, c);
        if (popup.w > 0 && popup.h > 0) {
            const ui_rect_t popup_clip = ui_rect_intersect(clip, popup);
            ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                              popup.x, popup.y, popup.w, popup.h, 0xFF1A2840u, clip);
            ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, popup, 1, 0xFF4A6080u, clip);
            for (int32_t i = 0; i < c->item_count; ++i) {
                const int32_t row_y = popup.y + (i * item_h);
                const uint32_t row_bg = (i == c->selected_index) ? 0xFF2F5C88u : 0xFF1A2840u;
                ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                                  popup.x + 1, row_y, popup.w - 2, item_h, row_bg, popup_clip);
                ui_draw_text_clip(ctx,
                                  popup.x + 8, row_y + (item_h - ctx->font_px) / 2,
                                  c->list_items[i] ? c->list_items[i] : "", 0xFFFFFFFFu, popup_clip);
            }
        }
    }
}

static inline ui_rect_t
ui_menu_item_popup_bounds(const ui_context_t *ctx, const ui_component_t *c)
{
    ui_rect_t popup = { 0, 0, 0, 0 };
    if (!ctx || !c || c->type != UI_COMPONENT_MENU_ITEM || !c->dropdown_open || c->item_count <= 0) return popup;
    const int32_t item_h = 22;
    const int32_t pw = c->bounds.w > 160 ? c->bounds.w : 160;
    popup.x = c->bounds.x;
    popup.y = c->bounds.y + c->bounds.h;
    popup.w = pw;
    popup.h = c->item_count * item_h;
    return popup;
}

/* Component-owned popup hit test helper.
 * Core find_*_at and event code can call this for menu-item-specific popup bounds checking. */
static inline bool
ui_menu_item_popup_contains(const ui_context_t *ctx, const ui_component_t *c, int32_t x, int32_t y)
{
    if (!c->dropdown_open) return false;
    const ui_rect_t popup = ui_menu_item_popup_bounds(ctx, c);
    return popup.w > 0 && popup.h > 0 && ui_point_in_bounds(x, y, popup);
}

/* Component-owned helper to map a point in the popup to a selected item index, or -1 if not in popup.
 * Allows core event code to ask the component for its selection logic without duplicating popup math. */
static inline int32_t
ui_menu_item_get_selection_from_point(const ui_context_t *ctx, const ui_component_t *c, int32_t x, int32_t y)
{
    if (!ui_menu_item_popup_contains(ctx, c, x, y)) return -1;
    const ui_rect_t popup = ui_menu_item_popup_bounds(ctx, c);
    const int32_t item_h = 22;
    const int32_t idx = (y - popup.y) / item_h;
    if (idx >= 0 && idx < c->item_count) return idx;
    return -1;
}

/* Component-owned helpers for header activation and popup pick.
 * Core does the sibling close orchestration and identification, component owns its open/close/pick reaction. */
static inline void
ui_menu_item_open_dropdown(ui_context_t *ctx, ui_component_t *mi)
{
    if (mi->item_count > 0) {
        mi->dropdown_open = 1;
        ui_mark_dirty(ctx);
    }
}

static inline void
ui_menu_item_close_dropdown(ui_context_t *ctx, ui_component_t *mi)
{
    mi->dropdown_open = 0;
    ui_mark_dirty(ctx);
}

static inline void
ui_menu_item_pick_and_invoke(ui_context_t *ctx, ui_component_t *mi, int32_t idx)
{
    if (idx >= 0 && idx < mi->item_count) {
        mi->selected_index = idx;
        mi->dropdown_open = 0;
        if (mi->on_click) mi->on_click(ctx, mi->id, mi->on_click_user);
        ui_mark_dirty(ctx);
    }
}

#endif /* WASMOS_LIBUI_MENU_ITEM_H */
