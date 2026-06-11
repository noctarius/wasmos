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
#define MAX_APP_ITEMS 12
#define MAX_WINS_PER_APP 8
#define TITLE_MAX 48

static ui_context_t g_ctx;
static int32_t g_rtc_endpoint = -1;

/* Per-app group data */
static int32_t g_app_mi_ids[MAX_APP_ITEMS];   /* component IDs of per-app menu items */
static int32_t g_app_owner_eps[MAX_APP_ITEMS];
static int32_t g_app_win_ids[MAX_APP_ITEMS][MAX_WINS_PER_APP];
static int32_t g_app_win_counts[MAX_APP_ITEMS];
static int32_t g_app_count = 0;

/* Reusable shmem for fetching window titles */
static int32_t g_title_shmem_id = -1;
static uint8_t *g_title_ptr = NULL;

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

/* ---- helpers ---- */

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

static void on_app_item_click(ui_context_t *ctx, int32_t component_id, void *user)
{
    (void)user;
    /* Find which app slot this is */
    int32_t app_idx = -1;
    for (int32_t i = 0; i < MAX_APP_ITEMS; ++i) {
        if (g_app_mi_ids[i] == component_id) { app_idx = i; break; }
    }
    if (app_idx < 0 || app_idx >= g_app_count) return;

    int32_t win_idx = 0;
    if (g_app_win_counts[app_idx] > 1) {
        ui_component_t *mi = ui_component_by_id(ctx, component_id);
        ui_menu_item_data_t *md = mi ? (ui_menu_item_data_t *)mi->component_data : NULL;
        if (md && md->list.selected >= 0 && md->list.selected < g_app_win_counts[app_idx])
            win_idx = md->list.selected;
    }
    int32_t wid = g_app_win_ids[app_idx][win_idx];
    int32_t status = 0;
    (void)ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                      GFX_IPC_FOCUS_WINDOW, wid, 0, 0, 0,
                      &status, 0, 0, 0);
}

/* ---- app list ---- */

static void refresh_app_list(void)
{
    /* Collect all non-menu-bar windows, group by owner_endpoint */
    typedef struct { int32_t ep; int32_t wids[MAX_WINS_PER_APP]; int32_t count; } AppGroup;
    AppGroup groups[MAX_APP_ITEMS];
    int32_t ngroups = 0;

    for (int32_t idx = 0; idx < 32 /* GFX_MAX_WINDOWS */; ++idx) {
        int32_t status = 0, wid = 0, owner_ep = 0;
        if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                        GFX_IPC_LIST_WINDOWS, idx, 0, 0, 0,
                        &status, &wid, &owner_ep, 0) != 0) break;
        if (status != GFX_STATUS_OK || wid == 0) break;
        /* Find existing group or create new one */
        int32_t gi = -1;
        for (int32_t g = 0; g < ngroups; ++g)
            if (groups[g].ep == owner_ep) { gi = g; break; }
        if (gi < 0) {
            if (ngroups >= MAX_APP_ITEMS) continue;
            gi = ngroups++;
            groups[gi].ep = owner_ep;
            groups[gi].count = 0;
        }
        if (groups[gi].count < MAX_WINS_PER_APP)
            groups[gi].wids[groups[gi].count++] = wid;
    }

    /* Update per-app menu items */
    g_app_count = ngroups;
    for (int32_t i = 0; i < MAX_APP_ITEMS; ++i) {
        ui_component_t *mi = ui_component_by_id(&g_ctx, g_app_mi_ids[i]);
        if (!mi) continue;
        ui_menu_item_data_t *md = (ui_menu_item_data_t *)mi->component_data;
        if (!md) continue;

        if (i >= ngroups) {
            /* Deactivate: hide */
            mi->preferred_h = 0;
            continue;
        }

        AppGroup *ag = &groups[i];
        g_app_owner_eps[i] = ag->ep;
        g_app_win_counts[i] = ag->count;
        for (int32_t w = 0; w < ag->count; ++w)
            g_app_win_ids[i][w] = ag->wids[w];

        /* Clear old list items */
        if (md->list.items) {
            for (int32_t j = 0; j < md->list.count; ++j) {
                if (md->list.items[j]) { free(md->list.items[j]); md->list.items[j] = 0; }
            }
        }
        md->list.count = 0;
        md->list.selected = -1;

        /* Fetch title for first window -> app name */
        char app_name[TITLE_MAX] = {0};
        if (g_title_ptr && g_title_shmem_id > 0) {
            int32_t status = 0, tlen = 0;
            ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                        GFX_IPC_GET_WINDOW_TITLE, ag->wids[0], g_title_shmem_id, TITLE_MAX - 1, 0,
                        &status, &tlen, 0, 0);
            if (status == GFX_STATUS_OK && tlen > 0) {
                wasmos_shmem_refresh(g_title_shmem_id, (int32_t)(uintptr_t)g_title_ptr, tlen + 1);
                for (int32_t k = 0; k < tlen && k < TITLE_MAX - 1; ++k)
                    app_name[k] = (char)g_title_ptr[k];
                app_name[tlen] = '\0';
            }
        }
        if (app_name[0] == '\0') {
            /* Fallback: "App N" */
            app_name[0] = 'A'; app_name[1] = 'p'; app_name[2] = 'p'; app_name[3] = ' ';
            int_to_str(ag->wids[0], app_name + 4, TITLE_MAX - 4);
        }
        ui_component_set_text(&g_ctx, g_app_mi_ids[i], app_name);
        mi->preferred_h = 80; /* visible with default width */

        /* For multi-window apps, populate window title list */
        if (ag->count > 1) {
            for (int32_t w = 0; w < ag->count; ++w) {
                char win_title[TITLE_MAX] = {0};
                if (g_title_ptr && g_title_shmem_id > 0) {
                    int32_t status = 0, tlen = 0;
                    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                                GFX_IPC_GET_WINDOW_TITLE, ag->wids[w], g_title_shmem_id, TITLE_MAX - 1, 0,
                                &status, &tlen, 0, 0);
                    if (status == GFX_STATUS_OK && tlen > 0) {
                        wasmos_shmem_refresh(g_title_shmem_id, (int32_t)(uintptr_t)g_title_ptr, tlen + 1);
                        for (int32_t k = 0; k < tlen && k < TITLE_MAX - 1; ++k)
                            win_title[k] = (char)g_title_ptr[k];
                        win_title[tlen] = '\0';
                    }
                }
                if (win_title[0] == '\0') {
                    win_title[0] = 'w'; win_title[1] = 'i'; win_title[2] = 'n'; win_title[3] = ' ';
                    int_to_str(ag->wids[w], win_title + 4, TITLE_MAX - 4);
                }
                ui_component_list_append(&g_ctx, g_app_mi_ids[i], win_title);
            }
        }
    }

    ui_mark_dirty(&g_ctx);
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

    /* "WasmOS" system menu - Reboot / Shutdown */
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

    /* Pre-allocate MAX_APP_ITEMS hidden menu item slots */
    for (int32_t i = 0; i < MAX_APP_ITEMS; ++i) {
        g_app_mi_ids[i] = ui_component_create_menu_item(&g_ctx);
        ui_component_t *mi = ui_component_by_id(&g_ctx, g_app_mi_ids[i]);
        if (mi) {
            mi->bg_color    = 0xFF1A2233u;
            mi->fg_color    = 0xFFDDE8F0u;
            mi->padding_px  = 8;
            mi->preferred_h = 0;  /* hidden until activated */
            mi->clickable   = 1;
            mi->on_click    = on_app_item_click;
        }
        ui_component_append_child(&g_ctx, g_ctx.root_id, g_app_mi_ids[i]);
    }

    /* Title shmem for GET_WINDOW_TITLE */
    g_title_shmem_id = wasmos_shmem_create(1, 0);
    if (g_title_shmem_id > 0) {
        int32_t mapped = wasmos_shmem_map_auto(g_title_shmem_id, 4096);
        if (mapped >= 0) g_title_ptr = (uint8_t *)(uintptr_t)(uint32_t)mapped;
    }

    refresh_app_list();
    update_clock();

    wasmos_ipc_message_t msg;
    int32_t refresh_ctr = 0;
    int32_t clock_ctr   = 0;

    while (!g_ctx.close_requested) {
        if (wasmos_ipc_call(g_ctx.gfx_endpoint, g_ctx.reply_endpoint,
                            GFX_IPC_POLL_EVENT, g_ctx.req_id++,
                            0, 0, 0, 0, &msg) == 0)
            ui_loop_handle_ipc(&g_ctx, &msg);

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

    ui_destroy(&g_ctx);
    return 0;
}
