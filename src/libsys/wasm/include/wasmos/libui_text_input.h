#ifndef WASMOS_LIBUI_TEXT_INPUT_H
#define WASMOS_LIBUI_TEXT_INPUT_H

/* libui_text_input.h - Text input component specific rendering. */

static inline void
ui_render_text_input(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    const int32_t active = (ctx->focused_component_id == c->id);
    const uint32_t inner = active ? 0xFF1F3148u : 0xFF1C2738u;
    const uint32_t outline = active ? 0xFF89C9FFu : c->border_color;
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                      draw_bounds.x + 1, draw_bounds.y + 1, draw_bounds.w - 2, draw_bounds.h - 2, inner, clip);
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, 1, outline, clip);
    const int32_t tx = draw_bounds.x + c->padding_px;
    const int32_t ty = draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2;
    ui_draw_text_clip(ctx, tx, ty, c->text ? c->text : "", 0xFFFFFFFFu, clip);
    if (active) {
        const int32_t caret_x = tx + ui_measure_text_width(ctx, c->text ? c->text : "");
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, caret_x, ty - 1, 1, ctx->font_px + 2, 0xFFFFFFFFu, clip);
    }
}

#endif /* WASMOS_LIBUI_TEXT_INPUT_H */
