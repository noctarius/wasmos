#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/libui.h"
#include "wasmos/startup.h"

#define MENU_REFRESH_TICKS 200
#define MAX_APP_WINDOWS 32

static ui_context_t g_ctx;
static int32_t g_apps_mi_id = -1;
static int32_t g_win_ids[MAX_APP_WINDOWS];
static int32_t g_win_count = 0;

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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const int32_t proc_ep = (int32_t)(uint32_t)wasmos_startup_arg(0);
    const int32_t reply_ep = wasmos_ipc_create_endpoint();
    if (reply_ep < 0) return 1;

    if (ui_menu_bar_init(&g_ctx, proc_ep, reply_ep) != 0) return 1;

    /* "WasmOS" branding — left-anchored, no dropdown */
    const int32_t brand_id = ui_component_create_menu_item(&g_ctx);
    {
        ui_component_t *brand = ui_component_by_id(&g_ctx, brand_id);
        if (brand) {
            brand->bg_color  = 0xFF1A2233u;
            brand->fg_color  = 0xFF88C4EEu;
            brand->padding_px = 10;
            brand->preferred_h = 90;
        }
        ui_component_set_text(&g_ctx, brand_id, "WasmOS");
        ui_component_append_child(&g_ctx, g_ctx.root_id, brand_id);
    }

    /* "Apps" menu item */
    g_apps_mi_id = ui_component_create_menu_item(&g_ctx);
    {
        ui_component_t *apps_mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
        if (apps_mi) {
            apps_mi->bg_color   = 0xFF1A2233u;
            apps_mi->fg_color   = 0xFFDDE8F0u;
            apps_mi->padding_px = 10;
            apps_mi->preferred_h = 70;
            apps_mi->clickable  = 1;
            apps_mi->on_click   = on_apps_click;
        }
        ui_component_set_text(&g_ctx, g_apps_mi_id, "Apps");
        ui_component_append_child(&g_ctx, g_ctx.root_id, g_apps_mi_id);
    }

    refresh_app_list();

    wasmos_ipc_message_t msg;
    int32_t refresh_ctr = 0;

    while (!g_ctx.close_requested) {
        if (wasmos_ipc_call(g_ctx.gfx_endpoint, g_ctx.reply_endpoint,
                            GFX_IPC_POLL_EVENT, g_ctx.req_id++,
                            0, 0, 0, 0, &msg) == 0) {
            ui_loop_handle_ipc(&g_ctx, &msg);
        }

        ui_loop_drain(&g_ctx);

        if (++refresh_ctr >= MENU_REFRESH_TICKS) {
            refresh_ctr = 0;
            refresh_app_list();
        }

        wasmos_sched_yield();
    }

    ui_destroy(&g_ctx);
    return 0;
}
