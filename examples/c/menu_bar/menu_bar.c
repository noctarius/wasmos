#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/libui.h"
#include "wasmos/startup.h"

#define MENU_REFRESH_TICKS 200
#define CLOCK_REFRESH_TICKS 50
#define MAX_APP_WINDOWS 32
#define POPUP_ITEM_H 22
#define POPUP_MIN_W  160

static ui_context_t g_ctx;
static int32_t g_apps_mi_id = -1;
static int32_t g_win_ids[MAX_APP_WINDOWS];
static int32_t g_win_count = 0;
static int32_t g_rtc_endpoint = -1;

/* ---- popup window state ---- */
static int32_t  g_popup_mi_id  = -1; /* component id of the owning menu item, -1 = closed */
static int32_t  g_popup_win_id = 0;
static int32_t  g_popup_buf_id = 0;
static int32_t  g_popup_shmem_id = 0;
static uint8_t *g_popup_base   = NULL;
static int32_t  g_popup_w      = 0;
static int32_t  g_popup_h      = 0;
static int32_t  g_popup_hovered = -1;
static uint32_t g_popup_prev_buttons = 0;

/* ---- popup helpers ---- */

static void popup_present(void)
{
    if (!g_popup_base || g_popup_win_id == 0 || g_popup_buf_id == 0) return;
    int32_t status = 0;
    wasmos_shmem_flush(g_popup_shmem_id, (int32_t)(uintptr_t)g_popup_base,
                       g_popup_w * g_popup_h * 4);
    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                GFX_IPC_PRESENT_WINDOW, g_popup_win_id, g_popup_buf_id, 0, 0,
                &status, 0, 0, 0);
}

static void popup_render(int32_t hovered)
{
    if (!g_popup_base || g_popup_mi_id < 0) return;
    ui_component_t *mi = ui_component_by_id(&g_ctx, g_popup_mi_id);
    ui_menu_item_data_t *md = mi ? (ui_menu_item_data_t *)mi->component_data : NULL;
    if (!md) return;

    /* Background + border */
    ui_fill_rect(g_popup_base, g_popup_w, g_popup_h, 0, 0, g_popup_w, g_popup_h, 0xFF1A2840u);
    const ui_rect_t full = {0, 0, g_popup_w, g_popup_h};
    ui_stroke_rect_clip(g_popup_base, g_popup_w, g_popup_h, full, 1, 0xFF4A6080u, full);

    /* Temporarily redirect context buffer to popup framebuffer for text rendering. */
    uint8_t *saved_base = g_ctx.mapped_base;
    int32_t  saved_w    = g_ctx.width;
    int32_t  saved_h    = g_ctx.height;
    g_ctx.mapped_base = g_popup_base;
    g_ctx.width       = g_popup_w;
    g_ctx.height      = g_popup_h;

    for (int32_t i = 0; i < md->list.count; ++i) {
        if (i == hovered) {
            ui_fill_rect(g_popup_base, g_popup_w, g_popup_h,
                         1, i * POPUP_ITEM_H, g_popup_w - 2, POPUP_ITEM_H, 0xFF2F5C88u);
        }
        ui_draw_text_clip(&g_ctx, 8,
                          i * POPUP_ITEM_H + (POPUP_ITEM_H - g_ctx.font_px) / 2,
                          (md->list.items[i] ? md->list.items[i] : ""),
                          0xFFFFFFFFu, full);
    }

    g_ctx.mapped_base = saved_base;
    g_ctx.width       = saved_w;
    g_ctx.height      = saved_h;
}

static void popup_close(void)
{
    if (g_popup_win_id == 0) return;
    int32_t status = 0;
    if (g_popup_buf_id > 0) {
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, g_popup_buf_id, 0, 0, 0,
                    &status, 0, 0, 0);
        g_popup_buf_id = 0;
    }
    if (g_popup_shmem_id > 0) {
        wasmos_shmem_unmap(g_popup_shmem_id);
        g_popup_shmem_id = 0;
    }
    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                GFX_IPC_DESTROY_WINDOW, g_popup_win_id, 0, 0, 0,
                &status, 0, 0, 0);
    g_popup_win_id    = 0;
    g_popup_mi_id     = -1;
    g_popup_base      = NULL;
    g_popup_w         = 0;
    g_popup_h         = 0;
    g_popup_hovered   = -1;
    g_popup_prev_buttons = 0;
}

static void popup_open(int32_t mi_id)
{
    popup_close();

    ui_component_t *mi = ui_component_by_id(&g_ctx, mi_id);
    ui_menu_item_data_t *md = mi ? (ui_menu_item_data_t *)mi->component_data : NULL;
    if (!md || !md->dropdown_open || md->list.count <= 0) return;

    const int32_t popup_x = mi->bounds.x;
    const int32_t popup_y = g_ctx.height; /* bar bottom = BAR_H */
    const int32_t popup_w = mi->bounds.w > POPUP_MIN_W ? mi->bounds.w : POPUP_MIN_W;
    const int32_t popup_h = md->list.count * POPUP_ITEM_H;
    int32_t status = 0, a1 = 0, a2 = 0, a3 = 0;

    /* Create popup window */
    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_CREATE_WINDOW, popup_w, popup_h,
                    (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status, &a1, &a2, &a3) != 0 || status != GFX_STATUS_OK) return;
    const int32_t win_id = a1;

    /* SYSTEM flag for topmost z-order */
    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_SET_WINDOW_FLAGS, win_id, (int32_t)GFX_WINDOW_FLAG_SYSTEM, 0, 0,
                    &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) {
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_DESTROY_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
        return;
    }

    /* Position below the owning menu item */
    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_MOVE_WINDOW, win_id, popup_x, popup_y, 0,
                    &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) {
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_DESTROY_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
        return;
    }

    /* Allocate backing buffer */
    int32_t buf_id = 0, shmem_id = 0, stride = 0;
    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_ALLOC_SHARED_BUFFER, win_id, popup_w, popup_h, 0,
                    &status, &buf_id, &shmem_id, &stride) != 0 || status != GFX_STATUS_OK) {
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_DESTROY_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
        return;
    }

    const int32_t bytes = (popup_w * popup_h * 4 + (UI_PAGE_SIZE - 1)) & ~(UI_PAGE_SIZE - 1);
    const int32_t mapped = wasmos_shmem_map_auto(shmem_id, bytes);
    if (mapped < 0) {
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, buf_id, 0, 0, 0, &status, 0, 0, 0);
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_DESTROY_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
        return;
    }

    g_popup_win_id   = win_id;
    g_popup_buf_id   = buf_id;
    g_popup_shmem_id = shmem_id;
    g_popup_base     = (uint8_t *)(uintptr_t)(uint32_t)mapped;
    g_popup_w        = popup_w;
    g_popup_h        = popup_h;
    g_popup_mi_id    = mi_id;
    g_popup_hovered  = -1;
    g_popup_prev_buttons = 0;

    popup_render(-1);
    popup_present();

    /* Focus popup so it receives pointer events */
    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                GFX_IPC_FOCUS_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
}

/* Close popup and clear all dropdown state; call when clicking outside. */
static void popup_dismiss(void)
{
    for (int32_t i = 0; i < g_ctx.component_count; ++i) {
        ui_component_t *c = &g_ctx.components[i];
        if (!c->in_use || c->type != UI_COMPONENT_MENU_ITEM) continue;
        ui_menu_item_data_t *d = (ui_menu_item_data_t *)c->component_data;
        if (d) d->dropdown_open = 0;
    }
    popup_close();
    ui_mark_dirty(&g_ctx);
}

/* Sync popup window existence with current dropdown_open state. */
static void sync_popup(void)
{
    int32_t should_be = -1;
    for (int32_t i = 0; i < g_ctx.component_count; ++i) {
        const ui_component_t *c = &g_ctx.components[i];
        if (!c->in_use || c->type != UI_COMPONENT_MENU_ITEM) continue;
        const ui_menu_item_data_t *d = (const ui_menu_item_data_t *)c->component_data;
        if (d && d->dropdown_open) { should_be = c->id; break; }
    }

    if (should_be == g_popup_mi_id) {
        /* Same menu item open — re-open if content height changed (e.g. app list refresh) */
        if (g_popup_mi_id >= 0 && g_popup_win_id > 0) {
            const ui_component_t *mi = ui_component_by_id(&g_ctx, g_popup_mi_id);
            const ui_menu_item_data_t *md = mi ? (const ui_menu_item_data_t *)mi->component_data : NULL;
            if (md && md->list.count * POPUP_ITEM_H != g_popup_h) {
                popup_close();
                popup_open(g_popup_mi_id);
            }
        }
        return;
    }

    if (g_popup_win_id > 0) popup_close();
    if (should_be >= 0) popup_open(should_be);
    g_popup_mi_id = should_be;
}

/* Handle a GFX_EVENT_POINTER that came from the popup window. */
static void popup_handle_pointer(const wasmos_ipc_message_t *msg)
{
    if (!g_popup_base || g_popup_mi_id < 0) return;
    const int32_t px       = ui_u16_lo(msg->arg2);
    const int32_t py       = ui_u16_hi(msg->arg2);
    const uint32_t buttons = (uint32_t)msg->arg3;
    const int32_t left_now  = (buttons & 1u) != 0;
    const int32_t left_prev = (g_popup_prev_buttons & 1u) != 0;
    g_popup_prev_buttons = buttons;

    const int32_t hovered = (py >= 0 && py < g_popup_h) ? (py / POPUP_ITEM_H) : -1;

    if (left_now && !left_prev) {
        /* Click: select and invoke */
        if (hovered >= 0) {
            ui_component_t *mi = ui_component_by_id(&g_ctx, g_popup_mi_id);
            ui_menu_item_data_t *md = mi ? (ui_menu_item_data_t *)mi->component_data : NULL;
            if (md && hovered < md->list.count) {
                md->list.selected = hovered;
                md->dropdown_open = 0;
                const int32_t cb_id = g_popup_mi_id;
                popup_close();
                if (mi->on_click) mi->on_click(&g_ctx, cb_id, mi->on_click_user);
                ui_mark_dirty(&g_ctx);
            }
        }
    } else if (!left_now && hovered != g_popup_hovered) {
        /* Hover: re-render with new highlight */
        g_popup_hovered = hovered;
        popup_render(hovered);
        popup_present();
    }
}

/* ---- clock ---- */

static void pad2(char *out, int32_t v)
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

static void format_clock(int32_t year, int32_t month, int32_t day,
                          int32_t hour, int32_t min, int32_t sec, char *buf)
{
    buf[0] = (char)('0' + (year / 1000) % 10);
    buf[1] = (char)('0' + (year / 100) % 10);
    buf[2] = (char)('0' + (year / 10) % 10);
    buf[3] = (char)('0' + year % 10);
    buf[4] = '-'; pad2(buf + 5, month);
    buf[7] = '-'; pad2(buf + 8, day);
    buf[10] = ' '; pad2(buf + 11, hour);
    buf[13] = ':'; pad2(buf + 14, min);
    buf[16] = ':'; pad2(buf + 17, sec);
    buf[19] = '\0';
}

static void update_clock(void)
{
    if (g_rtc_endpoint < 0) return;
    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(g_rtc_endpoint, g_ctx.reply_endpoint,
                        RTC_IPC_READ_REQ, g_ctx.req_id++,
                        0, 0, 0, 0, &reply) != 0) return;
    if (reply.type != RTC_IPC_READ_RESP) return;
    const int32_t a0 = reply.arg0, a1 = reply.arg1;
    char buf[24];
    format_clock((a1 >> 8) & 0xFFFF, a1 & 0xFF, (a0 >> 24) & 0xFF,
                 (a0 >> 16) & 0xFF, (a0 >> 8) & 0xFF, a0 & 0xFF, buf);
    ui_menu_bar_set_clock(&g_ctx, g_ctx.root_id, buf);
}

/* ---- app list ---- */

static void int_to_str(int32_t v, char *buf, int32_t cap)
{
    if (cap <= 1) return;
    if (v <= 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16];
    int32_t n = 0;
    while (v > 0 && n < 15) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int32_t out = 0;
    for (int32_t i = n - 1; i >= 0 && out < cap - 1; --i) buf[out++] = tmp[i];
    buf[out] = '\0';
}

static void refresh_app_list(void)
{
    ui_component_t *mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
    if (!mi) return;

    ui_menu_item_data_t *md = (ui_menu_item_data_t *)mi->component_data;
    if (md) {
        for (int32_t i = 0; i < md->list.count; ++i) {
            if (md->list.items && md->list.items[i]) { free(md->list.items[i]); md->list.items[i] = 0; }
        }
        md->list.count = 0;
    }
    g_win_count = 0;

    for (int32_t idx = 0; idx < MAX_APP_WINDOWS; ++idx) {
        int32_t status = 0, wid = 0;
        if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                        GFX_IPC_LIST_WINDOWS, idx, 0, 0, 0,
                        &status, &wid, 0, 0) != 0) break;
        if (status != GFX_STATUS_OK || wid == 0) break;
        if (wid == g_ctx.window_id) continue;

        char label[32];
        const char *prefix = "win ";
        int32_t n = 0;
        while (prefix[n] && n < 20) { label[n] = prefix[n]; n++; }
        char num[8];
        int_to_str(wid, num, 8);
        for (int32_t j = 0; num[j] && n < 31; ++j) label[n++] = num[j];
        label[n] = '\0';

        ui_component_list_append(&g_ctx, g_apps_mi_id, label);
        if (g_win_count < MAX_APP_WINDOWS) g_win_ids[g_win_count++] = wid;
    }
    ui_mark_dirty(&g_ctx);
}

/* ---- callbacks ---- */

static void on_wasmos_click(ui_context_t *ctx, int32_t component_id, void *user)
{
    (void)user;
    ui_component_t *mi = ui_component_by_id(ctx, component_id);
    if (!mi) return;
    ui_menu_item_data_t *md = (ui_menu_item_data_t *)mi->component_data;
    const int32_t sel = md ? md->list.selected : -1;
    if (sel == 0) {
        (void)wasmos_system_reboot();
    } else if (sel == 1) {
        (void)wasmos_system_halt();
    }
}

static void on_apps_click(ui_context_t *ctx, int32_t component_id, void *user)
{
    (void)user;
    ui_component_t *mi = ui_component_by_id(ctx, component_id);
    if (!mi) return;
    ui_menu_item_data_t *md = (ui_menu_item_data_t *)mi->component_data;
    const int32_t sel = md ? md->list.selected : -1;
    if (sel < 0 || sel >= g_win_count) return;
    const int32_t wid = g_win_ids[sel];
    int32_t status = 0;
    (void)ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                      GFX_IPC_FOCUS_WINDOW, wid, 0, 0, 0,
                      &status, 0, 0, 0);
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const int32_t proc_ep = (int32_t)(uint32_t)wasmos_startup_arg(0);
    const int32_t reply_ep = wasmos_ipc_create_endpoint();
    if (reply_ep < 0) return 1;

    if (ui_menu_bar_init(&g_ctx, proc_ep, reply_ep) != 0) return 1;

    for (int32_t spins = 0; spins < 512; ++spins) {
        g_rtc_endpoint = wasmos_svc_lookup(proc_ep, reply_ep, "rtc", g_ctx.req_id++);
        if (g_rtc_endpoint >= 0) break;
        (void)wasmos_sched_yield();
    }

    /* "WasmOS" system menu — Reboot / Shutdown */
    const int32_t brand_id = ui_component_create_menu_item(&g_ctx);
    {
        ui_component_t *brand = ui_component_by_id(&g_ctx, brand_id);
        if (brand) {
            brand->bg_color    = 0xFF1A2233u;
            brand->fg_color    = 0xFF88C4EEu;
            brand->padding_px  = 10;
            brand->preferred_h = 90;
            brand->clickable   = 1;
            brand->on_click    = on_wasmos_click;
        }
        ui_component_set_text(&g_ctx, brand_id, "WasmOS");
        ui_component_list_append(&g_ctx, brand_id, "Reboot");
        ui_component_list_append(&g_ctx, brand_id, "Shutdown");
        ui_component_append_child(&g_ctx, g_ctx.root_id, brand_id);
    }

    /* "Apps" menu item */
    g_apps_mi_id = ui_component_create_menu_item(&g_ctx);
    {
        ui_component_t *apps_mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
        if (apps_mi) {
            apps_mi->bg_color    = 0xFF1A2233u;
            apps_mi->fg_color    = 0xFFDDE8F0u;
            apps_mi->padding_px  = 10;
            apps_mi->preferred_h = 70;
            apps_mi->clickable   = 1;
            apps_mi->on_click    = on_apps_click;
        }
        ui_component_set_text(&g_ctx, g_apps_mi_id, "Apps");
        ui_component_append_child(&g_ctx, g_ctx.root_id, g_apps_mi_id);
    }

    refresh_app_list();
    update_clock();

    wasmos_ipc_message_t msg;
    int32_t refresh_ctr = 0;
    int32_t clock_ctr   = 0;

    while (!g_ctx.close_requested) {
        if (wasmos_ipc_call(g_ctx.gfx_endpoint, g_ctx.reply_endpoint,
                            GFX_IPC_POLL_EVENT, g_ctx.req_id++,
                            0, 0, 0, 0, &msg) == 0 &&
            msg.type == GFX_IPC_RESP && msg.arg0 == GFX_STATUS_OK) {

            if (g_popup_win_id > 0) {
                /* Route events to popup window or bar */
                if (msg.arg1 == GFX_EVENT_POINTER) {
                    /* Pointer goes to popup (it has focus) */
                    popup_handle_pointer(&msg);
                } else if ((msg.arg1 == GFX_EVENT_FOCUS_LOST ||
                             msg.arg1 == GFX_EVENT_CLOSE_REQUEST) &&
                            (int32_t)msg.arg2 == g_popup_win_id) {
                    /* Popup lost focus — user clicked elsewhere */
                    popup_dismiss();
                } else {
                    ui_loop_handle_ipc(&g_ctx, &msg);
                }
            } else {
                ui_loop_handle_ipc(&g_ctx, &msg);
            }
        }

        sync_popup();
        ui_loop_drain(&g_ctx);

        if (++clock_ctr >= CLOCK_REFRESH_TICKS) {
            clock_ctr = 0;
            update_clock();
        }

        if (++refresh_ctr >= MENU_REFRESH_TICKS) {
            refresh_ctr = 0;
            refresh_app_list();
        }

        wasmos_sched_yield();
    }

    popup_close();
    ui_destroy(&g_ctx);
    return 0;
}
