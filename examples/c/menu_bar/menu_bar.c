#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/libui.h"
#include "wasmos/proc.h"
#include "wasmos/startup.h"

#define MENU_REFRESH_TICKS 200
#define CLOCK_REFRESH_TICKS 50
#define MAX_APP_GROUPS 12
#define MAX_WINS_PER_APP 8
#define TITLE_MAX 48
#define POPUP_ITEM_H 22

static ui_context_t g_ctx;
static int32_t g_rtc_endpoint = -1;

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

/* ---- app groups ---- */

typedef struct {
    int32_t pid;
    char    name[32];
    int32_t win_ids[MAX_WINS_PER_APP];
    char    win_titles[MAX_WINS_PER_APP][TITLE_MAX];
    int32_t count;
} AppGroup;

static AppGroup g_groups[MAX_APP_GROUPS];
static int32_t  g_ngroups = 0;

/* single "Apps" menu item */
static int32_t g_apps_mi_id = -1;

/* reusable shmem for title fetch */
static int32_t  g_title_shmem_id = -1;
static uint8_t *g_title_ptr       = NULL;

/* ---- sub-popup (second-level, per app, shows window titles) ---- */

static int32_t  g_sub_app_idx     = -1;
static int32_t  g_sub_win_id      = 0;
static int32_t  g_sub_buf_id      = 0;
static int32_t  g_sub_shmem_id    = 0;
static uint8_t *g_sub_base        = NULL;
static int32_t  g_sub_w           = 0;
static int32_t  g_sub_h           = 0;
static int32_t  g_sub_hovered     = -1;
static uint32_t g_sub_prev_btns   = 0;
static int32_t  g_sub_flushing    = 0;
static int32_t  g_last_apps_hov   = -1; /* last md->popup_hovered seen */

static void sub_render(int32_t hovered)
{
    if (!g_sub_base || g_sub_app_idx < 0 || g_sub_app_idx >= g_ngroups) return;
    AppGroup *ag = &g_groups[g_sub_app_idx];

    ui_rect_t full = {0, 0, g_sub_w, g_sub_h};
    ui_fill_rect(g_sub_base, g_sub_w, g_sub_h, 0, 0, g_sub_w, g_sub_h, 0xFF1A2840u);
    ui_stroke_rect_clip(g_sub_base, g_sub_w, g_sub_h, full, 1, 0xFF4A6080u, full);

    uint8_t *sb = g_ctx.mapped_base; int32_t sw = g_ctx.width, sh = g_ctx.height;
    g_ctx.mapped_base = g_sub_base; g_ctx.width = g_sub_w; g_ctx.height = g_sub_h;

    for (int32_t i = 0; i < ag->count; ++i) {
        if (i == hovered)
            ui_fill_rect(g_sub_base, g_sub_w, g_sub_h,
                         1, i * POPUP_ITEM_H, g_sub_w - 2, POPUP_ITEM_H, 0xFF2F5C88u);
        ui_draw_text_clip(&g_ctx, 8,
                          i * POPUP_ITEM_H + (POPUP_ITEM_H - g_ctx.font_px) / 2,
                          ag->win_titles[i][0] ? ag->win_titles[i] : "window",
                          0xFFFFFFFFu, full);
    }

    g_ctx.mapped_base = sb; g_ctx.width = sw; g_ctx.height = sh;
}

static void sub_present(void)
{
    if (!g_sub_base || !g_sub_win_id || !g_sub_buf_id) return;
    int32_t status = 0;
    wasmos_shmem_flush(g_sub_shmem_id, (int32_t)(uintptr_t)g_sub_base,
                       g_sub_w * g_sub_h * 4);
    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                GFX_IPC_PRESENT_WINDOW, g_sub_win_id, g_sub_buf_id, 0, 0,
                &status, 0, 0, 0);
}

static void sub_close(void)
{
    if (!g_sub_win_id) return;
    int32_t status = 0;
    if (g_sub_buf_id) {
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, g_sub_buf_id, 0, 0, 0,
                    &status, 0, 0, 0);
        g_sub_buf_id = 0;
    }
    if (g_sub_shmem_id > 0) { wasmos_shmem_unmap(g_sub_shmem_id); g_sub_shmem_id = 0; }
    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                GFX_IPC_DESTROY_WINDOW, g_sub_win_id, 0, 0, 0,
                &status, 0, 0, 0);
    g_sub_win_id = 0; g_sub_app_idx = -1; g_sub_base = NULL;
    g_sub_w = 0; g_sub_h = 0; g_sub_hovered = -1;
    g_sub_prev_btns = 0; g_sub_flushing = 0;
}

static void sub_open(int32_t app_idx)
{
    sub_close();
    if (app_idx < 0 || app_idx >= g_ngroups) return;
    AppGroup *ag = &g_groups[app_idx];
    if (ag->count <= 0) return;

    /* Position sub-popup to the right of the "Apps" bar item */
    ui_component_t *apps_mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
    const int32_t px = apps_mi ? (apps_mi->bounds.x + apps_mi->bounds.w) : 0;
    const int32_t py = g_ctx.height; /* below bar */
    const int32_t pw = 180;
    const int32_t ph = ag->count * POPUP_ITEM_H;

    int32_t status = 0, a1 = 0, a2 = 0, a3 = 0;
    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_CREATE_WINDOW, pw, ph,
                    (int32_t)GFX_IPC_ABI_MAGIC,
                    (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                    &status, &a1, &a2, &a3) != 0 || status != GFX_STATUS_OK) return;
    const int32_t win_id = a1;

    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_SET_WINDOW_FLAGS, win_id,
                    (int32_t)(GFX_WINDOW_FLAG_TOPMOST | GFX_WINDOW_FLAG_NO_CHROME |
                               GFX_WINDOW_FLAG_NO_TASK_LIST), 0, 0,
                    &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) goto fail;

    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_MOVE_WINDOW, win_id, px, py, 0,
                    &status, 0, 0, 0) != 0 || status != GFX_STATUS_OK) goto fail;

    int32_t buf_id = 0, shmem_id = 0, stride = 0;
    if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_ALLOC_SHARED_BUFFER, win_id, pw, ph, 0,
                    &status, &buf_id, &shmem_id, &stride) != 0 || status != GFX_STATUS_OK) goto fail;

    const int32_t bytes = (pw * ph * 4 + (UI_PAGE_SIZE - 1)) & ~(UI_PAGE_SIZE - 1);
    const int32_t mapped = wasmos_shmem_map_auto(shmem_id, bytes);
    if (mapped < 0) {
        ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                    GFX_IPC_RELEASE_SHARED_BUFFER, buf_id, 0, 0, 0, &status, 0, 0, 0);
        goto fail;
    }

    g_sub_win_id    = win_id;
    g_sub_buf_id    = buf_id;
    g_sub_shmem_id  = shmem_id;
    g_sub_base      = (uint8_t *)(uintptr_t)(uint32_t)mapped;
    g_sub_w         = pw;
    g_sub_h         = ph;
    g_sub_app_idx   = app_idx;
    g_sub_hovered   = -1;
    g_sub_prev_btns = 0;
    g_sub_flushing  = 1;

    sub_render(-1);
    sub_present();
    /* No GFX_IPC_FOCUS_WINDOW here: sub-popup starts in preview mode (no focus).
     * on_apps_click will give it focus when the user commits to this app group. */
    return;
fail:
    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                GFX_IPC_DESTROY_WINDOW, win_id, 0, 0, 0, &status, 0, 0, 0);
}

static void sub_handle_pointer(const wasmos_ipc_message_t *msg)
{
    if (!g_sub_base || g_sub_app_idx < 0) return;
    AppGroup *ag = &g_groups[g_sub_app_idx];
    const int32_t  py      = ui_u16_hi(msg->arg2);
    const uint32_t buttons = (uint32_t)msg->arg3;
    const int32_t  left_now  = (buttons & 1u) != 0;
    const int32_t  left_prev = (g_sub_prev_btns & 1u) != 0;
    g_sub_prev_btns = buttons;

    if (g_sub_flushing) {
        if (buttons & 1u) { return; }
        g_sub_flushing = 0; g_sub_prev_btns = 0;
    }

    const int32_t hov = (py >= 0 && py < g_sub_h) ? (py / POPUP_ITEM_H) : -1;
    if (hov != g_sub_hovered) {
        g_sub_hovered = hov;
        sub_render(hov);
        sub_present();
    }

    if (!left_now && left_prev) {
        if (hov >= 0 && hov < ag->count) {
            int32_t wid = ag->win_ids[hov];
            int32_t status = 0;
            ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                        GFX_IPC_FOCUS_WINDOW, wid, 0, 0, 0, &status, 0, 0, 0);
        }
        sub_close();
        ui_mark_dirty(&g_ctx);
    }
}

/* Open sub-popup on hover; only runs while the Apps popup itself is open. */
static void sync_sub_popup_from_hover(void)
{
    ui_component_t *mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
    if (!mi) return;
    ui_menu_item_data_t *md = (ui_menu_item_data_t *)mi->component_data;
    /* Do nothing if the Apps popup is not currently shown */
    if (!md || !md->dropdown_open || !md->popup_win_id) {
        g_last_apps_hov = -1;
        return;
    }
    const int32_t hov = md->popup_hovered;
    if (hov == g_last_apps_hov) return;
    g_last_apps_hov = hov;

    if (hov >= 0 && hov < g_ngroups && g_groups[hov].count > 1) {
        if (g_sub_app_idx != hov)
            sub_open(hov);
    } else {
        if (g_sub_win_id) sub_close();
    }
}

/* ---- app list refresh ---- */

static void int_to_str(int32_t v, char *buf, int32_t cap)
{
    if (cap <= 1) return;
    if (v <= 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int32_t n = 0;
    while (v > 0 && n < 15) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int32_t out = 0;
    for (int32_t i = n - 1; i >= 0 && out < cap - 1; --i) buf[out++] = tmp[i];
    buf[out] = '\0';
}

static int32_t fetch_title(int32_t window_id, char *out, int32_t cap)
{
    if (!g_title_ptr || g_title_shmem_id <= 0) return 0;
    int32_t status = 0, tlen = 0;
    ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                GFX_IPC_GET_WINDOW_TITLE, window_id, g_title_shmem_id, cap - 1, 0,
                &status, &tlen, 0, 0);
    if (status != GFX_STATUS_OK || tlen <= 0) return 0;
    wasmos_shmem_refresh(g_title_shmem_id, (int32_t)(uintptr_t)g_title_ptr, tlen + 1);
    for (int32_t k = 0; k < tlen && k < cap - 1; ++k) out[k] = (char)g_title_ptr[k];
    out[tlen < cap - 1 ? tlen : cap - 1] = '\0';
    return tlen;
}

static int32_t pid_to_name(int32_t pid, char *out, int32_t cap)
{
    const int32_t total = wasmos_proc_count();
    for (int32_t i = 0; i < total; ++i) {
        char namebuf[64] = {0};
        int32_t got_pid = wasmos_proc_info(i, (int32_t)(uintptr_t)namebuf, (int32_t)sizeof(namebuf));
        if (got_pid == pid) {
            int32_t n = 0;
            while (namebuf[n] && n < cap - 1) { out[n] = namebuf[n]; n++; }
            out[n] = '\0';
            return n;
        }
    }
    return 0;
}

static void refresh_app_list(void)
{
    /* Collect windows grouped by PID */
    int32_t group_pids[MAX_APP_GROUPS]  = {0};
    int32_t group_wids[MAX_APP_GROUPS][MAX_WINS_PER_APP];
    int32_t group_counts[MAX_APP_GROUPS] = {0};
    int32_t ngroups = 0;

    for (int32_t idx = 0; idx < 32; ++idx) {
        int32_t status = 0, wid = 0, owner_ep = 0;
        if (ui_send_gfx(g_ctx.gfx_endpoint, g_ctx.reply_endpoint, g_ctx.req_id++,
                        GFX_IPC_LIST_WINDOWS, idx, 0, 0, 0,
                        &status, &wid, &owner_ep, 0) != 0) break;
        if (status != GFX_STATUS_OK || wid == 0) break;

        const int32_t pid = wasmos_ipc_endpoint_owner(owner_ep);

        int32_t gi = -1;
        for (int32_t g = 0; g < ngroups; ++g)
            if (group_pids[g] == pid) { gi = g; break; }
        if (gi < 0) {
            if (ngroups >= MAX_APP_GROUPS) continue;
            gi = ngroups++;
            group_pids[gi] = pid;
            group_counts[gi] = 0;
        }
        if (group_counts[gi] < MAX_WINS_PER_APP)
            group_wids[gi][group_counts[gi]++] = wid;
    }

    /* Build g_groups with names and titles */
    g_ngroups = ngroups;
    for (int32_t i = 0; i < ngroups; ++i) {
        AppGroup *ag = &g_groups[i];
        ag->pid   = group_pids[i];
        ag->count = group_counts[i];
        for (int32_t w = 0; w < ag->count; ++w) {
            ag->win_ids[w] = group_wids[i][w];
            ag->win_titles[w][0] = '\0';
            fetch_title(ag->win_ids[w], ag->win_titles[w], TITLE_MAX);
        }
        /* App name = process name */
        ag->name[0] = '\0';
        if (!pid_to_name(ag->pid, ag->name, 32)) {
            /* Fallback: first window title or "App N" */
            if (ag->win_titles[0][0])
                for (int32_t k = 0; k < 31 && ag->win_titles[0][k]; ++k)
                    ag->name[k] = ag->win_titles[0][k];
            else {
                ag->name[0]='A'; ag->name[1]='p'; ag->name[2]='p'; ag->name[3]=' ';
                int_to_str(ag->pid, ag->name + 4, 27);
            }
        }
    }

    /* Repopulate the Apps menu item list */
    ui_component_t *mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
    ui_menu_item_data_t *md = mi ? (ui_menu_item_data_t *)mi->component_data : NULL;
    if (!md) return;

    if (md->list.items) {
        for (int32_t j = 0; j < md->list.count; ++j)
            if (md->list.items[j]) { free(md->list.items[j]); md->list.items[j] = 0; }
    }
    md->list.count = 0;
    md->list.selected = -1;

    for (int32_t i = 0; i < ngroups; ++i)
        ui_component_list_append(&g_ctx, g_apps_mi_id, g_groups[i].name);

    /* If sub-popup is open for an app whose index shifted, close it */
    if (g_sub_app_idx >= ngroups) sub_close();

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
    if (sel == 0) (void)wasmos_system_reboot();
    else if (sel == 1) (void)wasmos_system_halt();
}

static void on_apps_click(ui_context_t *ctx, int32_t component_id, void *user)
{
    (void)ctx; (void)component_id; (void)user;
    ui_component_t *mi = ui_component_by_id(ctx, g_apps_mi_id);
    if (!mi) return;
    ui_menu_item_data_t *md = (ui_menu_item_data_t *)mi->component_data;
    const int32_t sel = md ? md->list.selected : -1;
    if (sel < 0 || sel >= g_ngroups) return;

    AppGroup *ag = &g_groups[sel];
    if (ag->count == 1) {
        /* Single window: focus directly */
        int32_t status = 0;
        ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                    GFX_IPC_FOCUS_WINDOW, ag->win_ids[0], 0, 0, 0,
                    &status, 0, 0, 0);
    } else {
        /* Multi-window: hover already opened sub-popup in preview mode.
         * Give it focus now so the user can click window titles. */
        if (g_sub_app_idx != sel) sub_open(sel); /* shouldn't be needed, but guard */
        if (g_sub_win_id > 0) {
            int32_t status = 0;
            ui_send_gfx(ctx->gfx_endpoint, ctx->reply_endpoint, ctx->req_id++,
                        GFX_IPC_FOCUS_WINDOW, g_sub_win_id, 0, 0, 0,
                        &status, 0, 0, 0);
        }
    }
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

    /* Title fetch shmem */
    g_title_shmem_id = wasmos_shmem_create(1, 0);
    if (g_title_shmem_id > 0) {
        int32_t mapped = wasmos_shmem_map_auto(g_title_shmem_id, 4096);
        if (mapped >= 0) g_title_ptr = (uint8_t *)(uintptr_t)(uint32_t)mapped;
    }

    /* "WasmOS" system menu */
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

            if (g_sub_win_id > 0) {
                if (msg.arg1 == GFX_EVENT_POINTER) {
                    sub_handle_pointer(&msg);
                } else if ((msg.arg1 == GFX_EVENT_FOCUS_LOST ||
                             msg.arg1 == GFX_EVENT_CLOSE_REQUEST) &&
                            (int32_t)msg.arg2 == g_sub_win_id) {
                    sub_close();
                    ui_mark_dirty(&g_ctx);
                } else {
                    ui_loop_handle_ipc(&g_ctx, &msg);
                }
            } else {
                ui_loop_handle_ipc(&g_ctx, &msg);
            }
        }

        sync_sub_popup_from_hover();
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

    sub_close();
    ui_destroy(&g_ctx);
    return 0;
}
