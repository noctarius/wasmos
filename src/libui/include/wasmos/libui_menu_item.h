#ifndef WASMOS_LIBUI_MENU_ITEM_H
#define WASMOS_LIBUI_MENU_ITEM_H

/* libui_menu_item.h - Menu item component specific rendering (including popup). */

/* Prototype for helper used by render (defined later in this header). */
static inline ui_rect_t ui_menu_item_popup_bounds(const ui_context_t *ctx, const ui_component_t *c);

static inline void
ui_render_menu_item(ui_context_t *ctx, const ui_component_t *c, ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y)
{
    (void)offset_y;
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)c->component_data;
    if (d && (d->dropdown_open || c->pressed)) {
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                          draw_bounds.x + 1, draw_bounds.y + 1,
                          draw_bounds.w - 2, draw_bounds.h - 2, 0xFF2A4060u, clip);
    }
    ui_draw_text_clip(ctx,
                      draw_bounds.x + (c->padding_px > 0 ? c->padding_px : 8),
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      (d && d->text.text) ? d->text.text : "",
                      c->fg_color ? c->fg_color : 0xFFDDE8F0u,
                      clip);
    if (d && d->dropdown_open && d->list.count > 0) {
        const int32_t item_h = 22;
        const ui_rect_t popup = ui_menu_item_popup_bounds(ctx, c);
        if (popup.w > 0 && popup.h > 0) {
            const ui_rect_t popup_clip = ui_rect_intersect(clip, popup);
            ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                              popup.x, popup.y, popup.w, popup.h, 0xFF1A2840u, clip);
            ui_stroke_rect_clip(ctx->mapped_base, ctx->width, ctx->height, popup, 1, 0xFF4A6080u, clip);
            for (int32_t i = 0; i < d->list.count; ++i) {
                const int32_t row_y = popup.y + (i * item_h);
                const uint32_t row_bg = (i == d->list.selected) ? 0xFF2F5C88u : 0xFF1A2840u;
                ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height,
                                  popup.x + 1, row_y, popup.w - 2, item_h, row_bg, popup_clip);
                ui_draw_text_clip(ctx,
                                  popup.x + 8, row_y + (item_h - ctx->font_px) / 2,
                                  d->list.items[i] ? d->list.items[i] : "", 0xFFFFFFFFu, popup_clip);
            }
        }
    }
}

static inline ui_rect_t
ui_menu_item_popup_bounds(const ui_context_t *ctx, const ui_component_t *c)
{
    ui_rect_t popup = { 0, 0, 0, 0 };
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)c->component_data;
    if (!ctx || !c || c->type != UI_COMPONENT_MENU_ITEM || !d || !d->dropdown_open || d->list.count <= 0) return popup;
    const int32_t item_h = 22;
    const int32_t pw = c->bounds.w > 160 ? c->bounds.w : 160;
    popup.x = c->bounds.x;
    popup.y = c->bounds.y + c->bounds.h;
    popup.w = pw;
    popup.h = d->list.count * item_h;
    return popup;
}

/* Component-owned popup hit test helper.
 * Core find_*_at and event code can call this for menu-item-specific popup bounds checking. */
static inline bool
ui_menu_item_popup_contains(const ui_context_t *ctx, const ui_component_t *c, int32_t x, int32_t y)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)c->component_data;
    if (!d || !d->dropdown_open) return false;
    const ui_rect_t popup = ui_menu_item_popup_bounds(ctx, c);
    return popup.w > 0 && popup.h > 0 && ui_point_in_bounds(x, y, popup);
}

/* Component-owned helper to map a point in the popup to a selected item index, or -1 if not in popup.
 * Allows core event code to ask the component for its selection logic without duplicating popup math. */
static inline int32_t
ui_menu_item_get_selection_from_point(const ui_context_t *ctx, const ui_component_t *c, int32_t x, int32_t y)
{
    if (!ui_menu_item_popup_contains(ctx, c, x, y)) return -1;
    const ui_rect_t popup = ui_menu_item_popup_bounds(ctx, c);
    const int32_t item_h = 22;
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)c->component_data;
    const int32_t idx = (y - popup.y) / item_h;
    if (idx >= 0 && idx < d->list.count) return idx;
    return -1;
}

/* Component-owned helpers for header activation and popup pick.
 * Core does the sibling close orchestration and identification, component owns its open/close/pick reaction. */
static inline void
ui_menu_item_open_dropdown(ui_context_t *ctx, ui_component_t *mi)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (d && d->list.count > 0) {
        d->dropdown_open = 1;
        ui_mark_dirty(ctx);
    }
}

static inline void
ui_menu_item_close_dropdown(ui_context_t *ctx, ui_component_t *mi)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (d) {
        d->dropdown_open = 0;
        ui_mark_dirty(ctx);
    }
}

static inline void
ui_menu_item_pick_and_invoke(ui_context_t *ctx, ui_component_t *mi, int32_t idx)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (d && idx >= 0 && idx < d->list.count) {
        d->list.selected = idx;
        d->dropdown_open = 0;
        if (mi->on_click) mi->on_click(ctx, mi->id, mi->on_click_user);
        ui_mark_dirty(ctx);
    }
}

/* Component-owned handling for pointer release anywhere in the menu system.
 * Core calls this on every pointer button release so that menu items own:
 * - detecting a click on a menu bar item (toggle its dropdown)
 * - detecting a click inside an open popup (pick the item)
 * - closing sibling menus before opening another
 * Core still owns the generic "clear pressed flags / active scroll" after any release,
 * and the separate dropdown outside-click close.
 */
static inline void
ui_menu_item_handle_pointer_release(ui_context_t *ctx, int32_t x, int32_t y)
{
    int32_t mi_id2 = -1;
    for (int32_t ci2 = 0; ci2 < ctx->component_count; ++ci2) {
        ui_component_t *mc = &ctx->components[ci2];
        if (!mc->in_use || mc->type != UI_COMPONENT_MENU_ITEM) continue;
        ui_menu_item_data_t *md = (ui_menu_item_data_t *)mc->component_data;
        if (ui_point_in_bounds(x, y, mc->bounds)) { mi_id2 = mc->id; break; }
        if (md && ui_menu_item_popup_contains(ctx, mc, x, y)) { mi_id2 = mc->id; break; }
    }
    if (mi_id2 > 0) {
        ui_component_t *mi2 = ui_component_by_id(ctx, mi_id2);
        ui_menu_item_data_t *d2 = mi2 ? (ui_menu_item_data_t *)mi2->component_data : NULL;
        if (mi2 && d2) {
            if (ui_point_in_bounds(x, y, mi2->bounds)) {
                const int32_t will_open = !d2->dropdown_open;
                for (int32_t ci3 = 0; ci3 < ctx->component_count; ++ci3) {
                    if (ctx->components[ci3].in_use && ctx->components[ci3].type == UI_COMPONENT_MENU_ITEM)
                        ui_menu_item_close_dropdown(ctx, &ctx->components[ci3]);
                }
                if (will_open) ui_menu_item_open_dropdown(ctx, mi2);
                ui_mark_dirty(ctx);
            } else if (d2->dropdown_open) {
                const int32_t idx2 = ui_menu_item_get_selection_from_point(ctx, mi2, x, y);
                if (idx2 >= 0) {
                    ui_menu_item_pick_and_invoke(ctx, mi2, idx2);
                }
            }
        }
    }
}

#endif /* WASMOS_LIBUI_MENU_ITEM_H */
