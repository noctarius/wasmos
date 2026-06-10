#ifndef WASMOS_LIBUI_DROPDOWN_H
#define WASMOS_LIBUI_DROPDOWN_H

/* libui_dropdown.h - Dropdown component specific rendering (including popup). */

static inline void
ui_render_dropdown(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    const int32_t active = (ctx->focused_component_id == c->id);
    const uint32_t inner = active ? 0xFF1F3148u : 0xFF1C2738u;
    const uint32_t outline = active ? 0xFF89C9FFu : c->border_color;
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                      draw_bounds.x + 1, draw_bounds.y + 1, draw_bounds.w - 2, draw_bounds.h - 2, inner, clip);
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, 1, outline, clip);

    const char *selected = "";
    if (c->selected_index >= 0 && c->selected_index < c->item_count && c->list_items && c->list_items[c->selected_index]) {
        selected = c->list_items[c->selected_index];
    } else if (c->text) {
        selected = c->text;
    }
    ui_draw_text_clip(ctx,
                      draw_bounds.x + c->padding_px, draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2, selected, 0xFFFFFFFFu, clip);
    ui_draw_text_clip(ctx,
                      draw_bounds.x + draw_bounds.w - 12, draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      c->dropdown_open ? "^" : "v", 0xFF9CB6CEu, clip);

    if (c->dropdown_open && c->item_count > 0) {
        const int32_t item_h = 20;
        const int32_t popup_h = (c->item_count * item_h > 120) ? 120 : (c->item_count * item_h);
        ui_rect_t popup = { draw_bounds.x, draw_bounds.y + draw_bounds.h, draw_bounds.w, popup_h };
        if ((popup.y + popup.h) > ctx->height) popup.y = draw_bounds.y - popup_h;
        if (popup.y < 0) popup.y = 0;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, popup.x, popup.y, popup.w, popup_h, 0xFF172233u, clip);
        ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, popup, 1, c->border_color, clip);
        const ui_rect_t popup_clip = ui_rect_intersect(clip, popup);
        for (int32_t i = 0; i < c->item_count; ++i) {
            const int32_t row_y = popup.y + (i * item_h);
            if (row_y >= (popup.y + popup.h)) break;
            const uint32_t row_bg = (i == c->selected_index) ? 0xFF2F5C88u : ((i & 1) ? 0xFF1F2E43u : 0xFF1A283B);
            ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, popup.x, row_y, popup.w, item_h, row_bg, popup_clip);
            ui_draw_text_clip(ctx,
                              popup.x + 6, row_y + (item_h - ctx->font_px) / 2,
                              c->list_items[i] ? c->list_items[i] : "", 0xFFFFFFFFu, popup_clip);
        }
    }
}

#endif /* WASMOS_LIBUI_DROPDOWN_H */
