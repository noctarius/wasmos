#ifndef WASMOS_LIBUI_TEXT_INPUT_H
#define WASMOS_LIBUI_TEXT_INPUT_H

/* libui_text_input.h - Text input component specific rendering. */

typedef ui_text_data_t ui_text_input_data_t;

static inline void
ui_render_text_input(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    ui_text_data_t *td = (ui_text_data_t *)c->component_data;
    const int32_t active = (ctx->focused_component_id == c->id);
    const uint32_t inner = active ? 0xFF1F3148u : 0xFF1C2738u;
    const uint32_t outline = active ? 0xFF89C9FFu : c->border_color;
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                      draw_bounds.x + 1, draw_bounds.y + 1, draw_bounds.w - 2, draw_bounds.h - 2, inner, clip);
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, 1, outline, clip);
    const int32_t tx = draw_bounds.x + c->padding_px;
    const int32_t ty = draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2;
    ui_draw_text_clip(ctx, tx, ty, (td && td->text) ? td->text : "", 0xFFFFFFFFu, clip);
    if (active) {
        const int32_t caret_x = tx + ui_measure_text_width(ctx, (td && td->text) ? td->text : "");
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, caret_x, ty - 1, 1, ctx->font_px + 2, 0xFFFFFFFFu, clip);
    }
}

/* Component-owned key handler for focused text input.
 * Core dispatches here for the specific editing behavior (backspace, append printable). */
static inline void
ui_text_input_handle_key(ui_context_t *ctx, ui_component_t *c, uint32_t key)
{
    ui_text_data_t *td = (ui_text_data_t *)c->component_data;
    if (!td) return;

    if (key == 8u || key == 127u) {
        if (td->text_len > 0) {
            td->text_len = ui_utf8_prev_boundary(td->text, td->text_len);
            td->text[td->text_len] = '\0';
            ui_mark_dirty(ctx);
        }
    } else if (key >= 32u && key <= 0x10FFFFu) {
        uint8_t enc[4];
        const int32_t enc_len = ui_utf8_encode(key, enc);
        if (enc_len <= 0) return;
        const int32_t need = td->text_len + enc_len + 1;
        if (td->text_cap < need) {
            int32_t new_cap = td->text_cap > 0 ? td->text_cap : UI_TEXT_INITIAL_CAP;
            while (new_cap < need) new_cap *= 2;
            char *new_text = (char *)malloc((size_t)new_cap);
            if (new_text) {
                if (td->text) memcpy(new_text, td->text, (size_t)td->text_len + 1);
                if (td->text) free(td->text);
                td->text = new_text;
                td->text_cap = new_cap;
            }
        }
        if (td->text_cap >= need) {
            memcpy(td->text + td->text_len, enc, (size_t)enc_len);
            td->text_len += enc_len;
            td->text[td->text_len] = '\0';
            ui_mark_dirty(ctx);
        }
    }
}

#endif /* WASMOS_LIBUI_TEXT_INPUT_H */
