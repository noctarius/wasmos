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
                const uint32_t row_bg = (d->list.selected >= 0 && i == d->list.selected) ? 0xFF2F5C88u : 0xFF1A2840u;
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

/* ---- popup window management ---- */

static inline void
ui_menu_item_popup_render(ui_context_t *ctx, ui_component_t *mi, int32_t hovered)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (!d || !d->popup_base) return;

    const int32_t item_h = 22;
    const ui_rect_t full = {0, 0, d->popup_w, d->popup_h};

    ui_fill_rect(d->popup_base, d->popup_w, d->popup_h, 0, 0, d->popup_w, d->popup_h, 0xFF1A2840u);
    ui_stroke_rect_clip(d->popup_base, d->popup_w, d->popup_h, full, 1, 0xFF4A6080u, full);

    uint8_t *saved_base = ctx->mapped_base;
    int32_t  saved_w    = ctx->width;
    int32_t  saved_h    = ctx->height;
    ctx->mapped_base = d->popup_base;
    ctx->width       = d->popup_w;
    ctx->height      = d->popup_h;

    for (int32_t i = 0; i < d->list.count; ++i) {
        if (i == hovered) {
            ui_fill_rect(d->popup_base, d->popup_w, d->popup_h,
                         1, i * item_h, d->popup_w - 2, item_h, 0xFF2F5C88u);
        }
        ui_draw_text_clip(ctx, 8,
                          i * item_h + (item_h - ctx->font_px) / 2,
                          (d->list.items[i] ? d->list.items[i] : ""),
                          0xFFFFFFFFu, full);
    }

    ctx->mapped_base = saved_base;
    ctx->width       = saved_w;
    ctx->height      = saved_h;
}

static inline void
ui_menu_item_popup_present(ui_context_t *ctx, ui_menu_item_data_t *d)
{
    if (!d || !d->popup_base || d->popup_win_id == 0 || d->popup_buf_id == 0) return;
    int32_t status = 0;
    wasmos_shmem_flush(d->popup_shmem_id, (int32_t)(uintptr_t)d->popup_base,
                       d->popup_w * d->popup_h * 4);
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                GFX_IPC_PRESENT_WINDOW, d->popup_win_id, d->popup_buf_id, 0, 0,
                &status, 0, 0, 0);
}

static inline void
ui_menu_item_popup_close(ui_context_t *ctx, ui_component_t *mi)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (!d || d->popup_win_id == 0) return;
    int32_t status = 0;
    if (d->popup_buf_id > 0) {
        ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, d->popup_buf_id, 0, 0, 0,
                    &status, 0, 0, 0);
        d->popup_buf_id = 0;
    }
    if (d->popup_shmem_id > 0) {
        wasmos_shmem_unmap(d->popup_shmem_id);
        d->popup_shmem_id = 0;
    }
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                GFX_IPC_DESTROY_WINDOW, d->popup_win_id, 0, 0, 0,
                &status, 0, 0, 0);
    d->popup_win_id       = 0;
    d->popup_base         = NULL;
    d->popup_w            = 0;
    d->popup_h            = 0;
    d->popup_hovered      = -1;
    d->popup_prev_buttons = 0;
}

static inline void
ui_menu_item_popup_open(ui_context_t *ctx, ui_component_t *mi)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (!d || !d->dropdown_open || d->list.count <= 0) return;
    ui_menu_item_popup_close(ctx, mi);

    const int32_t item_h  = 22;
    const int32_t min_w   = 160;
    const int32_t popup_x = mi->bounds.x;
    const int32_t popup_y = mi->bounds.y + mi->bounds.h; /* below the bar item, no overlap */
    const int32_t popup_w = mi->bounds.w > min_w ? mi->bounds.w : min_w;
    const int32_t popup_h = d->list.count * item_h;      /* list rows only, no header */
    int32_t status = 0, a1 = 0, a2 = 0, a3 = 0;

    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_CREATE_WINDOW, popup_w, popup_h,
                    (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status, &a1, &a2, &a3) != 0 || status != GFX_STATUS_OK) return;
    const int32_t win_id = a1;

    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_SET_WINDOW_FLAGS, win_id,
                    (int32_t)(GFX_WINDOW_FLAG_TOPMOST | GFX_WINDOW_FLAG_NO_CHROME | GFX_WINDOW_FLAG_NO_TASK_LIST), 0, 0,
                    &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) goto fail;

    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_MOVE_WINDOW, win_id, popup_x, popup_y, 0,
                    &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) goto fail;

    int32_t buf_id = 0, shmem_id = 0, stride = 0;
    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_ALLOC_SHARED_BUFFER, win_id, popup_w, popup_h, 0,
                    &status, &buf_id, &shmem_id, &stride) != 0 || status != GFX_STATUS_OK) goto fail;

    const int32_t bytes = (popup_w * popup_h * 4 + (UI_PAGE_SIZE - 1)) & ~(UI_PAGE_SIZE - 1);
    const int32_t mapped = wasmos_shmem_map_auto(shmem_id, bytes);
    if (mapped < 0) {
        ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, buf_id, 0, 0, 0, &status, 0, 0, 0);
        goto fail;
    }

    d->popup_win_id       = win_id;
    d->popup_buf_id       = buf_id;
    d->popup_shmem_id     = shmem_id;
    d->popup_base         = (uint8_t *)(uintptr_t)(uint32_t)mapped;
    d->popup_w            = popup_w;
    d->popup_h            = popup_h;
    d->popup_hovered      = -1;
    d->popup_prev_buttons = 0;
    d->popup_flushing     = 1; /* discard stale button-down events from before popup opened */

    ui_menu_item_popup_render(ctx, mi, -1);
    ui_menu_item_popup_present(ctx, d);
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                GFX_IPC_FOCUS_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
    return;
fail:
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                GFX_IPC_DESTROY_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
}

/* Called when popup window loses focus (user clicked outside). */
static inline void
ui_menu_item_dismiss_popup(ui_context_t *ctx, ui_component_t *mi)
{
    for (int32_t i = 0; i < ctx->component_count; ++i) {
        ui_component_t *c = &ctx->components[i];
        if (!c->in_use || c->type != UI_COMPONENT_MENU_ITEM) continue;
        ui_menu_item_data_t *cd = (ui_menu_item_data_t *)c->component_data;
        if (!cd) continue;
        cd->dropdown_open = 0;
        if (cd->popup_win_id > 0) ui_menu_item_popup_close(ctx, c);
    }
    ui_mark_dirty(ctx);
}

/* Reconcile popup window with dropdown_open state. Called from ui_loop_drain. */
static inline void
ui_menu_item_sync_popup(ui_context_t *ctx, ui_component_t *mi)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (!d) return;
    if (!d->dropdown_open) {
        if (d->popup_win_id > 0) ui_menu_item_popup_close(ctx, mi);
        return;
    }
    const int32_t expected_h = d->list.count * 22;
    if (d->popup_win_id == 0) {
        ui_menu_item_popup_open(ctx, mi);
    } else if (d->popup_h != expected_h) {
        ui_menu_item_popup_close(ctx, mi);
        ui_menu_item_popup_open(ctx, mi);
    }
}

/* Handle a POINTER event that came from the popup window. */
static inline void
ui_menu_item_handle_popup_event(ui_context_t *ctx, ui_component_t *mi,
                                 const wasmos_ipc_message_t *msg)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)mi->component_data;
    if (!d || d->popup_win_id == 0) return;

    const int32_t  px        = ui_u16_lo(msg->arg2);
    const int32_t  py        = ui_u16_hi(msg->arg2);
    const uint32_t buttons   = (uint32_t)msg->arg3;
    (void)px;

    /* Flush stale button-down events that were queued for the bar window
     * before the popup opened (user holding the mouse while bar was focused).
     * Discard until we see buttons=0, then arm normal press/release tracking. */
    if (d->popup_flushing) {
        if (buttons & 1u) { return; } /* stale press — discard */
        d->popup_flushing     = 0;
        d->popup_prev_buttons = 0;  /* clean slate for real interactions */
    }

    const int32_t  left_now  = (buttons & 1u) != 0;
    const int32_t  left_prev = (d->popup_prev_buttons & 1u) != 0;
    d->popup_prev_buttons    = buttons;

    const int32_t item_h  = 22;
    const int32_t hovered = (py >= 0 && py < d->popup_h) ? (py / item_h) : -1;

    if (hovered != d->popup_hovered) {
        d->popup_hovered = hovered;
        ui_menu_item_popup_render(ctx, mi, hovered);
        ui_menu_item_popup_present(ctx, d);
    }

    if (!left_now && left_prev) {
        if (hovered >= 0 && hovered < d->list.count) {
            d->list.selected = hovered;
            d->dropdown_open = 0;
            const int32_t mi_id    = mi->id;
            ui_button_click_cb_t cb = mi->on_click;
            void *user_data        = mi->on_click_user;
            ui_menu_item_popup_close(ctx, mi);
            if (cb) cb(ctx, mi_id, user_data);
            ui_mark_dirty(ctx);
        } else if (hovered < 0) {
            /* Release outside popup items — dismiss */
            d->dropdown_open = 0;
            ui_menu_item_popup_close(ctx, mi);
            ui_mark_dirty(ctx);
        }
    }
}

/* destroy_data for MENU_ITEM: frees owned heap, unmaps popup shmem (no IPC on destroy). */
static inline void
ui_menu_item_destroy_data(ui_component_t *c)
{
    ui_menu_item_data_t *d = (ui_menu_item_data_t *)c->component_data;
    if (!d) return;
    if (d->text.text) free(d->text.text);
    if (d->list.items) {
        for (int32_t i = 0; i < d->list.count; ++i)
            if (d->list.items[i]) free(d->list.items[i]);
        free(d->list.items);
    }
    if (d->popup_shmem_id > 0) wasmos_shmem_unmap(d->popup_shmem_id);
    free(d);
    c->component_data = NULL;
}

#endif /* WASMOS_LIBUI_MENU_ITEM_H */
