#ifndef WASMOS_LIBUI_LIST_VIEW_H
#define WASMOS_LIBUI_LIST_VIEW_H

/* libui_list_view.h - List view component specific rendering (including scrolling and selection). */

static inline void
ui_render_list_view(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    const ui_rect_t inner = {
        draw_bounds.x + c->padding_px,
        draw_bounds.y + c->padding_px,
        draw_bounds.w - (c->padding_px * 2),
        draw_bounds.h - (c->padding_px * 2)
    };
    const int32_t item_h = 20;
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, inner.x, inner.y, inner.w, inner.h, 0xFF172233u, clip);
    const ui_rect_t item_clip = ui_rect_intersect(clip, inner);
    for (int32_t i = 0; i < c->item_count; ++i) {
        const int32_t row_y = inner.y + (i * item_h) - c->scroll_y;
        const uint32_t row_bg = (i == c->selected_index) ? 0xFF2F5C88u : ((i & 1) ? 0xFF1F2E43u : 0xFF1A283B);
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, inner.x, row_y, inner.w, item_h, row_bg, item_clip);
        ui_draw_text_clip(ctx,
                          inner.x + 6, row_y + (item_h - ctx->font_px) / 2,
                          c->list_items[i] ? c->list_items[i] : "",
                          0xFFFFFFFFu,
                          item_clip);
    }
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, c->border_px, c->border_color, clip);
    if (c->scroll_max > 0 && inner.h > 8) {
        const int32_t track_h = inner.h;
        const int32_t thumb_h = (track_h * track_h) / (track_h + c->scroll_max);
        const int32_t th = thumb_h < 8 ? 8 : thumb_h;
        const int32_t ty = inner.y + ((track_h - th) * c->scroll_y) / c->scroll_max;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, inner.x + inner.w - 4, ty, 3, th, 0xFF6C88A8u, clip);
    }
}

static inline void
ui_layout_list_view(ui_context_t *ctx, ui_component_t *p)
{
    (void)ctx;
    const int32_t item_h = 20;
    const int32_t viewport_h = p->bounds.h - (p->padding_px * 2);
    const int32_t content_h = p->item_count * item_h;
    p->scroll_max = (content_h > viewport_h) ? (content_h - viewport_h) : 0;
    if (p->scroll_y < 0) p->scroll_y = 0;
    if (p->scroll_y > p->scroll_max) p->scroll_y = p->scroll_max;
    if (p->selected_index < 0) p->selected_index = 0;
    if (p->selected_index >= p->item_count) p->selected_index = (p->item_count > 0) ? (p->item_count - 1) : 0;
}

/* Component-owned reaction handler for pointer press (selection on list items).
 * Core (after ui_find_list_view_at) calls this so list owns its specific selection logic. */
static inline void
ui_list_view_handle_pointer_press(ui_context_t *ctx, ui_component_t *lv, int32_t pointer_x, int32_t pointer_y)
{
    if (lv->item_count > 0) {
        const int32_t rel_y = (pointer_y - (lv->bounds.y + lv->padding_px)) + lv->scroll_y;
        const int32_t idx = rel_y / 20;
        if (idx >= 0 && idx < lv->item_count) {
            lv->selected_index = idx;
            ui_mark_dirty(ctx);
        }
    }
}

/* Component-owned scroll drag handler (for active scroll during pointer drag). */
static inline void
ui_list_view_handle_scroll_drag(ui_context_t *ctx, ui_component_t *c, int32_t dy)
{
    if (c->scroll_max > 0) {
        c->scroll_y -= dy;
        if (c->scroll_y < 0) c->scroll_y = 0;
        if (c->scroll_y > c->scroll_max) c->scroll_y = c->scroll_max;
        ui_mark_dirty(ctx);
    }
}

#endif /* WASMOS_LIBUI_LIST_VIEW_H */
