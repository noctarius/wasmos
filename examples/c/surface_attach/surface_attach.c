/* surface_attach - drive a window surface the CLIENT owns end to end.
 *
 * This is the first client of the surface protocol that replaces
 * GFX_IPC_ALLOC_SHARED_BUFFER, in which the compositor allocated a buffer and
 * granted it to the app. A server cannot own a buffer it hands to a client:
 * xfer_buffer_release is owner-only, no hostcall transfers ownership, and
 * nothing tells a server when a client stopped reading
 * (docs/architecture/12-dma-transfers.md). So the app owns its surface and
 * lends it, and the three opcodes here are the handshake that makes that
 * expressible:
 *
 *   GFX_IPC_GET_SURFACE_SPEC  what must this surface look like? (allocates
 *                             nothing: stride, byte size, extent)
 *   GFX_IPC_ATTACH_SURFACE    here is a buffer I own and have borrowed to you
 *   GFX_IPC_DETACH_SURFACE    stop reading it; I am about to release it
 *
 * The detach is not symmetry for its own sake. There is no unborrow
 * notification, so releasing while the compositor still holds the borrow leaves
 * it reading a revoked borrow mid-composite, and the mapping it holds comes out
 * of a 32-slot pool shared by every native service.
 *
 * Each stage prints its own marker so a failing log says which one broke rather
 * than only that the app exited non-zero.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

#define WIN_W 64
#define WIN_H 32

/* Distinct and non-zero, so a surface the compositor never read back reads as
 * zeros rather than as a plausible colour. */
#define FILL_PIXEL 0xFF3366CCu

typedef struct {
    int32_t status;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
} gfx_reply_t;

static int send_gfx(int32_t gfx_ep, int32_t reply_ep, int32_t req_id, int32_t opcode, int32_t arg0,
                    int32_t arg1, int32_t arg2, int32_t arg3, gfx_reply_t* out) {
    wasmos_ipc_message_t resp;
    if (wasmos_ipc_call(gfx_ep, reply_ep, opcode, req_id, arg0, arg1, arg2, arg3, &resp) != 0) {
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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const int32_t proc_endpoint = wasmos_startup_arg(0);
    const int32_t reply_ep = wasmos_ipc_create_endpoint();
    if (proc_endpoint <= 0 || reply_ep < 0) {
        putsn("[test] surface attach no endpoints\n", 35);
        return 1;
    }
    int32_t req = 1;
    int32_t gfx_ep = 0;
    for (int32_t tries = 0; tries < 200 && gfx_ep <= 0; ++tries) {
        gfx_ep = wasmos_svc_lookup(proc_endpoint, reply_ep, "gfx", req++);
        if (gfx_ep <= 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (gfx_ep <= 0) {
        putsn("[test] surface attach no gfx\n", 29);
        return 1;
    }

    gfx_reply_t r;
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_CREATE_WINDOW, WIN_W, WIN_H, 0, 0, &r) != 0 ||
        r.status != WASMOS_ERR_NONE || r.arg1 <= 0) {
        printf("[test] surface attach create failed status=%d\n", (int)r.status);
        return 1;
    }
    const int32_t window_id = r.arg1;

    /* The spec is read, not assumed: stride is the compositor's to choose, and
     * a resize bumps the window generation so a cached spec goes stale. */
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_GET_SURFACE_SPEC, window_id, 0, 0, 0, &r) != 0 ||
        r.status != WASMOS_ERR_NONE) {
        printf("[test] surface attach spec failed status=%d\n", (int)r.status);
        return 1;
    }
    const int32_t stride = r.arg1;
    const int32_t byte_size = r.arg2;
    const int32_t extent = r.arg3;
    if (stride < WIN_W * 4 || byte_size < stride * WIN_H || extent != ((WIN_W << 16) | WIN_H)) {
        printf("[test] surface attach spec bogus stride=%d size=%d extent=%x\n",
               (int)stride,
               (int)byte_size,
               (unsigned)extent);
        return 1;
    }
    printf("[test] surface attach spec ok stride=%d size=%d\n", (int)stride, (int)byte_size);

    const int32_t bid = wasmos_xfer_buffer_acquire(byte_size);
    if (bid <= 0) {
        printf("[test] surface attach acquire failed rc=%d\n", (int)bid);
        return 1;
    }
    /* READ is all the compositor needs: it composites out of this surface and
     * never writes into it. */
    const int32_t borrow = wasmos_xfer_buffer_borrow(gfx_ep, bid, WASMOS_BUFFER_GRANT_READ);
    if (borrow <= 0) {
        printf("[test] surface attach borrow failed rc=%d\n", (int)borrow);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    const int32_t off = wasmos_xfer_buffer_map(bid);
    if (off < 0) {
        printf("[test] surface attach map failed rc=%d\n", (int)off);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    uint32_t* pixels = addr_cast(uint32_t*, off);
    for (int32_t y = 0; y < WIN_H; ++y) {
        uint32_t* row = addr_cast(uint32_t*, off + y * stride);
        for (int32_t x = 0; x < WIN_W; ++x) {
            row[x] = FILL_PIXEL;
        }
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_ATTACH_SURFACE, window_id, bid, borrow, 0, &r) !=
            0 ||
        r.status != WASMOS_ERR_NONE) {
        printf("[test] surface attach attach failed status=%d\n", (int)r.status);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    printf("[test] surface attach attach ok stride=%d\n", (int)r.arg2);

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_PRESENT_WINDOW, window_id, bid, 0, 0, &r) != 0 ||
        r.status != WASMOS_ERR_NONE) {
        printf("[test] surface attach present failed status=%d\n", (int)r.status);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    putsn("[test] surface attach present ok\n", 33);

    /* Detaching a surface that is still a window's current buffer is refused,
     * exactly as releasing one was: the compositor is mid-composite out of it.
     * Destroying the window is what frees it here. */
    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DETACH_SURFACE, window_id, bid, 0, 0, &r) != 0 ||
        r.status != WASMOS_ERR_GFX_BUSY) {
        printf("[test] surface attach detach-while-presented NOT refused status=%d\n",
               (int)r.status);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    putsn("[test] surface attach busy deny ok\n", 35);

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DESTROY_WINDOW, window_id, 0, 0, 0, &r) != 0 ||
        r.status != WASMOS_ERR_NONE) {
        printf("[test] surface attach destroy failed status=%d\n", (int)r.status);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }

    if (send_gfx(gfx_ep, reply_ep, req++, GFX_IPC_DETACH_SURFACE, window_id, bid, 0, 0, &r) != 0 ||
        r.status != WASMOS_ERR_NONE) {
        printf("[test] surface attach detach failed status=%d\n", (int)r.status);
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    putsn("[test] surface attach detach ok\n", 32);

    /* Only now is releasing safe: the compositor has acknowledged that it
     * stopped reading and dropped its mapping. */
    (void)wasmos_xfer_buffer_unmap(bid);
    (void)wasmos_xfer_buffer_release(bid);
    (void)pixels;

    /* Regression: 2026-09-02-overlay-unmap-leaves-uncommitted-page
     *
     * A SECOND overlay after the first was unmapped. Mapping one over
     * slot-backed linear memory frees that page's own frame and installs the
     * shared one; the unmap above has to put a frame back, or the page stays
     * inside what linear memory counts as COMMITTED with nothing behind it. The
     * kernel then refuses to publish the ring-3 window for the whole allocation,
     * so THIS map fails -- and so does every later one, in any process that
     * resizes a window (libui's ui_realloc_buffer unmaps on every resize).
     *
     * The write is part of the case, not decoration: a mapping that returns an
     * offset the guest cannot store through is not a mapping. */
    const int32_t bid2 = wasmos_xfer_buffer_acquire(byte_size);
    if (bid2 <= 0) {
        printf("[test] surface attach remap acquire failed rc=%d\n", (int)bid2);
        return 1;
    }
    const int32_t off2 = wasmos_xfer_buffer_map(bid2);
    if (off2 < 0) {
        printf("[test] surface attach remap failed rc=%d\n", (int)off2);
        (void)wasmos_xfer_buffer_release(bid2);
        return 1;
    }
    uint32_t* remapped = addr_cast(uint32_t*, off2);
    remapped[0] = FILL_PIXEL;
    if (remapped[0] != FILL_PIXEL) {
        putsn("[test] surface attach remap not writable\n", 41);
        (void)wasmos_xfer_buffer_unmap(bid2);
        (void)wasmos_xfer_buffer_release(bid2);
        return 1;
    }
    (void)wasmos_xfer_buffer_unmap(bid2);
    (void)wasmos_xfer_buffer_release(bid2);
    putsn("[test] surface attach remap ok\n", 31);

    putsn("[test] surface attach done\n", 27);
    return 0;
}
