#ifndef WASMOS_LIBUI_SCROLL_VIEW_H
#define WASMOS_LIBUI_SCROLL_VIEW_H

/* libui_scroll_view.h - Scroll view component specific rendering (viewport, children with offset, scrollbar). */

static inline void
ui_render_scroll_view(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
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
    while (child_id > 0) {
        ui_render_component_clip(ctx, child_id, child_clip, offset_y + c->scroll_y);
        ui_component_t *child = ui_component_by_id(ctx, child_id);
        if (!child) break;
        child_id = child->next_sibling_id;
    }
    if (c->scroll_max > 0 && inner.h > 8) {
        const int32_t track_h = inner.h;
        const int32_t thumb_h = (track_h * track_h) / (track_h + c->scroll_max);
        const int32_t th = thumb_h < 8 ? 8 : thumb_h;
        const int32_t ty = inner.y + ((track_h - th) * c->scroll_y) / c->scroll_max;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, inner.x + inner.w - 4, ty, 3, th, 0xFF6C88A8u, clip);
    }
}

#endif /* WASMOS_LIBUI_SCROLL_VIEW_H */
