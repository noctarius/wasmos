#include <stdint.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/libui.h"
#include "wasmos/startup.h"
#include "wasmo_mascot_rgba.h"

/* Mirrors the WASMOS_TRACE cmake flag (-DWASMOS_TRACE=ON); can also be forced
 * locally by defining GFX_SMOKE_TRACE=1 on the compiler command line. */
#ifndef GFX_SMOKE_TRACE
#define GFX_SMOKE_TRACE WASMOS_TRACE
#endif

#define GFX_REQ_BASE 0x6A00
#define FBPP 4
#define PAGE_SIZE 4096
#define GFX_W 64
#define GFX_H 64
#define GFX_RESIZE_W 320
#define GFX_RESIZE_H 180
#define GFX_FRAME_COUNT 8
#define GFX2_W 220
#define GFX2_H 140
#define GFX3_W 560
#define GFX3_H 560

typedef struct {
    int32_t status;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
} gfx_reply_t;

enum {
    GFX_SMOKE_E_SETUP = 11,
    GFX_SMOKE_E_NO_GFX = 12,
    GFX_SMOKE_E_CREATE = 13,
    GFX_SMOKE_E_ALLOC0 = 14,
    GFX_SMOKE_E_MAP0 = 15,
    GFX_SMOKE_E_PAINT0 = 16,
    GFX_SMOKE_E_PRESENT0 = 17,
    GFX_SMOKE_E_RESIZE = 18,
    GFX_SMOKE_E_UNMAP0 = 19,
    GFX_SMOKE_E_ALLOC1 = 20,
    GFX_SMOKE_E_MAP1 = 21,
    GFX_SMOKE_E_DAMAGE = 22,
    GFX_SMOKE_E_PAINT_LOOP = 23,
    GFX_SMOKE_E_PRESENT_LOOP = 24,
    GFX_SMOKE_E_INVALID_DENY = 25,
    GFX_SMOKE_E_RELEASE1 = 26,
    GFX_SMOKE_E_UNMAP1 = 27,
    GFX_SMOKE_E_RELEASE2 = 28,
    GFX_SMOKE_E_DESTROY = 29,
    GFX_SMOKE_E_POST_DESTROY = 30,
    GFX_SMOKE_E_EVENT_FOCUS = 31,
    GFX_SMOKE_E_EVENT_CLOSE = 32
};

static ui_context_t g_ctx1;
static ui_context_t g_ctx2;
static ui_context_t g_ctx3;

static int32_t
create_damage_rect_shmem(int32_t gfx_ep, int32_t width, int32_t height)
{
    int32_t shmem_id = wasmos_shmem_create(1, 0);
    if (shmem_id <= 0) {
        puts("[test] gfx smoke damage alloc failed");
        return -1;
    }
    int32_t gfx_owner = wasmos_ipc_endpoint_owner(gfx_ep);
    if (gfx_owner <= 0) {
        puts("[test] gfx smoke damage owner failed");
        return -1;
    }
    if (wasmos_shmem_grant(shmem_id, gfx_owner) != 0) {
        puts("[test] gfx smoke damage grant failed");
        return -1;
    }
    int32_t map_ptr = wasmos_shmem_map_auto(shmem_id, PAGE_SIZE);
    if (map_ptr < 0) {
        puts("[test] gfx smoke damage map failed");
        return -1;
    }
    gfx_rect_t *rect = (gfx_rect_t *)(uintptr_t)(uint32_t)map_ptr;
    rect->x = 0;
    rect->y = 0;
    rect->w = width;
    rect->h = height;
    if (wasmos_shmem_flush(shmem_id, map_ptr, (int32_t)sizeof(gfx_rect_t)) != 0) {
        puts("[test] gfx smoke damage flush failed");
        return -1;
    }
    if (wasmos_shmem_unmap(shmem_id) != 0) {
        puts("[test] gfx smoke damage unmap failed");
        return -1;
    }
    return shmem_id;
}

static int
map_shared_buffer_ptr(int32_t shmem_id, int32_t stride_bytes, int32_t height, uint8_t **out_base)
{
    int32_t byte_len = stride_bytes * height;
    int32_t map_len = (byte_len + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
    int32_t map_ptr = wasmos_shmem_map_auto(shmem_id, map_len);
    if (map_ptr < 0) {
        puts("[test] gfx smoke shmem map failed");
        return -1;
    }
    *out_base = (uint8_t *)(uintptr_t)(uint32_t)map_ptr;
    return 0;
}

static int
flush_shared_buffer_ptr(int32_t shmem_id, uint8_t *base, int32_t stride_bytes, int32_t height)
{
    int32_t byte_len = stride_bytes * height;
    int32_t ptr = (int32_t)(uintptr_t)base;
    if (wasmos_shmem_flush(shmem_id, ptr, byte_len) != 0) {
        puts("[test] gfx smoke shmem flush failed");
        return -1;
    }
    return 0;
}

static int
fill_pattern(uint8_t *base, int32_t width, int32_t height, int32_t stride_bytes, uint32_t phase)
{
    (void)stride_bytes;
    const int32_t packed_stride = width * FBPP;
    for (int32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)(void *)(base + (y * packed_stride));
        for (int32_t x = 0; x < width; ++x) {
            uint32_t r = (uint32_t)((x + (int32_t)phase) & 0xFF);
            uint32_t g = (uint32_t)((y + (int32_t)(phase * 3u)) & 0xFF);
            uint32_t b = (uint32_t)((x + y + (int32_t)(phase * 5u)) & 0xFF);
            row[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return 0;
}

static void
fill_rect(uint8_t *base,
          int32_t width,
          int32_t height,
          int32_t stride_bytes,
          int32_t x,
          int32_t y,
          int32_t w,
          int32_t h,
          uint32_t color)
{
    (void)stride_bytes;
    const int32_t packed_stride = width * FBPP;
    if (w <= 0 || h <= 0) return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    for (int32_t yy = y0; yy < y1; ++yy) {
        uint32_t *row = (uint32_t *)(void *)(base + (yy * packed_stride));
        for (int32_t xx = x0; xx < x1; ++xx) {
            row[xx] = color;
        }
    }
}

static int
fill_wasmos_logo(uint8_t *base, int32_t width, int32_t height, int32_t stride_bytes)
{
    const uint32_t bg = 0xFF171A22u;
    const int32_t mascot_src_w = WASMO_MASCOT_RGBA_WIDTH;
    const int32_t mascot_src_h = WASMO_MASCOT_RGBA_HEIGHT;
    const int32_t mascot_draw_w = 500;
    const int32_t mascot_draw_h = 500;
    fill_rect(base, width, height, stride_bytes, 0, 0, width, height, bg);
    const int32_t off_x = (width - mascot_draw_w) / 2;
    const int32_t off_y = (height - mascot_draw_h) / 2;
    for (int32_t y = 0; y < mascot_draw_h; ++y) {
        uint32_t *dst_row = (uint32_t *)(void *)(base + ((off_y + y) * (width * FBPP)));
        const int32_t sy = (y * mascot_src_h) / mascot_draw_h;
        const int32_t src_row_off = sy * mascot_src_w * 4;
        for (int32_t x = 0; x < mascot_draw_w; ++x) {
            const int32_t sx = (x * mascot_src_w) / mascot_draw_w;
            const int32_t i = src_row_off + (sx * 4);
            const uint32_t sr = wasmo_mascot_rgba[i + 0];
            const uint32_t sg = wasmo_mascot_rgba[i + 1];
            const uint32_t sb = wasmo_mascot_rgba[i + 2];
            const uint32_t sa = wasmo_mascot_rgba[i + 3];
            if (sa > 8u) {
                dst_row[off_x + x] = (0xFFu << 24) | (sr << 16) | (sg << 8) | sb;
            }
        }
    }

    return 0;
}

static int
send_gfx(int32_t gfx_ep,
         int32_t reply_ep,
         int32_t req_id,
         int32_t opcode,
         int32_t arg0,
         int32_t arg1,
         int32_t arg2,
         int32_t arg3,
         gfx_reply_t *out)
{
    wasmos_ipc_message_t resp;
    if (wasmos_ipc_call(gfx_ep,
                        reply_ep,
                        opcode,
                        req_id,
                        arg0,
                        arg1,
                        arg2,
                        arg3,
                        &resp) != 0) {
        return -1;
    }
    if (resp.type != GFX_IPC_RESP && resp.type != GFX_IPC_ERROR) {
        return -1;
    }
    if (out) {
        out->status = resp.arg0;
        out->arg1 = resp.arg1;
        out->arg2 = resp.arg2;
        out->arg3 = resp.arg3;
    }
    return 0;
}

static int
poll_gfx_focus_event(int32_t gfx_ep, int32_t reply_ep, int32_t *req, int32_t expected_window_id)
{
    gfx_reply_t ev;
    for (int i = 0; i < 96; ++i) {
        if (send_gfx(gfx_ep, reply_ep, (*req)++, GFX_IPC_POLL_EVENT, 0, 0, 0, 0, &ev) != 0 ||
            ev.status != GFX_STATUS_OK) {
            return -1;
        }
        if (ev.arg1 == GFX_EVENT_NONE) {
            (void)wasmos_sched_yield();
            continue;
        }
        if (ev.arg1 == GFX_EVENT_FOCUS_GAINED && ev.arg2 == expected_window_id) {
            puts("[test] gfx smoke event focus-gained");
            return 0;
        }
    }
    return -1;
}

static int
poll_gfx_events_once(int32_t gfx_ep, int32_t reply_ep, int32_t *req, int32_t *out_close_window_id)
{
    gfx_reply_t ev;
    if (send_gfx(gfx_ep, reply_ep, (*req)++, GFX_IPC_POLL_EVENT, 0, 0, 0, 0, &ev) != 0 ||
        ev.status != GFX_STATUS_OK) {
        return -1;
    }
    if (ev.arg1 == GFX_EVENT_KEY) {
        char msg[96];
        int n = snprintf(msg, sizeof(msg),
                         "[test] gfx smoke event key sc=%d flags=%d\n",
                         ev.arg2, ev.arg3);
        if (n > 0) {
            (void)putsn(msg, (size_t)n);
        }
    } else if (ev.arg1 == GFX_EVENT_FOCUS_GAINED) {
#if GFX_SMOKE_TRACE
        puts("[test] gfx smoke event focus-gained");
#endif
    } else if (ev.arg1 == GFX_EVENT_FOCUS_LOST) {
#if GFX_SMOKE_TRACE
        puts("[test] gfx smoke event focus-lost");
#endif
    } else if (ev.arg1 == GFX_EVENT_POINTER) {
        /* keep pointer events silent to reduce log noise during compositor debug */
    } else if (ev.arg1 == GFX_EVENT_CLOSE_REQUEST) {
        puts("[test] gfx smoke event close-request");
        if (out_close_window_id) {
            *out_close_window_id = ev.arg2;
        }
        return 1;
    } else if (ev.arg1 == GFX_EVENT_RESIZE) {
        int32_t rw = (int32_t)(ev.arg3 & 0xFFFF);
        int32_t rh = (int32_t)((ev.arg3 >> 16) & 0xFFFF);
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "[test] gfx smoke event resize win=%d w=%d h=%d\n",
                         ev.arg2, rw, rh);
        if (n > 0) {
            (void)putsn(msg, (size_t)n);
        }
    }
    return 0;
}

static int
handle_resize_realloc_logo(int32_t gfx_ep, int32_t reply_ep, int32_t *req, ui_context_t *ctx, int32_t new_w, int32_t new_h)
{
    gfx_reply_t reply;
    if (new_w <= 0 || new_h <= 0) {
        return -1;
    }
    if (ui_realloc_buffer(ctx, new_w, new_h) != 0) {
        puts("[test] gfx smoke resize3 alloc failed");
        return -1;
    }
    if (new_w >= 500 && new_h >= 500) {
        if (fill_wasmos_logo(ctx->mapped_base, new_w, new_h, ctx->stride_bytes) != 0) {
            return -1;
        }
    } else {
        if (fill_pattern(ctx->mapped_base, new_w, new_h, ctx->stride_bytes, 77u) != 0) {
            return -1;
        }
    }
    if (flush_shared_buffer_ptr(ctx->shmem_id, ctx->mapped_base, ctx->stride_bytes, new_h) != 0) {
        return -1;
    }
    if (send_gfx(gfx_ep, reply_ep, (*req)++, GFX_IPC_PRESENT_WINDOW,
                 ctx->window_id, ctx->buffer_id, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize3 present failed");
        return -1;
    }
    return 0;
}

static int
handle_resize_realloc(int32_t gfx_ep, int32_t reply_ep, int32_t *req, ui_context_t *ctx, int32_t new_w, int32_t new_h, uint32_t phase)
{
    gfx_reply_t reply;
    if (new_w <= 0 || new_h <= 0) {
        return -1;
    }
    if (ui_realloc_buffer(ctx, new_w, new_h) != 0) {
        puts("[test] gfx smoke resize alloc failed");
        return -1;
    }
    if (fill_pattern(ctx->mapped_base, new_w, new_h, ctx->stride_bytes, phase) != 0) {
        puts("[test] gfx smoke resize paint failed");
        return -1;
    }
    if (flush_shared_buffer_ptr(ctx->shmem_id, ctx->mapped_base, ctx->stride_bytes, new_h) != 0) {
        return -1;
    }
    if (send_gfx(gfx_ep, reply_ep, (*req)++, GFX_IPC_PRESENT_WINDOW,
                 ctx->window_id, ctx->buffer_id, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize present failed");
        return -1;
    }
    return 0;
}

static void
ui_demo_button_click(ui_context_t *ctx, int32_t component_id, void *user)
{
    (void)component_id;
#if GFX_SMOKE_TRACE
    puts("[dbg-libui] on_click fired");
#endif
    int32_t *click_count = (int32_t *)user;
    ui_component_t *root = ui_component_by_id(ctx, ctx->root_id);
    ui_component_t *label = ui_component_by_id(ctx, 2);
    ui_component_t *button = ui_component_by_id(ctx, 3);
    ui_component_t *checkbox = ui_component_by_id(ctx, 4);
    if (click_count) {
        (*click_count)++;
    }
    if (root) {
        root->bg_color = (click_count && ((*click_count) & 1)) ? 0xFF253248u : 0xFF202833u;
    }
    if (label) {
        label->fg_color = (click_count && ((*click_count) & 1)) ? 0xFF9CE2FFu : 0xFFFFFFFFu;
    }
    if (button) {
        button->border_color = (click_count && ((*click_count) & 1)) ? 0xFF9CE2FFu : 0xFF536271u;
    }
    if (checkbox) {
        ui_component_set_text(ctx, checkbox->id, ui_component_get_checked(checkbox) ? "checkbox: on" : "checkbox: off");
    }
    ui_mark_dirty(ctx);
}

static ui_context_t g_libui_ctx;
static int32_t g_libui_click_count = 0;

static int
start_libui_demo(int32_t proc_endpoint)
{
    ui_context_t *ui = &g_libui_ctx;
    g_libui_click_count = 0;
    if (ui_init(ui, proc_endpoint, wasmos_ipc_create_endpoint(), 520, 360) != 0) {
        puts("[test] libui demo init failed");
        return -1;
    }
    (void)ui_window_set_title(ui, "libui demo");

    int32_t label = ui_component_create_label(ui);
    int32_t button = ui_component_create_button(ui);
    int32_t checkbox = ui_component_create_checkbox(ui);
    int32_t input = ui_component_create_text_input(ui);
    int32_t list = ui_component_create_list_view(ui);
    int32_t panel = ui_component_create_panel(ui);
    if (label < 0 || button < 0 || checkbox < 0 || input < 0 || list < 0 || panel < 0) {
        ui_destroy(ui);
        return -1;
    }
    ui_component_t *p = ui_component_by_id(ui, panel);
    ui_component_t *l = ui_component_by_id(ui, label);
    ui_component_t *b = ui_component_by_id(ui, button);
    ui_component_t *cb = ui_component_by_id(ui, checkbox);
    ui_component_t *ti = ui_component_by_id(ui, input);
    ui_component_t *lv = ui_component_by_id(ui, list);
    if (!p || !l || !b || !cb || !ti || !lv) {
        ui_destroy(ui);
        return -1;
    }
    p->preferred_h = 308;
    p->bg_color = 0xFF1A2230u;
    p->padding_px = 8;
    p->gap_px = 8;
    l->preferred_h = 28;
    l->bg_color = 0xFF2A3550u;
    b->preferred_h = 34;
    b->bg_color = 0xFF2A3550u;
    cb->preferred_h = 28;
    cb->bg_color = 0xFF243147u;
    cb->clickable = 1;
    ti->preferred_h = 30;
    ti->bg_color = 0xFF1D2838u;
    ti->border_color = 0xFF4C627D;
    ti->clickable = 1;
    lv->preferred_h = 92;
    lv->bg_color = 0xFF1A2534u;
    lv->border_color = 0xFF4C627D;
    lv->padding_px = 6;
    lv->gap_px = 4;
    (void)ui_component_list_append(ui, list, "list row 1");
    (void)ui_component_list_append(ui, list, "list row 2");
    (void)ui_component_list_append(ui, list, "list row 3");
    (void)ui_component_list_append(ui, list, "list row 4");
    (void)ui_component_list_append(ui, list, "list row 5");
    (void)ui_component_list_append(ui, list, "list row 6");
    (void)ui_component_list_append(ui, list, "list row 7");
    (void)ui_component_list_append(ui, list, "list row 8");
    ui_component_set_checked(ui, checkbox, 0);
    ui_component_set_text(ui, label, "libui component demo");
    ui_component_set_text(ui, button, "press me");
    ui_component_set_text(ui, checkbox, "checkbox: off");
    ui_component_set_text(ui, input, "type here");
    ui_component_set_button_action(ui, button, ui_demo_button_click, &g_libui_click_count);
    ui_component_set_button_action(ui, checkbox, ui_demo_button_click, &g_libui_click_count);
    (void)ui_component_append_child(ui, ui->root_id, panel);
    (void)ui_component_append_child(ui, panel, label);
    (void)ui_component_append_child(ui, panel, button);
    (void)ui_component_append_child(ui, panel, checkbox);
    (void)ui_component_append_child(ui, panel, input);
    (void)ui_component_append_child(ui, panel, list);
    ui_mark_dirty(ui);
    if (ui_loop_drain(ui) != 0) {
        ui_destroy(ui);
        return -1;
    }

    puts("[test] libui demo ready");
    return 0;
}

static int
pump_libui_demo(void)
{
    ui_context_t *ui = &g_libui_ctx;
    if (ui->window_id <= 0) return 0;
    wasmos_ipc_message_t ev_raw;
    if (!ui->close_requested &&
        ui_send_gfx_raw(ui->gfx_endpoint, ui->reply_endpoint, ui->req_id++,
                        GFX_IPC_POLL_EVENT, 0, 0, 0, 0, &ev_raw) == 0) {
#if GFX_SMOKE_TRACE
        if (ev_raw.arg1 == GFX_EVENT_POINTER) {
            printf("[gfx-t] libui ptr win=%d x=%d y=%d btn=%d\n",
                   ev_raw.arg2,
                   (int)(ev_raw.arg3 & 0xFFF),
                   (int)((ev_raw.arg3 >> 12) & 0xFFF),
                   (int)((ev_raw.arg3 >> 24) & 1));
            if (((ev_raw.arg3 >> 24) & 1) != 0) {
                puts("[dbg-libui] pointer btn-down");
            }
        }
#endif
        (void)ui_loop_handle_ipc(ui, &ev_raw);
    }
    if (ui_loop_drain(ui) != 0) {
        return -1;
    }
    return 0;
}

static void
stop_libui_demo(void)
{
    ui_context_t *ui = &g_libui_ctx;
    if (ui->window_id > 0) {
        ui_destroy(ui);
        puts("[test] libui demo done");
    }
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    puts("[test] gfx smoke main start");

    int32_t proc_endpoint = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t gfx_ep = -1;
    int32_t req = GFX_REQ_BASE;
    gfx_reply_t reply;
    int32_t damage_shmem_id;
    int32_t libui_started = 0;

    if (proc_endpoint <= 0 || reply_ep < 0) {
        puts("[test] gfx smoke setup failed");
        return GFX_SMOKE_E_SETUP;
    }

    for (int spins = 0; spins < 2048; ++spins) {
        gfx_ep = wasmos_svc_lookup(proc_endpoint, reply_ep, "gfx", req++);
        if (gfx_ep >= 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (gfx_ep < 0) {
        puts("[test] gfx smoke no-gfx");
        return GFX_SMOKE_E_NO_GFX;
    }

    /* --- win1: ui_init for initial setup, then raw resize test sequence --- */
    if (ui_init(&g_ctx1, proc_endpoint, reply_ep, GFX_W, GFX_H) != 0) {
        puts("[test] gfx smoke create failed");
        return GFX_SMOKE_E_CREATE;
    }
    (void)ui_window_set_title(&g_ctx1, "gfx smoke 1");

    if (fill_pattern(g_ctx1.mapped_base, GFX_W, GFX_H, g_ctx1.stride_bytes, 1) != 0) {
        puts("[test] gfx smoke paint1 failed");
        return GFX_SMOKE_E_PAINT0;
    }
    if (flush_shared_buffer_ptr(g_ctx1.shmem_id, g_ctx1.mapped_base, g_ctx1.stride_bytes, GFX_H) != 0) {
        return GFX_SMOKE_E_PAINT0;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 g_ctx1.window_id, g_ctx1.buffer_id, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke present1 failed");
        return GFX_SMOKE_E_PRESENT0;
    }
    if (poll_gfx_focus_event(gfx_ep, reply_ep, &req, g_ctx1.window_id) != 0) {
        puts("[test] gfx smoke event focus missing");
        return GFX_SMOKE_E_EVENT_FOCUS;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RESIZE_WINDOW,
                 g_ctx1.window_id, GFX_RESIZE_W, GFX_RESIZE_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize failed");
        return GFX_SMOKE_E_RESIZE;
    }
    if (wasmos_shmem_unmap(g_ctx1.shmem_id) != 0) {
        puts("[test] gfx smoke shmem unmap failed");
        return GFX_SMOKE_E_UNMAP0;
    }

    {
        int32_t new_buffer_id = 0, new_shmem_id = 0, new_stride = 0;
        uint8_t *new_base = 0;
        if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_ALLOC_SHARED_BUFFER,
                     g_ctx1.window_id, GFX_RESIZE_W, GFX_RESIZE_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
            puts("[test] gfx smoke resize-alloc failed");
            return GFX_SMOKE_E_ALLOC1;
        }
        new_buffer_id = reply.arg1;
        new_shmem_id = reply.arg2;
        new_stride = reply.arg3;
        if (map_shared_buffer_ptr(new_shmem_id, new_stride, GFX_RESIZE_H, &new_base) != 0) {
            return GFX_SMOKE_E_MAP1;
        }
        g_ctx1.buffer_id = new_buffer_id;
        g_ctx1.shmem_id = new_shmem_id;
        g_ctx1.stride_bytes = new_stride;
        g_ctx1.width = GFX_RESIZE_W;
        g_ctx1.height = GFX_RESIZE_H;
        g_ctx1.mapped_base = new_base;
    }
    damage_shmem_id = create_damage_rect_shmem(gfx_ep, GFX_RESIZE_W, GFX_RESIZE_H);
    if (damage_shmem_id < 0) {
        return GFX_SMOKE_E_DAMAGE;
    }

    puts("[test] gfx smoke visible start");
    for (uint32_t frame = 0; frame < GFX_FRAME_COUNT; ++frame) {
        if (fill_pattern(g_ctx1.mapped_base, GFX_RESIZE_W, GFX_RESIZE_H, g_ctx1.stride_bytes, frame + 2u) != 0) {
            puts("[test] gfx smoke paint-loop failed");
            return GFX_SMOKE_E_PAINT_LOOP;
        }
        if (flush_shared_buffer_ptr(g_ctx1.shmem_id, g_ctx1.mapped_base, g_ctx1.stride_bytes, GFX_RESIZE_H) != 0) {
            return GFX_SMOKE_E_PRESENT_LOOP;
        }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 g_ctx1.window_id, g_ctx1.buffer_id, 1, damage_shmem_id, &reply) != 0 ||
            reply.status != GFX_STATUS_OK) {
            puts("[test] gfx smoke present-loop failed");
            return GFX_SMOKE_E_PRESENT_LOOP;
        }
        (void)poll_gfx_events_once(gfx_ep, reply_ep, &req, 0);
        (void)wasmos_sched_yield();
    }
    puts("[test] gfx smoke visible done");

    /* --- win2: ui_init + paint + present --- */
    if (ui_init(&g_ctx2, proc_endpoint, reply_ep, GFX2_W, GFX2_H) != 0) {
        puts("[test] gfx smoke create2 failed");
        return GFX_SMOKE_E_CREATE;
    }
    (void)ui_window_set_title(&g_ctx2, "gfx smoke 2");

    if (fill_pattern(g_ctx2.mapped_base, GFX2_W, GFX2_H, g_ctx2.stride_bytes, 42u) != 0) {
        puts("[test] gfx smoke paint2 failed");
        return GFX_SMOKE_E_PAINT_LOOP;
    }
    if (flush_shared_buffer_ptr(g_ctx2.shmem_id, g_ctx2.mapped_base, g_ctx2.stride_bytes, GFX2_H) != 0) {
        return GFX_SMOKE_E_PRESENT_LOOP;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 g_ctx2.window_id, g_ctx2.buffer_id, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke present2 failed");
        return GFX_SMOKE_E_PRESENT_LOOP;
    }

    /* --- win3: ui_init + paint + present --- */
    if (ui_init(&g_ctx3, proc_endpoint, reply_ep, GFX3_W, GFX3_H) != 0) {
        puts("[test] gfx smoke create3 failed");
        return GFX_SMOKE_E_CREATE;
    }
    (void)ui_window_set_title(&g_ctx3, "gfx smoke 3");

    if (fill_wasmos_logo(g_ctx3.mapped_base, GFX3_W, GFX3_H, g_ctx3.stride_bytes) != 0) {
        puts("[test] gfx smoke paint3 failed");
        return GFX_SMOKE_E_PAINT_LOOP;
    }
    if (flush_shared_buffer_ptr(g_ctx3.shmem_id, g_ctx3.mapped_base, g_ctx3.stride_bytes, GFX3_H) != 0) {
        return GFX_SMOKE_E_PRESENT_LOOP;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 g_ctx3.window_id, g_ctx3.buffer_id, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke present3 failed");
        return GFX_SMOKE_E_PRESENT_LOOP;
    }

    if (start_libui_demo(proc_endpoint) != 0) {
        puts("[test] libui demo failed");
        return 33;
    }
    libui_started = 1;

    puts("[test] gfx smoke waiting close-request x3");
    int closed1 = 0;
    int closed2 = 0;
    int closed3 = 0;
    while (!closed1 || !closed2 || !closed3) {
        int32_t close_id = 0;
        gfx_reply_t ev = {0};
        if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_POLL_EVENT, 0, 0, 0, 0, &ev) != 0 ||
            ev.status != GFX_STATUS_OK) {
            return GFX_SMOKE_E_EVENT_CLOSE;
        }
        int rc = 0;
        if (ev.arg1 == GFX_EVENT_CLOSE_REQUEST) {
            close_id = ev.arg2;
            puts("[test] gfx smoke event close-request");
            rc = 1;
        } else if (ev.arg1 == GFX_EVENT_RESIZE) {
            int32_t rw = (int32_t)(ev.arg3 & 0xFFFF);
            int32_t rh = (int32_t)((ev.arg3 >> 16) & 0xFFFF);
            if (ev.arg2 == g_ctx1.window_id && !closed1) {
                (void)handle_resize_realloc(gfx_ep, reply_ep, &req, &g_ctx1, rw, rh, 90u);
            } else if (ev.arg2 == g_ctx2.window_id && !closed2) {
                (void)handle_resize_realloc(gfx_ep, reply_ep, &req, &g_ctx2, rw, rh, 120u);
            } else if (ev.arg2 == g_ctx3.window_id && !closed3) {
                (void)handle_resize_realloc_logo(gfx_ep, reply_ep, &req, &g_ctx3, rw, rh);
            }
        } else if (ev.arg1 == GFX_EVENT_POINTER) {
            (void)ev;
        } else if (ev.arg1 == GFX_EVENT_FOCUS_GAINED) {
#if GFX_SMOKE_TRACE
            puts("[test] gfx smoke event focus-gained");
#endif
        } else if (ev.arg1 == GFX_EVENT_FOCUS_LOST) {
#if GFX_SMOKE_TRACE
            puts("[test] gfx smoke event focus-lost");
#endif
        }
        if (rc == 1) {
            if (!closed1 && close_id == g_ctx1.window_id) {
                if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                             g_ctx1.window_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
                    puts("[test] gfx smoke destroy1 failed");
                    return GFX_SMOKE_E_DESTROY;
                }
                closed1 = 1;
                continue;
            }
            if (!closed2 && close_id == g_ctx2.window_id) {
                if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                             g_ctx2.window_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
                    puts("[test] gfx smoke destroy2 failed");
                    return GFX_SMOKE_E_DESTROY;
                }
                closed2 = 1;
                continue;
            }
            if (!closed3 && close_id == g_ctx3.window_id) {
                if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                             g_ctx3.window_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
                    puts("[test] gfx smoke destroy3 failed");
                    return GFX_SMOKE_E_DESTROY;
                }
                closed3 = 1;
                continue;
            }
        }
        if (libui_started && pump_libui_demo() != 0) {
            puts("[test] libui demo pump failed");
            return 33;
        }
        (void)wasmos_sched_yield();
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 g_ctx1.window_id, g_ctx1.buffer_id + 1, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke invalid-buffer deny failed");
        return GFX_SMOKE_E_INVALID_DENY;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RESIZE_WINDOW,
                 g_ctx1.window_id, 200, 120, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke post-destroy deny failed");
        return GFX_SMOKE_E_POST_DESTROY;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 g_ctx1.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release1 failed");
        return GFX_SMOKE_E_RELEASE1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 g_ctx2.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release2a failed");
        return GFX_SMOKE_E_RELEASE1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 g_ctx3.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release3a failed");
        return GFX_SMOKE_E_RELEASE1;
    }
    if (wasmos_shmem_unmap(g_ctx1.shmem_id) != 0 ||
        wasmos_shmem_unmap(g_ctx2.shmem_id) != 0 ||
        wasmos_shmem_unmap(g_ctx3.shmem_id) != 0) {
        puts("[test] gfx smoke shmem unmap failed");
        return GFX_SMOKE_E_UNMAP1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 g_ctx1.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release2 deny failed");
        return GFX_SMOKE_E_RELEASE2;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 g_ctx2.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release2b deny failed");
        return GFX_SMOKE_E_RELEASE2;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 g_ctx3.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release3b deny failed");
        return GFX_SMOKE_E_RELEASE2;
    }

    puts("[test] gfx smoke app ok");
    puts("[test] gfx smoke main done");
    if (libui_started) stop_libui_demo();
    return 0;
}
