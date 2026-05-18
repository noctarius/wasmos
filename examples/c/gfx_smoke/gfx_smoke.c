#include <stdint.h>
#include <string.h>
#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"

#define GFX_REQ_BASE 0x6A00
#define FBPP 4
#define BUFFER_PTR 0x4000
#define PAGE_SIZE 4096
#define GFX_W 64
#define GFX_H 64
#define GFX_RESIZE_W 80
#define GFX_RESIZE_H 48

typedef struct {
    int32_t status;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
} gfx_reply_t;

static int
fill_pattern(int32_t shmem_id, int32_t width, int32_t height, int32_t stride_bytes, uint32_t phase)
{
    int32_t byte_len = stride_bytes * height;
    int32_t map_len = (byte_len + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
    if (wasmos_shmem_map(shmem_id, BUFFER_PTR, map_len) != 0) {
        return -1;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)BUFFER_PTR;
    for (int32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)(void *)(base + (y * stride_bytes));
        for (int32_t x = 0; x < width; ++x) {
            uint32_t r = (uint32_t)((x + (int32_t)phase) & 0xFF);
            uint32_t g = (uint32_t)((y + (int32_t)(phase * 3u)) & 0xFF);
            uint32_t b = (uint32_t)((x + y + (int32_t)(phase * 5u)) & 0xFF);
            row[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return wasmos_shmem_unmap(shmem_id);
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

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int32_t proc_endpoint = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t gfx_ep = -1;
    int32_t req = GFX_REQ_BASE;
    gfx_reply_t reply;
    int32_t window_id;
    int32_t buffer_id;
    int32_t shmem_id;
    int32_t stride_bytes;

    if (proc_endpoint <= 0 || reply_ep < 0) {
        puts("[test] gfx smoke setup failed");
        return 1;
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
        return 1;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_CREATE_WINDOW,
                 GFX_W, GFX_H,
                 (int32_t)GFX_IPC_ABI_MAGIC,
                 (int32_t)gfx_ipc_header_pack(GFX_IPC_ABI_VERSION, GFX_IPC_CREATE_WINDOW),
                 &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke create failed");
        return 1;
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
        return 1;
    }
    buffer_id = reply.arg1;
    shmem_id = reply.arg2;
    stride_bytes = reply.arg3;

    if (fill_pattern(shmem_id, GFX_W, GFX_H, stride_bytes, 1) != 0) {
        puts("[test] gfx smoke paint1 failed");
        return 1;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id, buffer_id, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke present1 failed");
        return 1;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RESIZE_WINDOW,
                 window_id, GFX_RESIZE_W, GFX_RESIZE_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize failed");
        return 1;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_ALLOC_SHARED_BUFFER,
                 window_id, GFX_RESIZE_W, GFX_RESIZE_H, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke resize-alloc failed");
        return 1;
    }
    buffer_id = reply.arg1;
    shmem_id = reply.arg2;
    stride_bytes = reply.arg3;

    puts("[test] gfx smoke visible start");
    for (uint32_t frame = 0; frame < 240u; ++frame) {
        if (fill_pattern(shmem_id, GFX_RESIZE_W, GFX_RESIZE_H, stride_bytes, frame + 2u) != 0) {
            puts("[test] gfx smoke paint-loop failed");
            return 1;
        }
        if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                     window_id, buffer_id, 0, 0, &reply) != 0 ||
            reply.status != GFX_STATUS_OK) {
            puts("[test] gfx smoke present-loop failed");
            return 1;
        }
        (void)wasmos_sched_yield();
    }
    puts("[test] gfx smoke visible done");

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW,
                 window_id, buffer_id + 1, 0, 0, &reply) != 0 ||
        reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke invalid-buffer deny failed");
        return 1;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke release1 failed");
        return 1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RELEASE_SHARED_BUFFER,
                 buffer_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke release2 deny failed");
        return 1;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW,
                 window_id, 0, 0, 0, &reply) != 0 || reply.status != GFX_STATUS_OK) {
        puts("[test] gfx smoke destroy failed");
        return 1;
    }
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_RESIZE_WINDOW,
                 window_id, 200, 120, 0, &reply) != 0 || reply.status != GFX_STATUS_INVALID) {
        puts("[test] gfx smoke post-destroy deny failed");
        return 1;
    }

    puts("[test] gfx smoke app ok");
    return 0;
}
