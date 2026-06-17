#ifndef WASMOS_LIBUI_BUTTON_H
#define WASMOS_LIBUI_BUTTON_H

/* libui_button.h - Button component specific rendering and behavior.
 * Included by libui.h after core generic facilities are defined.
 */

static inline void
ui_render_button(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    ui_text_data_t *td = (ui_text_data_t *)c->component_data;
    /* Use bg_color as the button fill; darken 25 % when pressed.
     * The outer ui_fill_rect_clip (in ui_render_component_clip) already filled
     * with bg_color, so we only need a separate fill when pressed. */
    if (c->pressed) {
        uint32_t col = c->bg_color;
        const uint32_t r = ((col >> 16) & 0xFFu) * 3u / 4u;
        const uint32_t g = ((col >>  8) & 0xFFu) * 3u / 4u;
        const uint32_t b = ( col        & 0xFFu) * 3u / 4u;
        col = (col & 0xFF000000u) | (r << 16) | (g << 8) | b;
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                          draw_bounds.x, draw_bounds.y, draw_bounds.w, draw_bounds.h, col, clip);
    }
    /* Centre text horizontally and vertically. */
    const char *text = (td && td->text) ? td->text : "";
    const int32_t tw = ui_measure_text_width(ctx, text);
    const int32_t tx = draw_bounds.x + (draw_bounds.w - tw) / 2;
    const int32_t ty = draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2;
    ui_draw_text_clip(ctx, tx, ty, text, c->fg_color, clip);
}

static inline void
ui_button_handle_pointer_release(ui_context_t *ctx, ui_component_t *c)
{
    if (c->on_click) c->on_click(ctx, c->id, c->on_click_user);
}

#endif /* WASMOS_LIBUI_BUTTON_H */
