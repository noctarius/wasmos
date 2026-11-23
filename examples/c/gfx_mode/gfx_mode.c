/* gfx_mode - test fixture: change the display mode through the compositor.
 *
 * Usage: gfx_mode <width> <height>
 *
 * Finds the compositor ("gfx") by name, asks it for <width>x<height> with
 * GFX_IPC_SET_DISPLAY_MODE, then asks GFX_IPC_GET_DISPLAY_INFO what the display
 * is now. Both requests are intents on the fixture's own event loop.
 * tests/test_ramfb_mode.py runs it.
 *
 * Prints "gfx-mode: set status=S WxH" with the compositor's packed status and
 * the geometry it reported, then "gfx-mode: display WxH", or
 * "gfx-mode: failed status=S" when the compositor cannot be reached. */
#include <stdint.h>
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "wasmos/api.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos/gfx_ipc.h"

#define GFX_FIND_MS 10000

static wasmos_sys_event_loop_t g_loop;
static wasmos_sys_svc_watch_set_t g_watches;
static wasmos_sys_svc_watch_t g_gfx_watch;
static uint32_t g_width;
static uint32_t g_height;
static int g_done;

static void on_display_info(void* user, const wasmos_ipc_message_t* msg) {
    (void)user;
    printf("gfx-mode: display %ux%u\n", (unsigned)msg->arg1, (unsigned)msg->arg2);
    g_done = 1;
}

static void on_set_mode(void* user, const wasmos_ipc_message_t* msg) {
    int32_t gfx_ep = (int32_t)(intptr_t)user;
    printf("gfx-mode: set status=%d %ux%u\n",
           (int)msg->arg0,
           (unsigned)msg->arg1,
           (unsigned)msg->arg2);
    if (wasmos_sys_intent_send(&g_loop,
                               gfx_ep,
                               g_loop.receiver_endpoint,
                               GFX_IPC_GET_DISPLAY_INFO,
                               0,
                               0,
                               0,
                               0,
                               on_display_info,
                               0,
                               0) != 0) {
        g_done = 1;
    }
}

static void on_gfx_bound(void* user, wasmos_sys_svc_watch_t* watch, int32_t endpoint) {
    (void)user;
    wasmos_sys_svc_watch_stop(watch);
    int32_t rc = wasmos_sys_intent_send(&g_loop,
                                        endpoint,
                                        g_loop.receiver_endpoint,
                                        GFX_IPC_SET_DISPLAY_MODE,
                                        (int32_t)g_width,
                                        (int32_t)g_height,
                                        0,
                                        0,
                                        on_set_mode,
                                        (void*)(intptr_t)endpoint,
                                        0);
    if (rc != 0) {
        printf("gfx-mode: failed status=%d\n", (int)rc);
        g_done = 1;
    }
}

static void on_gfx_missing(void* user, wasmos_sys_svc_watch_t* watch, int32_t status) {
    (void)user;
    (void)watch;
    printf("gfx-mode: failed status=%d\n", (int)status);
    g_done = 1;
}

int main(void) {
    char args[32];
    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    char* h = strchr(args, ' ');
    if (!h) {
        puts("gfx-mode: usage: gfx_mode <width> <height>");
        return 2;
    }
    *h++ = '\0';
    g_width = (uint32_t)atoi(args);
    g_height = (uint32_t)atoi(h);

    int32_t ep = wasmos_ipc_create_endpoint();
    if (ep < 0) {
        printf("gfx-mode: failed status=%d\n", (int)ep);
        return 1;
    }
    wasmos_sys_event_loop_init(&g_loop, ep, 0x100);
    g_loop.poll_timeout_ms = 100;
    if (wasmos_sys_svc_watch_set_init(&g_watches, &g_loop, wasmos_startup_proc_endpoint()) != 0 ||
        wasmos_sys_svc_watch_start(
            &g_watches, &g_gfx_watch, "gfx", GFX_FIND_MS, on_gfx_bound, on_gfx_missing, 0) != 0) {
        puts("gfx-mode: failed status=watch");
        return 1;
    }
    while (!g_done) {
        (void)wasmos_sys_event_loop_poll(&g_loop, 8);
        wasmos_sys_svc_watch_set_expire(&g_watches);
    }
    return 0;
}
