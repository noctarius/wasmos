#ifndef WASMOS_LIBUI_DROPDOWN_H
#define WASMOS_LIBUI_DROPDOWN_H

/* libui_dropdown.h - Dropdown component specific rendering (including popup). */

/* Render op for UI_COMPONENT_DROPDOWN: the closed field showing the selected
 * item (falling back to the component's placeholder text when nothing is
 * selected), a 'v'/'^' marker, a focus highlight, and — while open — the popup
 * list drawn straight into this window's framebuffer below the field, flipped
 * above it when it would fall off the bottom. The popup is capped at 120 px, so
 * a longer list is truncated rather than scrolled. Paints its own border, so
 * the core skips the generic one. */
static inline void ui_render_dropdown(ui_context_t* ctx, const ui_component_t* c,
                                      ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y) {
    (void)offset_y;
    ui_dropdown_data_t* d = (ui_dropdown_data_t*)c->component_data;
    const int32_t active = (ctx->focused_component_id == c->id);
    const uint32_t inner = active ? 0xFF1F3148u : 0xFF1C2738u;
    const uint32_t outline = active ? 0xFF89C9FFu : c->border_color;
    ui_fill_rect_clip(ctx->mapped_base,
                      ctx->width,
                      ctx->height,
                      draw_bounds.x + 1,
                      draw_bounds.y + 1,
                      draw_bounds.w - 2,
                      draw_bounds.h - 2,
                      inner,
                      clip);
    ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds, 1, outline, clip);

    const char* selected = "";
    if (d) {
        if (d->list.selected >= 0 && d->list.selected < d->list.count && d->list.items &&
            d->list.items[d->list.selected]) {
            selected = d->list.items[d->list.selected];
        } else if (d->text.text) {
            selected = d->text.text;
        }
    }
    ui_draw_text_clip(ctx,
                      draw_bounds.x + c->padding_px,
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      selected,
                      0xFFFFFFFFu,
                      clip);
    ui_draw_text_clip(ctx,
                      draw_bounds.x + draw_bounds.w - 12,
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      (d && d->dropdown_open) ? "^" : "v",
                      0xFF9CB6CEu,
                      clip);

    if (d && d->dropdown_open && d->list.count > 0) {
        const int32_t item_h = 20;
        const int32_t popup_h = (d->list.count * item_h > 120) ? 120 : (d->list.count * item_h);
        ui_rect_t popup = {draw_bounds.x, draw_bounds.y + draw_bounds.h, draw_bounds.w, popup_h};
        if ((popup.y + popup.h) > ctx->height)
            popup.y = draw_bounds.y - popup_h;
        if (popup.y < 0)
            popup.y = 0;
        ui_fill_rect_clip(ctx->mapped_base,
                          ctx->width,
                          ctx->height,
                          popup.x,
                          popup.y,
                          popup.w,
                          popup_h,
                          0xFF172233u,
                          clip);
        ui_stroke_rect_clip(
            ctx->mapped_base, ctx->width, ctx->height, popup, 1, c->border_color, clip);
        const ui_rect_t popup_clip = ui_rect_intersect(clip, popup);
        for (int32_t i = 0; i < d->list.count; ++i) {
            const int32_t row_y = popup.y + (i * item_h);
            if (row_y >= (popup.y + popup.h))
                break;
            const uint32_t row_bg =
                (i == d->list.selected) ? 0xFF2F5C88u : ((i & 1) ? 0xFF1F2E43u : 0xFF1A283B);
            ui_fill_rect_clip(ctx->mapped_base,
                              ctx->width,
                              ctx->height,
                              popup.x,
                              row_y,
                              popup.w,
                              item_h,
                              row_bg,
                              popup_clip);
            ui_draw_text_clip(ctx,
                              popup.x + 6,
                              row_y + (item_h - ctx->font_px) / 2,
                              d->list.items[i] ? d->list.items[i] : "",
                              0xFFFFFFFFu,
                              popup_clip);
        }
    }
}

/* Layout op for UI_COMPONENT_DROPDOWN. The dropdown has no child components, so
 * this only clamps the selection into [0, count) — which turns the initial -1
 * into row 0 once the list is non-empty. */
static inline void ui_layout_dropdown(ui_context_t* ctx, ui_component_t* p) {
    (void)ctx;
    ui_dropdown_data_t* d = (ui_dropdown_data_t*)p->component_data;
    if (!d)
        return;
    if (d->list.selected < 0)
        d->list.selected = 0;
    if (d->list.selected >= d->list.count)
        d->list.selected = (d->list.count > 0) ? (d->list.count - 1) : 0;
    /* FIXME: no-op self-assignment. The dropdown popup has no scroll state to
     * clamp, so this line has no effect and can go. */
    d->list.capacity = d->list.capacity;
}

/* Rectangle the open popup occupies, in the owning window's coordinates. The
 * popup matches the field's x and width, sits directly below it, flips above
 * when it would overflow the window and is pinned to y = 0 if it still does not
 * fit. Returns an all-zero (empty) rect when the component is not a DROPDOWN,
 * is closed, or has no items — callers test w and h rather than a flag. */
static inline ui_rect_t ui_dropdown_popup_bounds(const ui_context_t* ctx, const ui_component_t* c) {
    ui_rect_t popup = {0, 0, 0, 0};
    ui_dropdown_data_t* d = (ui_dropdown_data_t*)c->component_data;
    if (!ctx || !c || c->type != UI_COMPONENT_DROPDOWN || !d || !d->dropdown_open ||
        d->list.count <= 0)
        return popup;
    const int32_t item_h = 20;
    popup.x = c->bounds.x;
    popup.w = c->bounds.w;
    popup.h = (d->list.count * item_h > 120) ? 120 : (d->list.count * item_h);
    popup.y = c->bounds.y + c->bounds.h;
    if ((popup.y + popup.h) > ctx->height)
        popup.y = c->bounds.y - popup.h;
    if (popup.y < 0)
        popup.y = 0;
    return popup;
}

/* True when (x, y) lies inside the open popup. The dropdown popup is drawn into
 * the owning window's own framebuffer, so its bounds are in that window's
 * coordinates and the core find_*_at walkers can hit-test it directly — unlike
 * a menu-item popup, which lives in a separate compositor window. */
static inline bool ui_dropdown_popup_contains(const ui_context_t* ctx, const ui_component_t* c,
                                              int32_t x, int32_t y) {
    ui_dropdown_data_t* d = (ui_dropdown_data_t*)c->component_data;
    if (!d || !d->dropdown_open)
        return false;
    const ui_rect_t popup = ui_dropdown_popup_bounds(ctx, c);
    return popup.w > 0 && popup.h > 0 && ui_point_in_bounds(x, y, popup);
}

/* Component-owned reaction handlers.
 * Core calls these after routing (find target, focus, pressed, etc.) so dropdown owns
 * its specific behavior for pointer (toggle/pick) and keys (open/close/nav).
 *
 * A press on the field toggles the popup; a press on a row of the open popup
 * selects it (by 20 px row height) and closes the popup. A press elsewhere is
 * ignored here — closing on an outside click is driven by the core through
 * ui_dropdown_close_all_open(). Marks the context dirty on any state change. */
static inline void ui_dropdown_handle_pointer_press(ui_context_t* ctx, ui_component_t* c,
                                                    int32_t pointer_x, int32_t pointer_y) {
    ui_dropdown_data_t* d = (ui_dropdown_data_t*)c->component_data;
    if (!d)
        return;
    if (ui_point_in_bounds(pointer_x, pointer_y, c->bounds)) {
        d->dropdown_open = d->dropdown_open ? 0 : 1;
        ui_mark_dirty(ctx);
    } else if (d->dropdown_open) {
        const ui_rect_t popup = ui_dropdown_popup_bounds(ctx, c);
        if (popup.w > 0 && popup.h > 0 && ui_point_in_bounds(pointer_x, pointer_y, popup)) {
            const int32_t idx = (pointer_y - popup.y) / 20;
            if (idx >= 0 && idx < d->list.count)
                d->list.selected = idx;
            d->dropdown_open = 0;
            ui_mark_dirty(ctx);
        }
    }
}

/* Key op for a focused UI_COMPONENT_DROPDOWN. `key` is the packed
 * GFX_EVENT_KEY code; only its character byte is used. Escape closes the popup,
 * Return/Enter/Space toggles it, and 'j'/'k' (either case) move the selection
 * down/up without wrapping. Arrow keys carry no character and therefore do
 * nothing. Selection changes take effect immediately, whether or not the popup
 * is open, and no callback fires — the application reads list.selected. */
static inline void ui_dropdown_handle_key(ui_context_t* ctx, ui_component_t* c, uint32_t key) {
    ui_dropdown_data_t* d = (ui_dropdown_data_t*)c->component_data;
    if (!d)
        return;
    const uint32_t ch = ui_key_char(key);
    if (ch == 27u) {
        if (d->dropdown_open) {
            d->dropdown_open = 0;
            ui_mark_dirty(ctx);
        }
    } else if (ch == '\r' || ch == '\n' || ch == ' ') {
        d->dropdown_open = d->dropdown_open ? 0 : 1;
        ui_mark_dirty(ctx);
    } else if (ch == 'j' || ch == 'J') {
        if (d->list.count > 0 && d->list.selected < (d->list.count - 1)) {
            d->list.selected += 1;
            ui_mark_dirty(ctx);
        }
    } else if (ch == 'k' || ch == 'K') {
        if (d->list.count > 0 && d->list.selected > 0) {
            d->list.selected -= 1;
            ui_mark_dirty(ctx);
        }
    }
}

/* Simple close helper for orchestration in core (e.g. click outside).
 * Closes the popup and marks the context dirty only if it was open, so calling
 * it on an already-closed dropdown costs no repaint. The component's type is
 * not checked; the callers below filter by kind. */
static inline void ui_dropdown_close(ui_context_t* ctx, ui_component_t* c) {
    ui_dropdown_data_t* d = (ui_dropdown_data_t*)c->component_data;
    if (d && d->dropdown_open) {
        d->dropdown_open = 0;
        ui_mark_dirty(ctx);
    }
}

/* Component-owned helper: close every open dropdown.
 * Core calls this on pointer release when the click was a miss (no clickable hit),
 * so that dropdown "owns" knowing which of its instances are open and how to close them.
 * Scans the whole component pool and closes every open DROPDOWN, including ones
 * in unrelated subtrees. */
static inline void ui_dropdown_close_all_open(ui_context_t* ctx) {
    for (int32_t i = 0; i < ctx->component_count; ++i) {
        ui_component_t* c = &ctx->components[i];
        if (c->in_use && c->type == UI_COMPONENT_DROPDOWN) {
            ui_dropdown_close(ctx, c);
        }
    }
}

#endif /* WASMOS_LIBUI_DROPDOWN_H */
