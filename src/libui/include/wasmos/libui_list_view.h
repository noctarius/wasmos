#ifndef WASMOS_LIBUI_LIST_VIEW_H
#define WASMOS_LIBUI_LIST_VIEW_H

/* libui_list_view.h - List view component specific rendering (including scrolling and selection).
 */

/* Row index at (pointer_x, pointer_y) in window coordinates, or -1 when the
 * point is outside the component or past the last row. Rows are a fixed 20 px
 * tall and the current scroll offset is applied, so the result indexes
 * d->list.items directly. The scrollbar gutter is not excluded: a click on the
 * scrollbar still resolves to the row beside it. */
static inline int32_t ui_list_view_index_at(const ui_component_t* lv, const ui_list_view_data_t* d,
                                            int32_t pointer_x, int32_t pointer_y) {
    if (!lv || !d)
        return -1;
    if (pointer_x < lv->bounds.x || pointer_y < lv->bounds.y ||
        pointer_x >= (lv->bounds.x + lv->bounds.w) || pointer_y >= (lv->bounds.y + lv->bounds.h)) {
        return -1;
    }
    const int32_t rel_y = (pointer_y - (lv->bounds.y + lv->padding_px)) + d->scroll_y;
    if (rel_y < 0)
        return -1;
    const int32_t idx = rel_y / 20;
    if (idx < 0 || idx >= d->list.count)
        return -1;
    return idx;
}

/* Render op for UI_COMPONENT_LIST_VIEW: the inset content area, one 20 px row
 * per item with alternating stripes and a highlight on the selection, this
 * component's own border, and a vertical scrollbar when the content overflows.
 * It paints its own border, so the core skips the generic one; it also has no
 * child components to descend into.
 *
 * Rows are drawn for every item and clipped, not culled, so a long list costs
 * one font round trip per item per frame regardless of visibility. The
 * scrollbar gutter is reserved only while scroll_max > 0. */
static inline void ui_render_list_view(ui_context_t* ctx, const ui_component_t* c,
                                       ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y) {
    (void)offset_y;
    ui_list_view_data_t* d = (ui_list_view_data_t*)c->component_data;
    const int32_t scrollbar_w = (d && d->scroll_max > 0) ? 10 : 0;
    const ui_rect_t inner = {draw_bounds.x + c->padding_px,
                             draw_bounds.y + c->padding_px,
                             draw_bounds.w - (c->padding_px * 2),
                             draw_bounds.h - (c->padding_px * 2)};
    const int32_t content_w = inner.w - scrollbar_w;
    const ui_rect_t content_clip = {inner.x, inner.y, content_w, inner.h};
    const int32_t item_h = 20;
    ui_fill_rect_clip(ctx->mapped_base,
                      ctx->width,
                      ctx->height,
                      inner.x,
                      inner.y,
                      inner.w,
                      inner.h,
                      0xFF172233u,
                      clip);
    const ui_rect_t item_clip = ui_rect_intersect(clip, content_clip);
    if (d) {
        for (int32_t i = 0; i < d->list.count; ++i) {
            const int32_t row_y = inner.y + (i * item_h) - d->scroll_y;
            const uint32_t row_bg =
                (i == d->list.selected) ? 0xFF2F5C88u : ((i & 1) ? 0xFF1F2E43u : 0xFF1A283B);
            ui_fill_rect_clip(ctx->mapped_base,
                              ctx->width,
                              ctx->height,
                              inner.x,
                              row_y,
                              content_w,
                              item_h,
                              row_bg,
                              item_clip);
            ui_draw_text_clip(ctx,
                              inner.x + 6,
                              row_y + (item_h - ctx->font_px) / 2,
                              d->list.items[i] ? d->list.items[i] : "",
                              0xFFFFFFFFu,
                              item_clip);
        }
    }
    ui_stroke_rect_clip(ctx->mapped_base,
                        ctx->width,
                        ctx->height,
                        draw_bounds,
                        c->border_px,
                        c->border_color,
                        clip);
    if (d && d->scroll_max > 0 && inner.h > 8) {
        const int32_t track_x = inner.x + inner.w - scrollbar_w;
        ui_fill_rect_clip(ctx->mapped_base,
                          ctx->width,
                          ctx->height,
                          track_x - 1,
                          inner.y,
                          1,
                          inner.h,
                          0xFF31475Fu,
                          clip);
        ui_draw_v_scrollbar(ctx->mapped_base,
                            ctx->width,
                            ctx->height,
                            track_x,
                            inner.y,
                            scrollbar_w,
                            inner.h,
                            d->scroll_y,
                            d->scroll_max,
                            0xFF0E1622u,
                            0xFF8CB6D8u,
                            0xFFD7ECFFu,
                            clip);
    }
}

/* Layout op for UI_COMPONENT_LIST_VIEW. Assigns no child bounds (the rows are
 * not components); it recomputes scroll_max from the row count against the
 * viewport, clamps scroll_y into range, and clamps the selection into
 * [0, count). Note the clamp turns the initial "no selection" (-1) into row 0
 * as soon as the list is non-empty, so a list view always shows a selection. */
static inline void ui_layout_list_view(ui_context_t* ctx, ui_component_t* p) {
    (void)ctx;
    ui_list_view_data_t* d = (ui_list_view_data_t*)p->component_data;
    if (!d)
        return;
    const int32_t item_h = 20;
    const int32_t viewport_h = p->bounds.h - (p->padding_px * 2);
    const int32_t content_h = d->list.count * item_h;
    d->scroll_max = (content_h > viewport_h) ? (content_h - viewport_h) : 0;
    if (d->scroll_y < 0)
        d->scroll_y = 0;
    if (d->scroll_y > d->scroll_max)
        d->scroll_y = d->scroll_max;
    if (d->list.selected < 0)
        d->list.selected = 0;
    if (d->list.selected >= d->list.count)
        d->list.selected = (d->list.count > 0) ? (d->list.count - 1) : 0;
}

/* Component-owned reaction handler for pointer press (selection on list items).
 * Core (after ui_find_list_view_at) calls this so list owns its specific selection logic.
 *
 * Moves the selection to the row under the pointer and marks the context dirty.
 * A press that misses every row leaves the selection unchanged. No callback
 * fires here — activation is a double-click (ui_list_view_handle_activate). */
static inline void ui_list_view_handle_pointer_press(ui_context_t* ctx, ui_component_t* lv,
                                                     int32_t pointer_x, int32_t pointer_y) {
    ui_list_view_data_t* d = (ui_list_view_data_t*)lv->component_data;
    if (d && d->list.count > 0) {
        const int32_t idx = ui_list_view_index_at(lv, d, pointer_x, pointer_y);
        if (idx >= 0 && idx < d->list.count) {
            d->list.selected = idx;
            ui_mark_dirty(ctx);
        }
    }
}

/* Left double-click over a row: select it, then invoke on_activate with the row
 * index. Does nothing when no callback is installed, the list is empty, or the
 * point hits no row — including the case where the selection would otherwise
 * have moved, so a miss changes nothing. */
static inline void ui_list_view_handle_activate(ui_context_t* ctx, ui_component_t* lv,
                                                int32_t pointer_x, int32_t pointer_y) {
    ui_list_view_data_t* d = (ui_list_view_data_t*)lv->component_data;
    if (!d || d->list.count <= 0 || !d->on_activate)
        return;
    const int32_t idx = ui_list_view_index_at(lv, d, pointer_x, pointer_y);
    if (idx < 0 || idx >= d->list.count)
        return;
    d->list.selected = idx;
    d->on_activate(ctx, lv->id, idx, d->on_activate_user);
    ui_mark_dirty(ctx);
}

/* Right click over a row: select it, then invoke on_secondary_click with the
 * row index. Same no-op conditions as ui_list_view_handle_activate(). */
static inline void ui_list_view_handle_secondary_click(ui_context_t* ctx, ui_component_t* lv,
                                                       int32_t pointer_x, int32_t pointer_y) {
    ui_list_view_data_t* d = (ui_list_view_data_t*)lv->component_data;
    if (!d || d->list.count <= 0 || !d->on_secondary_click)
        return;
    const int32_t idx = ui_list_view_index_at(lv, d, pointer_x, pointer_y);
    if (idx < 0 || idx >= d->list.count)
        return;
    d->list.selected = idx;
    d->on_secondary_click(ctx, lv->id, idx, d->on_secondary_click_user);
    ui_mark_dirty(ctx);
}

/* Component-owned scroll drag handler (for active scroll during pointer drag).
 * `dy` is thumb travel in pixels; it is converted to content pixels by
 * ui_scroll_drag_delta() and the result is clamped into [0, scroll_max]. A drag
 * on a list that fits entirely does nothing. */
static inline void ui_list_view_handle_scroll_drag(ui_context_t* ctx, ui_component_t* c,
                                                   int32_t dy) {
    ui_list_view_data_t* d = (ui_list_view_data_t*)c->component_data;
    if (d && d->scroll_max > 0) {
        const int32_t viewport_h = c->bounds.h - (c->padding_px * 2);
        d->scroll_y += ui_scroll_drag_delta(dy, viewport_h, d->scroll_max);
        if (d->scroll_y < 0)
            d->scroll_y = 0;
        if (d->scroll_y > d->scroll_max)
            d->scroll_y = d->scroll_max;
        ui_mark_dirty(ctx);
    }
}

#endif /* WASMOS_LIBUI_LIST_VIEW_H */
