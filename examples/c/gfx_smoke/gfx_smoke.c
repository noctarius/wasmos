#include <stdint.h>
#include <string.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"

#define GFX_REQ_BASE 0x6A00
#define FBPP 4
#define PAGE_SIZE 4096
#define GFX_W 64
#define GFX_H 64
#define GFX_RESIZE_W 320
#define GFX_RESIZE_H 180
#define GFX_FRAME_COUNT 8
#define GFX_HOLD_TICKS 180

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
    GFX_SMOKE_E_EVENT_FOCUS = 31
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
fill_pattern(uint8_t *base, int32_t width, int32_t height, int32_t stride_bytes, uint32_t phase)
{
    for (int32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)(void *)(base + (y * stride_bytes));
        for (int32_t x = 0; x < width; ++x) {
            uint32_t r = (uint32_t)((x + (int32_t)phase) & 0xFF);
            uint32_t g = (uint32_t)((y + (int32_t)(phase * 3u)) & 0xFF);
            uint32_t b = (uint32_t)((x + y + (int32_t)(phase * 5u)) & 0xFF);
            row[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
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
    int32_t shmem_id;
    int32_t damage_shmem_id;
    int32_t stride_bytes;
    uint8_t *mapped_base;

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
        if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                     window_id, buffer_id, 1, damage_shmem_id, &reply) != 0 ||
            reply.status != GFX_STATUS_OK) {
            puts("[test] gfx smoke present-loop failed");
            return GFX_SMOKE_E_PRESENT_LOOP;
        }
        (void)wasmos_sched_yield();
    }
    puts("[test] gfx smoke visible done");
    {
        int32_t start_ticks = wasmos_sched_ticks();
        while (start_ticks >= 0) {
            int32_t now = wasmos_sched_ticks();
            if (now < 0) {
                break;
            }
            if ((now - start_ticks) >= GFX_HOLD_TICKS) {
                break;
            }
            (void)wasmos_sched_yield();
        }
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id, buffer_id + 1, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke invalid-buffer deny failed");
        return GFX_SMOKE_E_INVALID_DENY;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release1 failed");
        return GFX_SMOKE_E_RELEASE1;
    }
    if (wasmos_shmem_unmap(shmem_id) != 0) {
        puts("[test] gfx smoke shmem unmap failed");
        return GFX_SMOKE_E_UNMAP1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release2 deny failed");
        return GFX_SMOKE_E_RELEASE2;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                 window_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke destroy failed");
        return GFX_SMOKE_E_DESTROY;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RESIZE_WINDOW,
                 window_id, 200, 120, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke post-destroy deny failed");
        return GFX_SMOKE_E_POST_DESTROY;
    }

    puts("[test] gfx smoke app ok");
    puts("[test] gfx smoke main done");
    return 0;
}
