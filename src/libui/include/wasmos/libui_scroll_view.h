#ifndef WASMOS_LIBUI_SCROLL_VIEW_H
#define WASMOS_LIBUI_SCROLL_VIEW_H

/* libui_scroll_view.h - Scroll view component specific rendering (viewport, children with offset, scrollbar). */

static inline void
ui_render_scroll_view(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    ui_scroll_view_data_t *d = (ui_scroll_view_data_t *)c->component_data;
    const ui_rect_t inner = {
        draw_bounds.x + c->padding_px,
        draw_bounds.y + c->padding_px,
        draw_bounds.w - (c->padding_px * 2),
        draw_bounds.h - (c->padding_px * 2)
    };
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, inner.x, inner.y, inner.w, inner.h, 0xFF1B2535u, clip);
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, c->border_px, c->border_color, clip);
    const ui_rect_t child_clip = ui_rect_intersect(clip, inner);
    int32_t child_id = c->first_child_id;
    int32_t off = d ? d->scroll_y : 0;
    while (child_id > 0) {
        ui_render_component_clip(ctx, child_id, child_clip, offset_y + off);
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
    if (d && d->scroll_max > 0 && inner.h > 8) {
        const int32_t scrollbar_w = 10;
        const int32_t track_x = inner.x + inner.w - scrollbar_w;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, track_x - 1, inner.y, 1, inner.h, 0xFF31475Fu, clip);
        ui_draw_v_scrollbar(ctx->mapped_base, ctx->width, ctx->height,
                            track_x, inner.y, scrollbar_w, inner.h,
                            d->scroll_y, d->scroll_max,
                            0xFF0E1622u, 0xFF8CB6D8u, 0xFFD7ECFFu, clip);
    }
}

static inline void
ui_layout_scroll_view(ui_context_t *ctx, ui_component_t *p)
{
    ui_scroll_view_data_t *d = (ui_scroll_view_data_t *)p->component_data;
    if (!d) return;

    int32_t y_cur = p->bounds.y + p->padding_px;
    int32_t content_h = 0;
    int32_t child_id = p->first_child_id;
    while (child_id > 0) {
        ui_component_t *c = ui_component_by_id(ctx, child_id);
        if (!c) break;
        const int32_t h = c->preferred_h > 8 ? c->preferred_h : 8;
        c->bounds.x = p->bounds.x + p->padding_px;
        c->bounds.y = y_cur;
        c->bounds.w = p->bounds.w - (p->padding_px * 2);
        c->bounds.h = h;
        y_cur += h + p->gap_px;
        content_h += h + p->gap_px;
        if (c->first_child_id > 0) ui_layout_vertical(ctx, c->id);
        child_id = c->next_sibling_id;
    }
    if (content_h > 0) content_h -= p->gap_px;
    const int32_t viewport_h = p->bounds.h - (p->padding_px * 2);
    d->scroll_max = (content_h > viewport_h) ? (content_h - viewport_h) : 0;
    if (d->scroll_y < 0) d->scroll_y = 0;
    if (d->scroll_y > d->scroll_max) d->scroll_y = d->scroll_max;
}

/* Component-owned scroll drag handler (for active scroll during pointer drag). */
static inline void
ui_scroll_view_handle_scroll_drag(ui_context_t *ctx, ui_component_t *c, int32_t dy)
{
    ui_scroll_view_data_t *d = (ui_scroll_view_data_t *)c->component_data;
    if (d && d->scroll_max > 0) {
        d->scroll_y += dy;
        if (d->scroll_y < 0) d->scroll_y = 0;
        if (d->scroll_y > d->scroll_max) d->scroll_y = d->scroll_max;
        ui_mark_dirty(ctx);
    }
}

#endif /* WASMOS_LIBUI_SCROLL_VIEW_H */
