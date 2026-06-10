#ifndef WASMOS_LIBUI_LABEL_H
#define WASMOS_LIBUI_LABEL_H

/* libui_label.h - Label component specific rendering and behavior.
 * Included by libui.h after core generic facilities are defined.
 * Component-specific code lives here so the core stays small as we add widgets.
 */

typedef ui_text_data_t ui_label_data_t;  /* label only needs text for display */

static inline void
ui_render_label(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    ui_text_data_t *td = (ui_text_data_t *)c->component_data;
    ui_draw_text_clip(ctx,
                      draw_bounds.x + c->padding_px,
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      (td && td->text) ? td->text : "",
                      c->fg_color,
                      clip);
}

#endif /* WASMOS_LIBUI_LABEL_H */
