#ifndef WASMOS_LIBUI_MENU_BAR_H
#define WASMOS_LIBUI_MENU_BAR_H

/* libui_menu_bar.h - Menu bar component specific rendering. */

/* Render op for UI_COMPONENT_MENU_BAR: a 1 px separator along the bottom edge,
 * every child menu item through the core clip walker, then the clock text
 * right-aligned with a 10 px inset (skipped when it would start at or left of
 * x = 0). Descends into children itself, so the core skips its generic border
 * and child pass. */
static inline void ui_render_menu_bar(ui_context_t* ctx, const ui_component_t* c,
                                      ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y) {
    ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds.x,
                      draw_bounds.y + draw_bounds.h - 1, draw_bounds.w, 1, 0xFF304050u, clip);
    int32_t mi_child_id = c->first_child_id;
    while (mi_child_id > 0) {
        ui_render_component_clip(ctx, mi_child_id, clip, offset_y);
        ui_component_t* mi_child = ui_component_by_id(ctx, mi_child_id);
        if (!mi_child)
            break;
        mi_child_id = mi_child->next_sibling_id;
    }

    const ui_menu_bar_data_t* d = (const ui_menu_bar_data_t*)c->component_data;
    if (d && d->clock_text[0]) {
        const int32_t tw = ui_measure_text_width(ctx, d->clock_text);
        const int32_t tx = draw_bounds.x + draw_bounds.w - tw - 10;
        const int32_t ty = draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2;
        if (tx > 0) {
            ui_draw_text_clip(ctx, tx, ty, d->clock_text, 0xFF99B8D0u, clip);
        }
    }
}

/* Set the menu bar's right-aligned status text and mark the context dirty.
 * Type-checked: does nothing unless `id` is a MENU_BAR. A NULL `text` clears
 * the string. Longer input is truncated to 23 bytes, not refused, and the cut
 * is by byte, so it can split a multi-byte UTF-8 sequence. The application owns
 * `text`; a copy is stored. libui never updates this by itself — the caller
 * decides when to re-read a clock and push the new string. */
static inline void ui_menu_bar_set_clock(ui_context_t* ctx, int32_t id, const char* text) {
    ui_component_t* c = ui_component_by_id(ctx, id);
    if (!c || c->type != UI_COMPONENT_MENU_BAR || !c->component_data)
        return;
    ui_menu_bar_data_t* d = (ui_menu_bar_data_t*)c->component_data;
    int32_t i = 0;
    /* Truncates at sizeof(clock_text) - 1 = 23 characters. */
    while (i < 23 && text && text[i]) {
        d->clock_text[i] = text[i];
        i++;
    }
    d->clock_text[i] = '\0';
    ui_mark_dirty(ctx);
}

/* Layout op for UI_COMPONENT_MENU_BAR: places children left to right from the
 * bar's left padding, separated by gap_px, each spanning the bar's full height.
 * A child's preferred_h is read as its WIDTH here — 0 means hidden (the item is
 * skipped, keeping whatever bounds it last had, so it stays hit-testable at its
 * old position), and any value of 4 or less falls back to 80 px. Children are
 * not recursed into; a menu item's own entries live in its popup window. */
static inline void ui_layout_menu_bar(ui_context_t* ctx, ui_component_t* p) {
    int32_t x_cur = p->bounds.x + p->padding_px;
    int32_t child_id2 = p->first_child_id;
    while (child_id2 > 0) {
        ui_component_t* mc = ui_component_by_id(ctx, child_id2);
        if (!mc)
            break;
        if (mc->preferred_h == 0) {
            child_id2 = mc->next_sibling_id;
            continue;
        } /* hidden item */
        /* preferred_h repurposed as preferred width for menu items */
        const int32_t iw = mc->preferred_h > 4 ? mc->preferred_h : 80;
        mc->bounds.x = x_cur;
        mc->bounds.y = p->bounds.y;
        mc->bounds.w = iw;
        mc->bounds.h = p->bounds.h;
        x_cur += iw + p->gap_px;
        child_id2 = mc->next_sibling_id;
    }
}

#endif /* WASMOS_LIBUI_MENU_BAR_H */
