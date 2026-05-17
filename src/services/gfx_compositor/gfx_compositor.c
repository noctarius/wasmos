#include <stdint.h>
#include <stdio.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/gfx_ipc.h"
#include "wasmos_driver_abi.h"

#define GFX_REQUEST_BASE 0x7000
#define GFX_FB_LOOKUP_RETRIES 2048

static int32_t g_proc_endpoint = -1;
static int32_t g_gfx_endpoint = -1;
static int32_t g_fb_endpoint = -1;

static int
lookup_fb_endpoint(void)
{
    for (int32_t i = 0; i < GFX_FB_LOOKUP_RETRIES; ++i) {
        int32_t ep = wasmos_svc_lookup(g_proc_endpoint, g_gfx_endpoint, "fb", GFX_REQUEST_BASE + i);
        if (ep >= 0) {
            g_fb_endpoint = ep;
            return 0;
        }
        (void)wasmos_sched_yield();
    }
    return -1;
}

static void
log_fb_geometry_probe(void)
{
    if (g_fb_endpoint < 0) {
        return;
    }

    wasmos_ipc_message_t reply;
    int32_t req_id = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 1;
    if (wasmos_ipc_call(g_fb_endpoint,
                        g_gfx_endpoint,
                        FBTEXT_IPC_GEOMETRY_REQ,
                        req_id,
                        0,
                        0,
                        0,
                        0,
                        &reply) != 0) {
        printf("[gfx] fb geometry probe failed\n");
        return;
    }

    if (reply.type == FBTEXT_IPC_RESP) {
        printf("[gfx] fb geometry cols=%d rows=%d\n", reply.arg0, reply.arg1);
    }
}

static int
reply_unsupported(const wasmos_ipc_message_t *msg)
{
    if (!msg || msg->source < 0 || msg->request_id == 0) {
        return -1;
    }
    return wasmos_ipc_send(msg->source,
                           g_gfx_endpoint,
                           GFX_IPC_RESP,
                           msg->request_id,
                           GFX_STATUS_UNSUPPORTED,
                           0,
                           0,
                           0);
}

WASMOS_WASM_EXPORT int32_t
initialize(int32_t proc_endpoint,
           int32_t ignored_arg1,
           int32_t ignored_arg2,
           int32_t ignored_arg3)
{
    (void)ignored_arg1;
    (void)ignored_arg2;
    (void)ignored_arg3;

    if (proc_endpoint < 0) {
        return -1;
    }
    g_proc_endpoint = proc_endpoint;
    g_gfx_endpoint = wasmos_ipc_create_endpoint();
    if (g_gfx_endpoint < 0) {
        return -1;
    }

    if (wasmos_svc_register(g_proc_endpoint, g_gfx_endpoint, "gfx", 1) != 0) {
        printf("[gfx] register failed\n");
        return -1;
    }

    if (lookup_fb_endpoint() != 0) {
        /* TODO(graphics): switch to explicit fb-display IPC ABI once the
         * framebuffer driver gains Phase 1 compositor-facing messages. */
        printf("[gfx] fb endpoint unavailable\n");
    } else {
        log_fb_geometry_probe();
        printf("[test] gfx compositor handshake ok\n");
    }

    for (;;) {
        if (wasmos_ipc_recv(g_gfx_endpoint) < 0) {
            return -1;
        }

        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);
        if (!gfx_ipc_header_valid((uint32_t)msg.arg2, (uint32_t)msg.arg3)) {
            (void)reply_unsupported(&msg);
            continue;
        }

        /* FIXME(phase-1): add per-window ownership tracking and strict command
         * buffer validation before handling submit/present operations. */
        (void)reply_unsupported(&msg);
    }
}
