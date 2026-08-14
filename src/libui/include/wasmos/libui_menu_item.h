#ifndef WASMOS_LIBUI_MENU_ITEM_H
#define WASMOS_LIBUI_MENU_ITEM_H

/* libui_menu_item.h - Menu item component: tree-based popup system.
 *
 * Every MENU_ITEM is a node.  Leaves carry an on_click callback.  Non-leaves
 * open their own popup window on hover (preview, no focus) and hand it focus
 * on click.  Depth is arbitrary — each non-leaf item manages one popup window
 * that lists its direct children.
 *
 * A popup is a real compositor window with its own shared buffer, not an
 * overlay in the bar window. Two consequences run through this file: popup
 * pointer events arrive already in popup-local coordinates and are routed by
 * window id (so ui_menu_item_popup_contains() must answer false for bar-window
 * coordinates), and a popup row's component bounds are republished in SCREEN
 * coordinates by ui_menu_item_popup_render() so a sub-popup can be positioned
 * against them.
 *
 * Rows are a fixed 22 px tall and popups are at least 160 px wide.
 */

static inline void ui_menu_item_popup_close(ui_context_t* ctx, ui_component_t* mi);
static inline void ui_menu_item_popup_open(ui_context_t* ctx, ui_component_t* mi);
static inline void ui_menu_item_popup_render(ui_context_t* ctx, ui_component_t* mi,
                                             int32_t hovered_row);
static inline void ui_menu_item_popup_present(ui_context_t* ctx, ui_menu_item_data_t* d);

/* Count direct children of a MENU_ITEM component. Zero means the item is a
 * leaf and has no popup; the count also sets the popup's height. */
static inline int32_t ui_menu_item_child_count(const ui_context_t* ctx, const ui_component_t* mi) {
    int32_t n = 0;
    int32_t cid = mi->first_child_id;
    while (cid > 0) {
        const ui_component_t* c = ui_component_by_id(ptr_cast(ui_context_t, ctx), cid);
        if (!c)
            break;
        ++n;
        cid = c->next_sibling_id;
    }
    return n;
}

/* Return the child component at zero-based row index, or NULL if out of range.
 * Row order is sibling order, which is the order the popup paints them in. */
static inline ui_component_t* ui_menu_item_child_at_row(ui_context_t* ctx, const ui_component_t* mi,
                                                        int32_t row) {
    int32_t n = 0;
    int32_t cid = mi->first_child_id;
    while (cid > 0) {
        ui_component_t* c = ui_component_by_id(ctx, cid);
        if (!c)
            break;
        if (n == row)
            return c;
        ++n;
        cid = c->next_sibling_id;
    }
    return NULL;
}

/* Render op for UI_COMPONENT_MENU_ITEM, drawing only the item's own label in
 * the window it lives in (the bar for a top-level item), with a highlight fill
 * while it is open or pressed. Its entries are children painted into the popup
 * window by ui_menu_item_popup_render(), which is why the core stops here and
 * does not descend. */
static inline void ui_render_menu_item(ui_context_t* ctx, const ui_component_t* c,
                                       ui_rect_t draw_bounds, ui_rect_t clip, int32_t offset_y) {
    (void)offset_y;
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)c->component_data;
    if (d && (d->dropdown_open || c->pressed)) {
        ui_fill_rect_clip(ctx->mapped_base, ctx->width, ctx->height, draw_bounds.x + 1,
                          draw_bounds.y + 1, draw_bounds.w - 2, draw_bounds.h - 2, 0xFF2A4060u,
                          clip);
    }
    ui_draw_text_clip(ctx, draw_bounds.x + (c->padding_px > 0 ? c->padding_px : 8),
                      draw_bounds.y + (draw_bounds.h - ctx->font_px) / 2,
                      (d && d->text.text) ? d->text.text : "",
                      c->fg_color ? c->fg_color : 0xFFDDE8F0u, clip);
    /* Only the bar/row label is drawn here; the popup has its own window. */
}

/* Compute the screen position/size for the popup window of mi.
 * Respects the parent type: below the bar item if parent is MENU_BAR,
 * or to the right of the row if parent is another MENU_ITEM.
 *
 * Width is max(mi->bounds.w, 160) and height is child_count * 22. All four
 * out-parameters are optional. Placement reads mi->bounds, which for a sub-menu
 * is the row rect published by ui_menu_item_popup_render() — so that render
 * must have run since the row moved, or the popup lands at a stale position.
 * The result is not clamped to the display, so a popup near an edge can be
 * positioned partly off-screen. */
static inline void ui_menu_item_popup_position(const ui_context_t* ctx, const ui_component_t* mi,
                                               int32_t child_count, int32_t* out_x, int32_t* out_y,
                                               int32_t* out_w, int32_t* out_h) {
    const int32_t item_h = 22;
    const int32_t min_w = 160;
    int32_t px, py, pw, ph;

    pw = mi->bounds.w > min_w ? mi->bounds.w : min_w;
    ph = child_count * item_h;

    /* Determine placement based on parent type */
    const ui_component_t* parent = ui_component_by_id(ptr_cast(ui_context_t, ctx), mi->parent_id);
    if (parent && parent->type == UI_COMPONENT_MENU_ITEM) {
        /* Sub-menu: open to the right of this item's row in its parent's popup.
         * mi->bounds is updated by popup_render to the row rect in screen coords. */
        px = mi->bounds.x + mi->bounds.w;
        py = mi->bounds.y;
    } else {
        /* Top-level bar item: open below it */
        px = mi->bounds.x;
        py = mi->bounds.y + mi->bounds.h;
    }

    if (out_x)
        *out_x = px;
    if (out_y)
        *out_y = py;
    if (out_w)
        *out_w = pw;
    if (out_h)
        *out_h = ph;
}

/* Always false, by design. The popup is a separate compositor window, so its
 * events arrive with coordinates relative to that window and are routed by
 * window id in ui_loop_handle_ipc. The core find_*_at walkers only ever pass
 * bar-window coordinates, and answering true here would map them onto popup
 * rows. The vtable slot stays filled so the core keeps asking the component. */
static inline bool ui_menu_item_popup_contains(const ui_context_t* ctx, const ui_component_t* c,
                                               int32_t x, int32_t y) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)c->component_data;
    if (!d || !d->dropdown_open || d->popup_win_id == 0)
        return false;
    (void)ctx;
    (void)x;
    (void)y;
    return false;
}

/* Repaint the popup's own framebuffer: one row per direct child, the hovered
 * row highlighted, and a '>' marker on rows that have children of their own.
 * Also republishes every child's bounds as its row rect in SCREEN coordinates,
 * which is what ui_menu_item_popup_position() reads to place a sub-popup — so
 * this must run before opening one. */
static inline void ui_menu_item_popup_render(ui_context_t* ctx, ui_component_t* mi,
                                             int32_t hovered_row) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)mi->component_data;
    if (!d || !d->popup_base)
        return;

    const int32_t item_h = 22;
    const ui_rect_t full = {0, 0, d->popup_w, d->popup_h};

    ui_fill_rect(d->popup_base, d->popup_w, d->popup_h, 0, 0, d->popup_w, d->popup_h, 0xFF1A2840u);
    ui_stroke_rect_clip(d->popup_base, d->popup_w, d->popup_h, full, 1, 0xFF4A6080u, full);

    /* ui_draw_text_clip() always draws into ctx->mapped_base at ctx->width x
     * ctx->height, so point the context at the popup framebuffer for the
     * duration and restore it before returning. */
    uint8_t* saved_base = ctx->mapped_base;
    int32_t saved_w = ctx->width;
    int32_t saved_h = ctx->height;
    ctx->mapped_base = d->popup_base;
    ctx->width = d->popup_w;
    ctx->height = d->popup_h;

    int32_t popup_screen_x = 0, popup_screen_y = 0;

    int32_t pw2 = 0, ph2 = 0;
    const int32_t nc = ui_menu_item_child_count(ctx, mi);
    ui_menu_item_popup_position(ctx, mi, nc, &popup_screen_x, &popup_screen_y, &pw2, &ph2);

    int32_t row = 0;
    int32_t cid = mi->first_child_id;
    while (cid > 0) {
        ui_component_t* child = ui_component_by_id(ctx, cid);
        if (!child)
            break;

        const int32_t row_y = row * item_h;

        child->bounds.x = popup_screen_x;
        child->bounds.y = popup_screen_y + row_y;
        child->bounds.w = d->popup_w;
        child->bounds.h = item_h;

        if (row == hovered_row) {
            ui_fill_rect(d->popup_base, d->popup_w, d->popup_h, 1, row_y, d->popup_w - 2, item_h,
                         0xFF2F5C88u);
        }

        const ui_menu_item_data_t* cd = (const ui_menu_item_data_t*)child->component_data;
        const char* label = (cd && cd->text.text) ? cd->text.text : "";
        ui_draw_text_clip(ctx, 8, row_y + (item_h - ctx->font_px) / 2, label, 0xFFFFFFFFu, full);

        if (child->first_child_id > 0) {
            ui_draw_text_clip(ctx, d->popup_w - 14, row_y + (item_h - ctx->font_px) / 2, ">",
                              0xFFAABBCCu, full);
        }

        ++row;
        cid = child->next_sibling_id;
    }

    ctx->mapped_base = saved_base;
    ctx->width = saved_w;
    ctx->height = saved_h;
}

/* Flush the popup's shared buffer and present it. Does nothing when the popup
 * is not fully set up. The compositor's reply status is discarded — a present
 * refused because the popup is being torn down is not an error worth acting on
 * here. Blocks for one compositor round trip. */
static inline void ui_menu_item_popup_present(ui_context_t* ctx, ui_menu_item_data_t* d) {
    if (!d || !d->popup_base || d->popup_win_id == 0 || d->popup_buf_id == 0)
        return;
    int32_t status = 0;
    wasmos_shmem_flush(d->popup_shmem_id, addr_cast(int32_t, d->popup_base),
                       d->popup_w * d->popup_h * 4);
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_PRESENT_WINDOW,
                d->popup_win_id, d->popup_buf_id, 0, 0, &status, 0, 0, 0);
}

/* Tear down mi's popup window: release the shared buffer, unmap the shmem,
 * destroy the window, reset the popup fields, and recursively close every
 * descendant's popup (clearing their dropdown_open too). A no-op when no popup
 * window exists. Blocks for the compositor round trips involved.
 *
 * mi's own dropdown_open is deliberately left alone, so a caller that closes
 * the window without clearing the flag will see ui_menu_item_sync_popup()
 * reopen it on the next drain — ui_menu_item_popup_open() relies on exactly
 * that to reopen at a new size. */
static inline void ui_menu_item_popup_close(ui_context_t* ctx, ui_component_t* mi) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)mi->component_data;
    if (!d || d->popup_win_id == 0)
        return;
    int32_t status = 0;
    if (d->popup_buf_id > 0) {
        ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, d->popup_buf_id, 0, 0, 0, &status, 0, 0, 0);
        d->popup_buf_id = 0;
    }
    if (d->popup_shmem_id > 0) {
        wasmos_shmem_unmap(d->popup_shmem_id);
        d->popup_shmem_id = 0;
    }
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_DESTROY_WINDOW,
                d->popup_win_id, 0, 0, 0, &status, 0, 0, 0);
    d->popup_win_id = 0;
    d->popup_base = NULL;
    d->popup_w = 0;
    d->popup_h = 0;
    d->popup_hovered = -1;
    d->popup_prev_buttons = 0;
    d->popup_has_focus = 0;

    /* Recursively close any open child sub-popups */
    int32_t cid = mi->first_child_id;
    while (cid > 0) {
        ui_component_t* child = ui_component_by_id(ctx, cid);
        if (!child)
            break;
        ui_menu_item_data_t* cd = (ui_menu_item_data_t*)child->component_data;
        if (cd) {
            cd->dropdown_open = 0;
            if (cd->popup_win_id > 0)
                ui_menu_item_popup_close(ctx, child);
        }
        cid = child->next_sibling_id;
    }
}

/* Create, paint and reveal mi's popup window. Requires dropdown_open to be set
 * already and does nothing for a leaf item. Any existing popup is closed first,
 * so this doubles as the reopen path after a size change.
 *
 * Whether the popup takes keyboard/pointer focus depends on the parent: a
 * top-level item (parent is the MENU_BAR) focuses its popup, while a sub-menu
 * opened by hover stays a focus-less preview until the row is clicked. The
 * window is created INVISIBLE and revealed only after the first present, so no
 * placeholder frame flashes. Every failure path destroys the partially built
 * window and leaves the popup fields untouched, so the item stays closed.
 * Blocks for several compositor round trips. */
static inline void ui_menu_item_popup_open(ui_context_t* ctx, ui_component_t* mi) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)mi->component_data;
    if (!d || !d->dropdown_open)
        return;

    const int32_t child_count = ui_menu_item_child_count(ctx, mi);
    if (child_count <= 0)
        return;

    const ui_component_t* parent = ui_component_by_id(ctx, mi->parent_id);
    const int32_t take_focus = (parent && parent->type == UI_COMPONENT_MENU_BAR);

    ui_menu_item_popup_close(ctx, mi);

    int32_t popup_x = 0, popup_y = 0, popup_w = 0, popup_h = 0;
    ui_menu_item_popup_position(ctx, mi, child_count, &popup_x, &popup_y, &popup_w, &popup_h);

    int32_t status = 0, a1 = 0, a2 = 0, a3 = 0;
    /* Source the popup window from the event endpoint so it becomes the popup's
     * owner: the compositor then pushes the popup's pointer/focus events to the
     * same endpoint the loop drains. (Sourcing from reply_endpoint would send
     * them to the request/reply channel, where the sync-call loop discards
     * them — breaking popup hover and focus-lost.) */
    if (ui_send_gfx(ctx->gfx_endpoint, ctx->event_endpoint, ctx->req_id++, GFX_IPC_CREATE_WINDOW,
                    popup_w, popup_h, (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status, &a1, &a2, &a3) != 0 ||
        status != WASMOS_ERR_NONE)
        return;
    const int32_t win_id = a1;

    /* INVISIBLE during setup prevents the placeholder flash before the first present. */
    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_SET_WINDOW_FLAGS,
                    win_id,
                    (int32_t)(GFX_WINDOW_FLAG_TOPMOST | GFX_WINDOW_FLAG_NO_CHROME |
                              GFX_WINDOW_FLAG_NO_TASK_LIST | GFX_WINDOW_FLAG_INVISIBLE),
                    0, 0, &status, 0, 0, 0) != 0 ||
        status != WASMOS_ERR_NONE)
        goto fail;

    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_MOVE_WINDOW,
                    win_id, popup_x, popup_y, 0, &status, 0, 0, 0) != 0 ||
        status != WASMOS_ERR_NONE)
        goto fail;

    int32_t buf_id = 0, shmem_id = 0, stride = 0;
    if (ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_ALLOC_SHARED_BUFFER, win_id, popup_w, popup_h, 0, &status, &buf_id,
                    &shmem_id, &stride) != 0 ||
        status != WASMOS_ERR_NONE)
        goto fail;

    const int32_t bytes = (popup_w * popup_h * 4 + (UI_PAGE_SIZE - 1)) & ~(UI_PAGE_SIZE - 1);
    const int32_t mapped = wasmos_shmem_map_auto(shmem_id, bytes);
    if (mapped < 0) {
        ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, buf_id, 0, 0, 0, &status, 0, 0, 0);
        goto fail;
    }

    d->popup_win_id = win_id;
    d->popup_buf_id = buf_id;
    d->popup_shmem_id = shmem_id;
    d->popup_base = ptr_cast(uint8_t, (uint32_t)mapped);
    d->popup_w = popup_w;
    d->popup_h = popup_h;
    d->popup_hovered = -1;
    d->popup_prev_buttons = 0;
    d->popup_flushing = 1; /* discard stale button-down events from before popup opened */
    d->popup_has_focus = take_focus ? 1 : 0;

    ui_menu_item_popup_render(ctx, mi, -1);
    ui_menu_item_popup_present(ctx, d);
    /* Reveal: clear INVISIBLE now that the buffer has content. */
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_SET_WINDOW_FLAGS,
                win_id,
                (int32_t)(GFX_WINDOW_FLAG_TOPMOST | GFX_WINDOW_FLAG_NO_CHROME |
                          GFX_WINDOW_FLAG_NO_TASK_LIST),
                0, 0, &status, 0, 0, 0);
    /* Top-level bar popups need focus so they receive pointer events.
     * Hover-opened child submenus must stay preview-only until clicked;
     * otherwise they trap pointer focus and the parent popup stops updating
     * hover state for sibling rows. */
    if (take_focus) {
        ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_FOCUS_WINDOW,
                    win_id, 0, 0, 0, &status, 0, 0, 0);
    }
    return;
fail:
    ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++, GFX_IPC_DESTROY_WINDOW,
                win_id, 0, 0, 0, &status, 0, 0, 0);
}

/* Flag-only open/close for a bar item. ui_loop_drain -> ui_menu_item_sync_popup
 * turns dropdown_open into the actual popup window.
 *
 * Open is refused for a leaf (an item with no children), so nothing is marked
 * dirty in that case. Close always sets the flag and marks dirty, whether or
 * not a popup exists. Neither issues any compositor IPC. */
static inline void ui_menu_item_open_dropdown(ui_context_t* ctx, ui_component_t* mi) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)mi->component_data;
    if (!d)
        return;
    if (mi->first_child_id > 0) {
        d->dropdown_open = 1;
        ui_mark_dirty(ctx);
    }
}

static inline void ui_menu_item_close_dropdown(ui_context_t* ctx, ui_component_t* mi) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)mi->component_data;
    if (d) {
        d->dropdown_open = 0;
        ui_mark_dirty(ctx);
    }
}

/* Reconcile the popup window with dropdown_open. Called for every MENU_ITEM
 * from ui_loop_drain, so a flag flipped anywhere takes effect on the next
 * repaint; a size change reopens the window.
 *
 * Closed with no window, or open with a window of the right height, are both
 * no-ops. The height comparison is how an item whose entries changed gets a
 * correctly sized popup; a change that leaves the child count the same is not
 * detected and the popup keeps its old contents until something else repaints
 * it. */
static inline void ui_menu_item_sync_popup(ui_context_t* ctx, ui_component_t* mi) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)mi->component_data;
    if (!d)
        return;
    if (!d->dropdown_open) {
        if (d->popup_win_id > 0)
            ui_menu_item_popup_close(ctx, mi);
        return;
    }
    const int32_t child_count = ui_menu_item_child_count(ctx, mi);
    const int32_t expected_h = child_count * 22;
    if (d->popup_win_id == 0) {
        ui_menu_item_popup_open(ctx, mi);
    } else if (d->popup_h != expected_h) {
        ui_menu_item_popup_close(ctx, mi);
        ui_menu_item_popup_open(ctx, mi);
    }
}

/* Handle one GFX_EVENT_POINTER from mi's popup window. msg->arg3 carries
 * popup-window coordinates, so the row is py / item_h with no translation.
 * Hover moves open/close sub-popups as a preview; a left release activates a
 * leaf's on_click or hands the sub-popup focus.
 *
 * The caller must have routed the event to the component that owns the window
 * it came from; this function does not check msg's window id. Button-down
 * events queued before the popup existed are discarded until the first release
 * (popup_flushing), so the click that opened a menu cannot immediately pick a
 * row under the cursor. Activation happens on release, not press. A release
 * outside the popup does nothing — dismissal comes from FOCUS_LOST. */
static inline void ui_menu_item_handle_popup_event(ui_context_t* ctx, ui_component_t* mi,
                                                   const wasmos_ipc_message_t* msg) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)mi->component_data;
    if (!d || d->popup_win_id == 0)
        return;

    const int32_t px = ui_ptr_evt_x(msg->arg3);
    const int32_t py = ui_ptr_evt_y(msg->arg3);
    const uint32_t buttons = ui_ptr_evt_buttons(msg->arg3);
    (void)px;

    /* Flush stale button-down events queued before popup opened. */
    if (d->popup_flushing) {
        if (buttons & 1u) {
            return;
        }
        d->popup_flushing = 0;
        d->popup_prev_buttons = 0;
    }

    const int32_t left_now = (buttons & 1u) != 0;
    const int32_t left_prev = (d->popup_prev_buttons & 1u) != 0;
    d->popup_prev_buttons = buttons;

    const int32_t item_h = 22;
    const int32_t child_count = ui_menu_item_child_count(ctx, mi);
    const int32_t hovered = (py >= 0 && py < d->popup_h) ? (py / item_h) : -1;

    if (hovered != d->popup_hovered) {
        const int32_t old_row = d->popup_hovered;
        d->popup_hovered = hovered;

        /* Close sub-popup of previously-hovered child (if it has one). */
        if (old_row >= 0) {
            ui_component_t* old_child = ui_menu_item_child_at_row(ctx, mi, old_row);
            if (old_child && old_child->first_child_id > 0) {
                ui_menu_item_data_t* ocd = (ui_menu_item_data_t*)old_child->component_data;
                if (ocd) {
                    ocd->dropdown_open = 0;
                    if (ocd->popup_win_id > 0)
                        ui_menu_item_popup_close(ctx, old_child);
                }
            }
        }

        /* Render first: this updates each child->bounds to its screen-space row rect,
         * which popup_position uses to place the sub-popup to the right. */
        ui_menu_item_popup_render(ctx, mi, hovered);
        ui_menu_item_popup_present(ctx, d);

        /* Open sub-popup for newly-hovered non-leaf child (bounds now correct). */
        if (hovered >= 0) {
            ui_component_t* new_child = ui_menu_item_child_at_row(ctx, mi, hovered);
            if (new_child && new_child->first_child_id > 0) {
                ui_menu_item_data_t* ncd = (ui_menu_item_data_t*)new_child->component_data;
                if (ncd && ncd->popup_win_id == 0) {
                    ncd->dropdown_open = 1;
                    ui_menu_item_popup_open(ctx, new_child);
                }
            }
        }
    }

    if (!left_now && left_prev) {
        if (hovered >= 0 && hovered < child_count) {
            ui_component_t* child = ui_menu_item_child_at_row(ctx, mi, hovered);
            if (child) {
                if (child->first_child_id == 0) {
                    /* Leaf: dismiss the popup first so the callback runs with
                     * the menu already closed, from values cached beforehand. */
                    d->dropdown_open = 0;
                    const int32_t mi_id = mi->id;
                    (void)mi_id;
                    ui_button_click_cb_t cb = child->on_click;
                    void* user_data = child->on_click_user;
                    const int32_t child_id = child->id;
                    ui_menu_item_popup_close(ctx, mi);
                    if (cb)
                        cb(ctx, child_id, user_data);
                    ui_mark_dirty(ctx);
                } else {
                    /* Non-leaf: give sub-popup focus if preview is showing */
                    ui_menu_item_data_t* cd = (ui_menu_item_data_t*)child->component_data;
                    if (cd) {
                        if (cd->popup_win_id == 0) {
                            /* Preview not yet open — open it now */
                            cd->dropdown_open = 1;
                            ui_menu_item_popup_open(ctx, child);
                        }
                        if (cd->popup_win_id > 0) {
                            int32_t status = 0;
                            ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                                        GFX_IPC_FOCUS_WINDOW, cd->popup_win_id, 0, 0, 0, &status, 0,
                                        0, 0);
                        }
                    }
                }
            }
        }
        /* A release outside the popup deliberately does nothing: the popup is
         * dismissed by FOCUS_LOST once the click lands outside the whole menu
         * hierarchy. Closing here would also fire when the pointer merely
         * drifted a few pixels past the popup edge. */
    }
}

/* Pointer release inside the bar window: a leaf bar item fires its on_click,
 * a non-leaf toggles its own dropdown after closing its siblings. Popup rows
 * are handled by ui_menu_item_handle_popup_event instead.
 *
 * (x, y) are bar-window coordinates. Only items whose parent is a MENU_BAR are
 * considered, and the first one containing the point wins; a release that hits
 * none does nothing. Toggling only sets flags — the window itself follows on
 * the next ui_loop_drain(). */
static inline void ui_menu_item_handle_pointer_release(ui_context_t* ctx, int32_t x, int32_t y) {
    int32_t mi_id2 = -1;
    for (int32_t ci2 = 0; ci2 < ctx->component_count; ++ci2) {
        ui_component_t* mc = &ctx->components[ci2];
        if (!mc->in_use || mc->type != UI_COMPONENT_MENU_ITEM)
            continue;
        /* Only top-level bar items (parent is MENU_BAR or root) */
        const ui_component_t* par = ui_component_by_id(ctx, mc->parent_id);
        if (!par || par->type != UI_COMPONENT_MENU_BAR)
            continue;
        if (ui_point_in_bounds(x, y, mc->bounds)) {
            mi_id2 = mc->id;
            break;
        }
    }
    if (mi_id2 > 0) {
        ui_component_t* mi2 = ui_component_by_id(ctx, mi_id2);
        ui_menu_item_data_t* d2 = mi2 ? (ui_menu_item_data_t*)mi2->component_data : NULL;
        if (mi2 && d2 && ui_point_in_bounds(x, y, mi2->bounds)) {
            /* Check if this bar item is a pure leaf (no children) */
            if (mi2->first_child_id == 0) {
                /* Leaf bar item: fire its own on_click directly */
                if (mi2->on_click)
                    mi2->on_click(ctx, mi2->id, mi2->on_click_user);
                ui_mark_dirty(ctx);
            } else {
                /* Non-leaf: toggle dropdown open/close, close siblings first */
                const int32_t will_open = !d2->dropdown_open;
                for (int32_t ci3 = 0; ci3 < ctx->component_count; ++ci3) {
                    ui_component_t* sib = &ctx->components[ci3];
                    if (!sib->in_use || sib->type != UI_COMPONENT_MENU_ITEM)
                        continue;
                    const ui_component_t* sp = ui_component_by_id(ctx, sib->parent_id);
                    if (!sp || sp->type != UI_COMPONENT_MENU_BAR)
                        continue;
                    ui_menu_item_close_dropdown(ctx, sib);
                }
                if (will_open)
                    ui_menu_item_open_dropdown(ctx, mi2);
                ui_mark_dirty(ctx);
            }
        }
    }
}

/* Close every menu popup in the context. The mi argument is the component that
 * observed the focus loss and is not otherwise consulted: dismissal is
 * all-or-nothing across the whole menu hierarchy.
 *
 * Clears dropdown_open on every MENU_ITEM in the pool and destroys any popup
 * window each still owns, then marks the context dirty. Blocks for the
 * compositor round trips of every window it closes. */
static inline void ui_menu_item_dismiss_popup(ui_context_t* ctx, ui_component_t* mi) {
    for (int32_t i = 0; i < ctx->component_count; ++i) {
        ui_component_t* c = &ctx->components[i];
        if (!c->in_use || c->type != UI_COMPONENT_MENU_ITEM)
            continue;
        ui_menu_item_data_t* cd = (ui_menu_item_data_t*)c->component_data;
        if (!cd)
            continue;
        cd->dropdown_open = 0;
        if (cd->popup_win_id > 0)
            ui_menu_item_popup_close(ctx, c);
    }
    (void)mi;
    ui_mark_dirty(ctx);
}

/* destroy_data op for UI_COMPONENT_MENU_ITEM: frees the label, unmaps the popup
 * shmem if one is still mapped, frees the data struct and clears
 * component_data. Tolerates already-cleared data; the component pointer itself
 * must be non-NULL. */
static inline void ui_menu_item_destroy_data(ui_component_t* c) {
    ui_menu_item_data_t* d = (ui_menu_item_data_t*)c->component_data;
    if (!d)
        return;
    if (d->text.text)
        free(d->text.text);
    /* Only the shmem mapping is released here; the compositor window and its
     * shared buffer are torn down by ui_menu_item_popup_close().
     * FIXME: a popup still open when ui_destroy() walks the components leaks
     * its window and buffer, because destroy_data has no gfx endpoint to send
     * DESTROY_WINDOW / RELEASE_SHARED_BUFFER on. */
    if (d->popup_shmem_id > 0)
        wasmos_shmem_unmap(d->popup_shmem_id);
    free(d);
    c->component_data = NULL;
}

#endif /* WASMOS_LIBUI_MENU_ITEM_H */
