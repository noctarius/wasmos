/* menu_bar - the system menu bar, and the reference for libui's menu system.
 *
 * A full application rather than a smoke test: it owns the top-of-screen bar
 * with a "WasmOS" system menu (Reboot, Shutdown) and an "Apps" menu that lists
 * the running applications, grouped by owning process, with one entry per
 * window that focuses it.
 *
 * What it demonstrates:
 *   - ui_menu_bar_init, which creates a topmost, chrome-less, task-list-less
 *     window spanning the display and roots the tree in a MENU_BAR;
 *   - building menus as a component tree: ui_component_create_menu_item for a
 *     bar item, ui_menu_item_add_item for each entry, and a leaf's on_click for
 *     the action. Popups are separate compositor windows managed by libui;
 *   - an event loop that blocks. wasmos_ipc_select_wait_timeout parks on the
 *     event endpoint with a ~250 ms bound, so pushed events wake it at once
 *     while the clock and the app list still refresh at idle without polling;
 *   - reading a service reply into a shmem buffer: GFX_IPC_GET_WINDOW_TITLE
 *     writes into a region this app created and mapped once, and
 *     wasmos_shmem_refresh must be called before the bytes are read back;
 *   - wasmos_sync_user_read after wasmos_proc_info_stats, because the kernel
 *     writes those structs into linear memory behind the guest's back.
 *
 * Two identity pitfalls are worth copying: wasmos_ipc_endpoint_owner returns a
 * context id, not a pid, which is why process lookup matches on
 * wasmos_proc_stats_t::context_id; and MENU_BAR layout reads a child's
 * preferred_h as its WIDTH.
 *
 * The app-list rebuild tears down and recreates the Apps subtree, so it is
 * skipped while that menu is open — rebuilding under an open popup would
 * destroy the popup windows and drop focus.
 *
 * Preconditions: the "gfx" and "font" services must be running. The "rtc"
 * service is optional: without it the clock is simply not drawn. */
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

static ui_context_t g_ctx;
static int32_t g_rtc_endpoint = -1;

/* ---- clock ---- */

static void pad2(char* out, int32_t v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

static void format_clock(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t min,
                         int32_t sec, char* buf) {
    buf[0] = (char)('0' + (year / 1000) % 10);
    buf[1] = (char)('0' + (year / 100) % 10);
    buf[2] = (char)('0' + (year / 10) % 10);
    buf[3] = (char)('0' + year % 10);
    buf[4] = '-';
    pad2(buf + 5, month);
    buf[7] = '-';
    pad2(buf + 8, day);
    buf[10] = ' ';
    pad2(buf + 11, hour);
    buf[13] = ':';
    pad2(buf + 14, min);
    buf[16] = ':';
    pad2(buf + 17, sec);
    buf[19] = '\0';
}

static void update_clock(void) {
    if (g_rtc_endpoint < 0)
        return;
    wasmos_ipc_message_t reply;
    if (wasmos_ipc_call(g_rtc_endpoint,
                        g_ctx.reply_endpoint,
                        RTC_IPC_READ_REQ,
                        g_ctx.req_id++,
                        0,
                        0,
                        0,
                        0,
                        &reply) != 0)
        return;
    if (reply.type != RTC_IPC_READ_RESP)
        return;
    const int32_t a0 = reply.arg0, a1 = reply.arg1;
    char buf[24];
    format_clock((a1 >> 8) & 0xFFFF,
                 a1 & 0xFF,
                 (a0 >> 24) & 0xFF,
                 (a0 >> 16) & 0xFF,
                 (a0 >> 8) & 0xFF,
                 a0 & 0xFF,
                 buf);
    ui_menu_bar_set_clock(&g_ctx, g_ctx.root_id, buf);
}

/* ---- app list globals ---- */

/* single "Apps" menu item */
static int32_t g_apps_mi_id = -1;

/* reusable shmem for title fetch */
static int32_t g_title_shmem_id = -1;
static uint8_t* g_title_ptr = NULL;

/* ---- callbacks ---- */

static void on_reboot(ui_context_t* ctx, int32_t id, void* user) {
    (void)ctx;
    (void)id;
    (void)user;
    (void)wasmos_system_reboot();
}

static void on_shutdown(ui_context_t* ctx, int32_t id, void* user) {
    (void)ctx;
    (void)id;
    (void)user;
    (void)wasmos_system_halt();
}

static void on_window_focus(ui_context_t* ctx, int32_t id, void* user) {
    (void)id;
    int32_t wid = (int32_t)(intptr_t)user;
    int32_t status = 0;
    ui_send_gfx(ctx->gfx_endpoint,
                ctx->reply_endpoint,
                ctx->req_id++,
                GFX_IPC_FOCUS_WINDOW,
                wid,
                0,
                0,
                0,
                &status,
                0,
                0,
                0);
}

/* ---- helpers ---- */

static void int_to_str(int32_t v, char* buf, int32_t cap) {
    if (cap <= 1)
        return;
    if (v <= 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[16];
    int32_t n = 0;
    while (v > 0 && n < 15) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int32_t out = 0;
    for (int32_t i = n - 1; i >= 0 && out < cap - 1; --i)
        buf[out++] = tmp[i];
    buf[out] = '\0';
}

static int32_t fetch_title(int32_t window_id, char* out, int32_t cap) {
    if (!g_title_ptr || g_title_shmem_id <= 0)
        return 0;
    int32_t status = 0, tlen = 0;
    ui_send_gfx(g_ctx.gfx_endpoint,
                g_ctx.reply_endpoint,
                g_ctx.req_id++,
                GFX_IPC_GET_WINDOW_TITLE,
                window_id,
                g_title_shmem_id,
                cap - 1,
                0,
                &status,
                &tlen,
                0,
                0);
    if (status != WASMOS_ERR_NONE || tlen <= 0)
        return 0;
    wasmos_shmem_refresh(g_title_shmem_id, addr_cast(int32_t, g_title_ptr), tlen + 1);
    for (int32_t k = 0; k < tlen && k < cap - 1; ++k)
        out[k] = (char)g_title_ptr[k];
    out[tlen < cap - 1 ? tlen : cap - 1] = '\0';
    return tlen;
}

/* wasmos_ipc_endpoint_owner() returns a context_id, NOT a PID.
 * wasmos_proc_info_stats() returns a stats struct that contains context_id,
 * which is what makes the match correct. */
static int32_t context_to_name(int32_t context_id, char* out, int32_t cap) {
    const int32_t total = wasmos_proc_count();
    for (int32_t i = 0; i < total; ++i) {
        char namebuf[64] = {0};
        uint32_t parent_pid = 0;
        wasmos_proc_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        const int32_t rc = wasmos_proc_info_stats(i,
                                                  addr_cast(int32_t, namebuf),
                                                  (int32_t)sizeof(namebuf),
                                                  addr_cast(int32_t, &parent_pid),
                                                  addr_cast(int32_t, &stats));
        if (rc < 0)
            continue;
        /* Kernel writes into WASM linear memory via mm_copy_to_user; sync
         * is required for the data to be visible on the WASM side. */
        if (wasmos_sync_user_read(addr_cast(int32_t, namebuf), (int32_t)sizeof(namebuf)) != 0 ||
            wasmos_sync_user_read(addr_cast(int32_t, &parent_pid), (int32_t)sizeof(parent_pid)) !=
                0 ||
            wasmos_sync_user_read(addr_cast(int32_t, &stats), (int32_t)sizeof(stats)) != 0) {
            continue;
        }
        if ((int32_t)stats.context_id == context_id) {
            int32_t n = 0;
            while (namebuf[n] && n < cap - 1) {
                out[n] = namebuf[n];
                n++;
            }
            out[n] = '\0';
            return n;
        }
    }
    return 0;
}

/* Remove all children of parent_id (and their descendants) from the component pool. */
static void remove_all_children(ui_context_t* ctx, int32_t parent_id) {
    ui_component_t* parent = ui_component_by_id(ctx, parent_id);
    if (!parent)
        return;
    int32_t child_id = parent->first_child_id;
    while (child_id > 0) {
        ui_component_t* child = ui_component_by_id(ctx, child_id);
        if (!child)
            break;
        int32_t next = child->next_sibling_id;
        remove_all_children(ctx, child_id);
        /* Close any open popup */
        ui_menu_item_data_t* d = (ui_menu_item_data_t*)child->component_data;
        if (d && d->popup_win_id > 0)
            ui_menu_item_popup_close(ctx, child);
        /* Reclaim slot */
        if (d) {
            if (d->text.text) {
                free(d->text.text);
                d->text.text = NULL;
            }
            if (d->popup_shmem_id > 0) {
                wasmos_shmem_unmap(d->popup_shmem_id);
                d->popup_shmem_id = 0;
            }
            free(d);
            child->component_data = NULL;
        }
        child->in_use = 0;
        child->parent_id = 0;
        child->first_child_id = 0;
        child->next_sibling_id = 0;
        child_id = next;
    }
    parent->first_child_id = 0;
}

/* ---- app list refresh ---- */

/* Large scratch buffers for refresh_app_list — kept static to avoid stack overflow
 * (default WASM stack is 4 KB; these arrays total ~6 KB). */
typedef struct {
    int32_t ctx_id;
    char name[32];
    int32_t win_ids[MAX_WINS_PER_APP];
    char win_titles[MAX_WINS_PER_APP][TITLE_MAX];
    int32_t count;
} AppGroup;

static AppGroup g_groups[MAX_APP_GROUPS];
static int32_t g_group_ctx_ids[MAX_APP_GROUPS];
static int32_t g_group_wids[MAX_APP_GROUPS][MAX_WINS_PER_APP];
static int32_t g_group_counts[MAX_APP_GROUPS];

static void refresh_app_list(void) {
    /* Don't tear down the component tree while the Apps popup is open —
     * remove_all_children destroys sub-popups and clears focus. */

    const ui_component_t* mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
    if (mi) {
        const ui_menu_item_data_t* md = (const ui_menu_item_data_t*)mi->component_data;
        if (md && md->dropdown_open)
            return;
    }

    int32_t ngroups = 0;
    for (int32_t i = 0; i < MAX_APP_GROUPS; ++i)
        g_group_ctx_ids[i] = g_group_counts[i] = 0;

    for (int32_t idx = 0; idx < 32; ++idx) {
        int32_t status = 0, wid = 0, owner_ep = 0;
        if (ui_send_gfx(g_ctx.gfx_endpoint,
                        g_ctx.reply_endpoint,
                        g_ctx.req_id++,
                        GFX_IPC_LIST_WINDOWS,
                        idx,
                        0,
                        0,
                        0,
                        &status,
                        &wid,
                        &owner_ep,
                        0) != 0)
            break;
        if (status != WASMOS_ERR_NONE || wid == 0)
            break;

        const int32_t ctx_id = wasmos_ipc_endpoint_owner(owner_ep);

        int32_t gi = -1;
        for (int32_t g = 0; g < ngroups; ++g)
            if (g_group_ctx_ids[g] == ctx_id) {
                gi = g;
                break;
            }
        if (gi < 0) {
            if (ngroups >= MAX_APP_GROUPS)
                continue;
            gi = ngroups++;
            g_group_ctx_ids[gi] = ctx_id;
            g_group_counts[gi] = 0;
        }
        if (g_group_counts[gi] < MAX_WINS_PER_APP)
            g_group_wids[gi][g_group_counts[gi]++] = wid;
    }

    /* Build groups with names and titles */
    for (int32_t i = 0; i < ngroups; ++i) {
        AppGroup* ag = &g_groups[i];
        ag->ctx_id = g_group_ctx_ids[i];
        ag->count = g_group_counts[i];
        for (int32_t w = 0; w < ag->count; ++w) {
            ag->win_ids[w] = g_group_wids[i][w];
            ag->win_titles[w][0] = '\0';
            fetch_title(ag->win_ids[w], ag->win_titles[w], TITLE_MAX);
        }
        ag->name[0] = '\0';
        if (!context_to_name(ag->ctx_id, ag->name, 32)) {
            if (ag->win_titles[0][0]) {
                for (int32_t k = 0; k < 31 && ag->win_titles[0][k]; ++k)
                    ag->name[k] = ag->win_titles[0][k];
            } else {
                ag->name[0] = 'A';
                ag->name[1] = 'p';
                ag->name[2] = 'p';
                ag->name[3] = ' ';
                int_to_str(ag->ctx_id, ag->name + 4, 27);
            }
        }
    }

    /* Rebuild the Apps menu item tree from scratch */
    remove_all_children(&g_ctx, g_apps_mi_id);

    for (int32_t i = 0; i < ngroups; ++i) {
        AppGroup* ag = &g_groups[i];
        if (ag->count <= 0)
            continue;

        if (ag->count == 1) {
            /* Single window: leaf item that focuses the window directly */
            const int32_t app_item_id = ui_menu_item_add_item(&g_ctx, g_apps_mi_id, ag->name);
            ui_component_t* app_mi = ui_component_by_id(&g_ctx, app_item_id);
            if (app_mi) {
                app_mi->clickable = 1;
                app_mi->on_click = on_window_focus;
                app_mi->on_click_user = (void*)(intptr_t)ag->win_ids[0];
            }
        } else {
            /* Multiple windows: app group item with per-window children */
            const int32_t app_item_id = ui_menu_item_add_item(&g_ctx, g_apps_mi_id, ag->name);
            for (int32_t w = 0; w < ag->count; ++w) {
                const char* title = ag->win_titles[w][0] ? ag->win_titles[w] : "window";
                const int32_t win_item_id = ui_menu_item_add_item(&g_ctx, app_item_id, title);
                ui_component_t* win_mi = ui_component_by_id(&g_ctx, win_item_id);
                if (win_mi) {
                    win_mi->clickable = 1;
                    win_mi->on_click = on_window_focus;
                    win_mi->on_click_user = (void*)(intptr_t)ag->win_ids[w];
                }
            }
        }
    }

    ui_mark_dirty(&g_ctx);
}

/* ---- main ---- */

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const int32_t proc_ep = (int32_t)(uint32_t)wasmos_startup_arg(0);
    const int32_t reply_ep = wasmos_ipc_create_endpoint();
    if (reply_ep < 0)
        return 1;

    if (ui_menu_bar_init(&g_ctx, proc_ep, reply_ep) != 0)
        return 1;

    for (int32_t spins = 0; spins < 512; ++spins) {
        g_rtc_endpoint = wasmos_svc_lookup(proc_ep, reply_ep, "rtc", g_ctx.req_id++);
        if (g_rtc_endpoint >= 0)
            break;
        (void)wasmos_sched_yield();
    }

    /* Title fetch shmem */
    g_title_shmem_id = wasmos_shmem_create(1, 0);
    if (g_title_shmem_id > 0) {
        int32_t mapped = wasmos_shmem_map_auto(g_title_shmem_id, 4096);
        if (mapped >= 0)
            g_title_ptr = ptr_cast(uint8_t, (uint32_t)mapped);
    }

    /* "WasmOS" system menu */
    const int32_t brand_id = ui_component_create_menu_item(&g_ctx);

    ui_component_t* brand = ui_component_by_id(&g_ctx, brand_id);
    if (brand) {
        brand->bg_color = 0xFF1A2233u;
        brand->fg_color = 0xFF88C4EEu;
        brand->padding_px = 10;
        brand->preferred_h = 90;
    }
    ui_component_set_text(&g_ctx, brand_id, "WasmOS");
    ui_component_append_child(&g_ctx, g_ctx.root_id, brand_id);

    /* Reboot and Shutdown as leaf children */
    const int32_t reboot_id = ui_menu_item_add_item(&g_ctx, brand_id, "Reboot");
    ui_component_t* reboot_mi = ui_component_by_id(&g_ctx, reboot_id);
    if (reboot_mi) {
        reboot_mi->clickable = 1;
        reboot_mi->on_click = on_reboot;
    }

    const int32_t shutdown_id = ui_menu_item_add_item(&g_ctx, brand_id, "Shutdown");
    ui_component_t* shutdown_mi = ui_component_by_id(&g_ctx, shutdown_id);
    if (shutdown_mi) {
        shutdown_mi->clickable = 1;
        shutdown_mi->on_click = on_shutdown;
    }

    /* "Apps" menu item */
    g_apps_mi_id = ui_component_create_menu_item(&g_ctx);

    ui_component_t* apps_mi = ui_component_by_id(&g_ctx, g_apps_mi_id);
    if (apps_mi) {
        apps_mi->bg_color = 0xFF1A2233u;
        apps_mi->fg_color = 0xFFDDE8F0u;
        apps_mi->padding_px = 10;
        apps_mi->preferred_h = 70;
    }
    ui_component_set_text(&g_ctx, g_apps_mi_id, "Apps");
    ui_component_append_child(&g_ctx, g_ctx.root_id, g_apps_mi_id);

    refresh_app_list();
    update_clock();

    wasmos_ipc_message_t msg;
    int32_t refresh_ctr = 0;
    int32_t clock_ctr = 0;

    /* Block on pushed events with a periodic timeout so the clock/app-list
     * still update at idle, instead of busy-polling the compositor. Events wake
     * the wait immediately; the timeout (~1/4 s) only bounds the idle sleep. */
    const int32_t MENU_BAR_TICK_MS = 250;
    const int32_t CLOCK_WAKES = 4;    /* ~1 s clock refresh */
    const int32_t REFRESH_WAKES = 40; /* ~10 s app-list refresh */
    int32_t sel = wasmos_ipc_select_create();
    if (sel < 0 || wasmos_ipc_select_add(sel, g_ctx.event_endpoint) != 0) {
        ui_destroy(&g_ctx);
        return 1;
    }

    while (!g_ctx.close_requested) {
        (void)wasmos_ipc_select_wait_timeout(sel, MENU_BAR_TICK_MS);
        while (wasmos_ipc_drain(g_ctx.event_endpoint) > 0) {
            wasmos_ipc_message_read_last(&msg);
            ui_loop_handle_ipc(&g_ctx, &msg);
        }

        if (++clock_ctr >= CLOCK_WAKES) {
            clock_ctr = 0;
            update_clock();
        }
        if (++refresh_ctr >= REFRESH_WAKES) {
            refresh_ctr = 0;
            refresh_app_list();
        }

        ui_loop_drain(&g_ctx);
    }

    ui_destroy(&g_ctx);
    return 0;
}
