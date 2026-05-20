#include <stdint.h>
#include <string.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmo_mascot_rgba.h"

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

typedef struct {
    int32_t window_id;
    int32_t buffer_id;
    int32_t shmem_id;
    int32_t stride_bytes;
    int32_t width;
    int32_t height;
    uint8_t *mapped_base;
} gfx_window_rt_t;

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
        puts("[test] gfx smoke event focus-gained");
    } else if (ev.arg1 == GFX_EVENT_FOCUS_LOST) {
        puts("[test] gfx smoke event focus-lost");
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
handle_resize_realloc(int32_t gfx_ep, int32_t reply_ep, int32_t *req, gfx_window_rt_t *win, int32_t new_w, int32_t new_h, uint32_t phase)
{
    gfx_reply_t reply;
    if (new_w <= 0 || new_h <= 0) {
        return -1;
    }
    if (send_gfx(gfx_ep, reply_ep, (*req)++, GFX_IPC_ALLOC_SHARED_BUFFER,
                 win->window_id, new_w, new_h, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize alloc failed");
        return -1;
    }
    int32_t new_buffer_id = reply.arg1;
    int32_t new_shmem_id = reply.arg2;
    int32_t new_stride = reply.arg3;
    uint8_t *new_base = 0;
    if (map_shared_buffer_ptr(new_shmem_id, new_stride, new_h, &new_base) != 0) {
        return -1;
    }
    if (fill_pattern(new_base, new_w, new_h, new_stride, phase) != 0) {
        puts("[test] gfx smoke resize paint failed");
        return -1;
    }
    if (flush_shared_buffer_ptr(new_shmem_id, new_base, new_stride, new_h) != 0) {
        return -1;
    }
    if (send_gfx(gfx_ep, reply_ep, (*req)++, GFX_IPC_PRESENT_WINDOW,
                 win->window_id, new_buffer_id, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize present failed");
        return -1;
    }

    if (send_gfx(gfx_ep, reply_ep, (*req)++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 win->buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize release old failed");
        return -1;
    }
    if (wasmos_shmem_unmap(win->shmem_id) != 0) {
        puts("[test] gfx smoke resize unmap old failed");
        return -1;
    }

    win->buffer_id = new_buffer_id;
    win->shmem_id = new_shmem_id;
    win->stride_bytes = new_stride;
    win->width = new_w;
    win->height = new_h;
    win->mapped_base = new_base;
    return 0;
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
    int32_t window_id;
    int32_t buffer_id;
    int32_t buffer_id2 = 0;
    int32_t buffer_id3 = 0;
    int32_t shmem_id;
    int32_t shmem_id2 = 0;
    int32_t shmem_id3 = 0;
    int32_t damage_shmem_id;
    int32_t window_id2 = 0;
    int32_t window_id3 = 0;
    int32_t stride_bytes2 = 0;
    int32_t stride_bytes3 = 0;
    int32_t stride_bytes;
    uint8_t *mapped_base;
    uint8_t *mapped_base2 = 0;
    uint8_t *mapped_base3 = 0;
    gfx_window_rt_t win1 = {0};
    gfx_window_rt_t win2 = {0};
    gfx_window_rt_t win3 = {0};

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

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_CREATE_WINDOW,
                 GFX_W, GFX_H,
                 (int32_t)GFX_IPC_ABI_MAGIC,
                 (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                 &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke create failed");
        return GFX_SMOKE_E_CREATE;
    }
    window_id = reply.arg1;

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_ALLOC_SHARED_BUFFER,
                 window_id, GFX_W, GFX_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "[test] gfx smoke alloc failed status=%d a1=%d a2=%d a3=%d\n",
                         reply.status, reply.arg1, reply.arg2, reply.arg3);
        if (n > 0) {
            (void)putsn(buf, (size_t)n);
        }
        return GFX_SMOKE_E_ALLOC0;
    }
    buffer_id = reply.arg1;
    shmem_id = reply.arg2;
    stride_bytes = reply.arg3;

    if (map_shared_buffer_ptr(shmem_id, stride_bytes, GFX_H, &mapped_base) != 0) {
        return GFX_SMOKE_E_MAP0;
    }
    if (fill_pattern(mapped_base, GFX_W, GFX_H, stride_bytes, 1) != 0) {
        puts("[test] gfx smoke paint1 failed");
        return GFX_SMOKE_E_PAINT0;
    }
    if (flush_shared_buffer_ptr(shmem_id, mapped_base, stride_bytes, GFX_H) != 0) {
        return GFX_SMOKE_E_PAINT0;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id, buffer_id, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke present1 failed");
        return GFX_SMOKE_E_PRESENT0;
    }
    if (poll_gfx_focus_event(gfx_ep, reply_ep, &req, window_id) != 0) {
        puts("[test] gfx smoke event focus missing");
        return GFX_SMOKE_E_EVENT_FOCUS;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RESIZE_WINDOW,
                 window_id, GFX_RESIZE_W, GFX_RESIZE_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize failed");
        return GFX_SMOKE_E_RESIZE;
    }
    if (wasmos_shmem_unmap(shmem_id) != 0) {
        puts("[test] gfx smoke shmem unmap failed");
        return GFX_SMOKE_E_UNMAP0;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_ALLOC_SHARED_BUFFER,
                 window_id, GFX_RESIZE_W, GFX_RESIZE_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize-alloc failed");
        return GFX_SMOKE_E_ALLOC1;
    }
    buffer_id = reply.arg1;
    shmem_id = reply.arg2;
    stride_bytes = reply.arg3;
    if (map_shared_buffer_ptr(shmem_id, stride_bytes, GFX_RESIZE_H, &mapped_base) != 0) {
        return GFX_SMOKE_E_MAP1;
    }
    win1.window_id = window_id;
    win1.buffer_id = buffer_id;
    win1.shmem_id = shmem_id;
    win1.stride_bytes = stride_bytes;
    win1.width = GFX_RESIZE_W;
    win1.height = GFX_RESIZE_H;
    win1.mapped_base = mapped_base;
    damage_shmem_id = create_damage_rect_shmem(gfx_ep, GFX_RESIZE_W, GFX_RESIZE_H);
    if (damage_shmem_id < 0) {
        return GFX_SMOKE_E_DAMAGE;
    }

    puts("[test] gfx smoke visible start");
    for (uint32_t frame = 0; frame < GFX_FRAME_COUNT; ++frame) {
        if (fill_pattern(mapped_base, GFX_RESIZE_W, GFX_RESIZE_H, stride_bytes, frame + 2u) != 0) {
            puts("[test] gfx smoke paint-loop failed");
            return GFX_SMOKE_E_PAINT_LOOP;
        }
        if (flush_shared_buffer_ptr(shmem_id, mapped_base, stride_bytes, GFX_RESIZE_H) != 0) {
            return GFX_SMOKE_E_PRESENT_LOOP;
        }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id, buffer_id, 1, damage_shmem_id, &reply) != 0 ||
            reply.status != GFX_STATUS_OK) {
            puts("[test] gfx smoke present-loop failed");
            return GFX_SMOKE_E_PRESENT_LOOP;
        }
        (void)poll_gfx_events_once(gfx_ep, reply_ep, &req, 0);
        (void)wasmos_sched_yield();
    }
    puts("[test] gfx smoke visible done");

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_CREATE_WINDOW,
                 GFX2_W, GFX2_H,
                 (int32_t)GFX_IPC_ABI_MAGIC,
                 (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                 &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke create2 failed");
        return GFX_SMOKE_E_CREATE;
    }
    window_id2 = reply.arg1;

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_ALLOC_SHARED_BUFFER,
                 window_id2, GFX2_W, GFX2_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke alloc2 failed");
        return GFX_SMOKE_E_ALLOC1;
    }
    buffer_id2 = reply.arg1;
    shmem_id2 = reply.arg2;
    stride_bytes2 = reply.arg3;
    if (map_shared_buffer_ptr(shmem_id2, stride_bytes2, GFX2_H, &mapped_base2) != 0) {
        return GFX_SMOKE_E_MAP1;
    }
    if (fill_pattern(mapped_base2, GFX2_W, GFX2_H, stride_bytes2, 42u) != 0) {
        puts("[test] gfx smoke paint2 failed");
        return GFX_SMOKE_E_PAINT_LOOP;
    }
    if (flush_shared_buffer_ptr(shmem_id2, mapped_base2, stride_bytes2, GFX2_H) != 0) {
        return GFX_SMOKE_E_PRESENT_LOOP;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id2, buffer_id2, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke present2 failed");
        return GFX_SMOKE_E_PRESENT_LOOP;
    }
    win2.window_id = window_id2;
    win2.buffer_id = buffer_id2;
    win2.shmem_id = shmem_id2;
    win2.stride_bytes = stride_bytes2;
    win2.width = GFX2_W;
    win2.height = GFX2_H;
    win2.mapped_base = mapped_base2;

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_CREATE_WINDOW,
                 GFX3_W, GFX3_H,
                 (int32_t)GFX_IPC_ABI_MAGIC,
                 (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                 &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke create3 failed");
        return GFX_SMOKE_E_CREATE;
    }
    window_id3 = reply.arg1;

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_ALLOC_SHARED_BUFFER,
                 window_id3, GFX3_W, GFX3_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke alloc3 failed");
        return GFX_SMOKE_E_ALLOC1;
    }
    buffer_id3 = reply.arg1;
    shmem_id3 = reply.arg2;
    stride_bytes3 = reply.arg3;
    if (map_shared_buffer_ptr(shmem_id3, stride_bytes3, GFX3_H, &mapped_base3) != 0) {
        return GFX_SMOKE_E_MAP1;
    }
    if (fill_wasmos_logo(mapped_base3, GFX3_W, GFX3_H, stride_bytes3) != 0) {
        puts("[test] gfx smoke paint3 failed");
        return GFX_SMOKE_E_PAINT_LOOP;
    }
    if (flush_shared_buffer_ptr(shmem_id3, mapped_base3, stride_bytes3, GFX3_H) != 0) {
        return GFX_SMOKE_E_PRESENT_LOOP;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id3, buffer_id3, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke present3 failed");
        return GFX_SMOKE_E_PRESENT_LOOP;
    }
    win3.window_id = window_id3;
    win3.buffer_id = buffer_id3;
    win3.shmem_id = shmem_id3;
    win3.stride_bytes = stride_bytes3;
    win3.width = GFX3_W;
    win3.height = GFX3_H;
    win3.mapped_base = mapped_base3;

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
            if (ev.arg2 == win1.window_id && !closed1) {
                if (handle_resize_realloc(gfx_ep, reply_ep, &req, &win1, rw, rh, 90u) != 0) {
                    return GFX_SMOKE_E_RESIZE;
                }
            } else if (ev.arg2 == win2.window_id && !closed2) {
                if (handle_resize_realloc(gfx_ep, reply_ep, &req, &win2, rw, rh, 120u) != 0) {
                    return GFX_SMOKE_E_RESIZE;
                }
            }
        } else if (ev.arg1 == GFX_EVENT_POINTER) {
            (void)ev;
        } else if (ev.arg1 == GFX_EVENT_FOCUS_GAINED) {
            puts("[test] gfx smoke event focus-gained");
        } else if (ev.arg1 == GFX_EVENT_FOCUS_LOST) {
            puts("[test] gfx smoke event focus-lost");
        }
        if (rc == 1) {
            if (!closed1 && close_id == window_id) {
                if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                             window_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
                    puts("[test] gfx smoke destroy1 failed");
                    return GFX_SMOKE_E_DESTROY;
                }
                closed1 = 1;
                continue;
            }
            if (!closed2 && close_id == window_id2) {
                if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                             window_id2, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
                    puts("[test] gfx smoke destroy2 failed");
                    return GFX_SMOKE_E_DESTROY;
                }
                closed2 = 1;
                continue;
            }
            if (!closed3 && close_id == window_id3) {
                if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                             window_id3, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
                    puts("[test] gfx smoke destroy3 failed");
                    return GFX_SMOKE_E_DESTROY;
                }
                closed3 = 1;
                continue;
            }
        }
        (void)wasmos_sched_yield();
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id, win1.buffer_id + 1, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke invalid-buffer deny failed");
        return GFX_SMOKE_E_INVALID_DENY;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RESIZE_WINDOW,
                 window_id, 200, 120, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke post-destroy deny failed");
        return GFX_SMOKE_E_POST_DESTROY;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 win1.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release1 failed");
        return GFX_SMOKE_E_RELEASE1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 win2.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release2a failed");
        return GFX_SMOKE_E_RELEASE1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 win3.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release3a failed");
        return GFX_SMOKE_E_RELEASE1;
    }
    if (wasmos_shmem_unmap(win1.shmem_id) != 0 ||
        wasmos_shmem_unmap(win2.shmem_id) != 0 ||
        wasmos_shmem_unmap(win3.shmem_id) != 0) {
        puts("[test] gfx smoke shmem unmap failed");
        return GFX_SMOKE_E_UNMAP1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 win1.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release2 deny failed");
        return GFX_SMOKE_E_RELEASE2;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 win2.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release2b deny failed");
        return GFX_SMOKE_E_RELEASE2;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 win3.buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release3b deny failed");
        return GFX_SMOKE_E_RELEASE2;
    }

    puts("[test] gfx smoke app ok");
    puts("[test] gfx smoke main done");
    return 0;
}
