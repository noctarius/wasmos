#ifndef WASMOS_LIBUI_CHECKBOX_H
#define WASMOS_LIBUI_CHECKBOX_H

/* libui_checkbox.h - Checkbox component specific rendering. */

typedef struct {
    ui_text_data_t text;
    int32_t checked;
} ui_checkbox_data_t;

static inline void
ui_render_checkbox(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    ui_checkbox_data_t *d = (ui_checkbox_data_t *)c->component_data;
    const int32_t box = draw_bounds.h > 16 ? 16 : draw_bounds.h - 4;
    const int32_t bx = draw_bounds.x + c->padding_px;
    const int32_t by = draw_bounds.y + (draw_bounds.h - box) / 2;
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, bx, by, box, box, 0xFF2B3440u, clip);
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, (ui_rect_t){bx, by, box, box}, 1, 0xFF9CB6CEu, clip);
    if (d && d->checked) {
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, bx + 4, by + 4, box - 8, box - 8, 0xFF66CC88u, clip);
    }
    ui_draw_text_clip(ctx,
                      bx + box + 8,
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      (d && d->text.text) ? d->text.text : "",
                      c->fg_color,
                      clip);
}

/* Component-owned toggle for checkbox (before its on_click). */
static inline void
ui_checkbox_toggle(ui_context_t *ctx, ui_component_t *c)
{
    ui_checkbox_data_t *d = (ui_checkbox_data_t *)c->component_data;
    if (d) {
        d->checked = !d->checked;
        ui_mark_dirty(ctx);
    }
}

static inline void
ui_checkbox_handle_pointer_release(ui_context_t *ctx, ui_component_t *c)
{
    ui_checkbox_toggle(ctx, c);
    if (c->on_click) c->on_click(ctx, c->id, c->on_click_user);
}

#endif /* WASMOS_LIBUI_CHECKBOX_H */

