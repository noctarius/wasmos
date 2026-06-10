#ifndef WASMOS_LIBUI_BUTTON_H
#define WASMOS_LIBUI_BUTTON_H

/* libui_button.h - Button component specific rendering and behavior.
 * Included by libui.h after core generic facilities are defined.
 */

typedef ui_text_data_t ui_button_data_t;

static inline void
ui_render_button(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    ui_text_data_t *td = (ui_text_data_t *)c->component_data;
    const uint32_t inner = c->pressed ? 0xFF2B6AA0u : 0xFF4B91CCu;
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                      draw_bounds.x + 2, draw_bounds.y + 2, draw_bounds.w - 4, draw_bounds.h - 4, inner, clip);
    ui_draw_text_clip(ctx,
                      draw_bounds.x + c->padding_px,
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      (td && td->text) ? td->text : "",
                      0xFFFFFFFFu,
                      clip);
}

static inline void
ui_button_handle_pointer_release(ui_context_t *ctx, ui_component_t *c)
{
    if (c->on_click) c->on_click(ctx, c->id, c->on_click_user);
}

#endif /* WASMOS_LIBUI_BUTTON_H */
