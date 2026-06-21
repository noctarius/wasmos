#ifndef WASMOS_LIBUI_TREE_VIEW_H
#define WASMOS_LIBUI_TREE_VIEW_H

static inline int32_t
ui_tree_view_index_at(const ui_component_t *tv, const ui_tree_view_data_t *d, int32_t pointer_x, int32_t pointer_y)
{
    if (!tv || !d) return -1;
    if (pointer_x < tv->bounds.x || pointer_y < tv->bounds.y ||
        pointer_x >= (tv->bounds.x + tv->bounds.w) ||
        pointer_y >= (tv->bounds.y + tv->bounds.h)) {
        return -1;
    }
    const int32_t rel_y = (pointer_y - (tv->bounds.y + tv->padding_px)) + d->scroll_y;
    if (rel_y < 0) return -1;
    {
        const int32_t idx = rel_y / 20;
        if (idx < 0 || idx >= d->list.count) return -1;
        return idx;
    }
}

static inline void
ui_render_tree_view(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    ui_tree_view_data_t *d = (ui_tree_view_data_t *)c->component_data;
    const int32_t scrollbar_w = (d && d->scroll_max > 0) ? 10 : 0;
    const ui_rect_t inner = {
        draw_bounds.x + c->padding_px,
        draw_bounds.y + c->padding_px,
        draw_bounds.w - (c->padding_px * 2),
        draw_bounds.h - (c->padding_px * 2)
    };
    const int32_t content_w = inner.w - scrollbar_w;
    const ui_rect_t content_clip = {
        inner.x,
        inner.y,
        content_w,
        inner.h
    };
    const int32_t item_h = 20;

    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                      inner.x, inner.y, inner.w, inner.h, 0xFF172233u, clip);
    {
        const ui_rect_t item_clip = ui_rect_intersect(clip, content_clip);
        if (d) {
            for (int32_t i = 0; i < d->list.count; ++i) {
                const int32_t row_y = inner.y + (i * item_h) - d->scroll_y;
                const int32_t depth = (d->depths && i < d->list.count) ? d->depths[i] : 0;
                const int32_t text_x = inner.x + 6 + (depth * 14);
                const uint32_t row_bg =
                    (i == d->list.selected) ? 0xFF2F5C88u : ((i & 1) ? 0xFF1F2E43u : 0xFF1A283B);

                ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                                  inner.x, row_y, content_w, item_h, row_bg, item_clip);
                for (int32_t level = 0; level < depth; ++level) {
                    const int32_t guide_x = inner.x + 10 + (level * 14);
                    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                                      guide_x, row_y + 3, 1, item_h - 6, 0xFF3B516Au, item_clip);
                }
                ui_draw_text_clip(ctx,
                                  text_x, row_y + (item_h - ctx->font_px) / 2,
                                  d->list.items[i] ? d->list.items[i] : "",
                                  0xFFFFFFFFu,
                                  item_clip);
            }
        }
    }
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, c->border_px, c->border_color, clip);
    if (d && d->scroll_max > 0 && inner.h > 8) {
        const int32_t track_x = inner.x + inner.w - scrollbar_w;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, track_x - 1, inner.y, 1, inner.h, 0xFF31475Fu, clip);
        ui_draw_v_scrollbar(ctx->mapped_base, ctx->width, ctx->height,
                            track_x, inner.y, scrollbar_w, inner.h,
                            d->scroll_y, d->scroll_max,
                            0xFF0E1622u, 0xFF8CB6D8u, 0xFFD7ECFFu, clip);
    }
}

static inline void
ui_layout_tree_view(ui_context_t *ctx, ui_component_t *p)
{
    (void)ctx;
    ui_tree_view_data_t *d = (ui_tree_view_data_t *)p->component_data;
    if (!d) return;
    {
        const int32_t item_h = 20;
        const int32_t viewport_h = p->bounds.h - (p->padding_px * 2);
        const int32_t content_h = d->list.count * item_h;
        d->scroll_max = (content_h > viewport_h) ? (content_h - viewport_h) : 0;
        if (d->scroll_y < 0) d->scroll_y = 0;
        if (d->scroll_y > d->scroll_max) d->scroll_y = d->scroll_max;
        if (d->list.selected < 0) d->list.selected = 0;
        if (d->list.selected >= d->list.count) d->list.selected = (d->list.count > 0) ? (d->list.count - 1) : 0;
    }
}

static inline void
ui_tree_view_handle_pointer_press(ui_context_t *ctx, ui_component_t *tv, int32_t pointer_x, int32_t pointer_y)
{
    ui_tree_view_data_t *d = (ui_tree_view_data_t *)tv->component_data;
    if (d && d->list.count > 0) {
        const int32_t idx = ui_tree_view_index_at(tv, d, pointer_x, pointer_y);
        if (idx >= 0 && idx < d->list.count) {
            d->list.selected = idx;
            ui_mark_dirty(ctx);
        }
    }
}

static inline void
ui_tree_view_handle_activate(ui_context_t *ctx, ui_component_t *tv, int32_t pointer_x, int32_t pointer_y)
{
    ui_tree_view_data_t *d = (ui_tree_view_data_t *)tv->component_data;
    if (!d || d->list.count <= 0 || !d->on_activate) return;
    {
        const int32_t idx = ui_tree_view_index_at(tv, d, pointer_x, pointer_y);
        if (idx < 0 || idx >= d->list.count) return;
        d->list.selected = idx;
        d->on_activate(ctx, tv->id, idx, d->on_activate_user);
        ui_mark_dirty(ctx);
    }
}

static inline void
ui_tree_view_handle_secondary_click(ui_context_t *ctx, ui_component_t *tv, int32_t pointer_x, int32_t pointer_y)
{
    ui_tree_view_data_t *d = (ui_tree_view_data_t *)tv->component_data;
    if (!d || d->list.count <= 0 || !d->on_secondary_click) return;
    {
        const int32_t idx = ui_tree_view_index_at(tv, d, pointer_x, pointer_y);
        if (idx < 0 || idx >= d->list.count) return;
        d->list.selected = idx;
        d->on_secondary_click(ctx, tv->id, idx, d->on_secondary_click_user);
        ui_mark_dirty(ctx);
    }
}

static inline void
ui_tree_view_handle_scroll_drag(ui_context_t *ctx, ui_component_t *c, int32_t dy)
{
    ui_tree_view_data_t *d = (ui_tree_view_data_t *)c->component_data;
    if (d && d->scroll_max > 0) {
        const int32_t viewport_h = c->bounds.h - (c->padding_px * 2);
        d->scroll_y += ui_scroll_drag_delta(dy, viewport_h, d->scroll_max);
        if (d->scroll_y < 0) d->scroll_y = 0;
        if (d->scroll_y > d->scroll_max) d->scroll_y = d->scroll_max;
        ui_mark_dirty(ctx);
    }
}

static inline void
ui_tree_view_destroy_data(ui_component_t *c)
{
    ui_tree_view_data_t *d;
    if (!c || !c->component_data) return;
    d = (ui_tree_view_data_t *)c->component_data;
    if (d->list.items) {
        for (int32_t i = 0; i < d->list.count; ++i) {
            if (d->list.items[i]) {
                free(d->list.items[i]);
                d->list.items[i] = NULL;
            }
        }
        free(d->list.items);
        d->list.items = NULL;
    }
    if (d->depths) {
        free(d->depths);
        d->depths = NULL;
        d->depth_capacity = 0;
    }
    free(d);
    c->component_data = NULL;
}

#endif /* WASMOS_LIBUI_TREE_VIEW_H */
