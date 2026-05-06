#include "wasmos_native_driver.h"
#include "../include/wasmos_driver_abi.h"
#include "fbtext_internal.h"

/*
 * Native framebuffer driver — Phase 1 virtual terminal.
 *
 * Extends the original gradient demo into a text-rendering server:
 *   1. Probe and map the physical framebuffer.
 *   2. Initialise the cell grid and font.
 *   3. Replay the kernel early-log ring buffer so the full boot log appears.
 *   4. Drain the shared serial console ring and serve FBTEXT control IPC.
 *
 * The internal rendering code lives in render.c; this file owns the driver
 * lifecycle, early-log replay, and the IPC server loop.
 *
 * IPC message encoding:
 *   FBTEXT_IPC_CELL_WRITE_REQ  arg0=col  arg1=row  arg2=codepoint  arg3=(fg<<8)|bg
 *   FBTEXT_IPC_CURSOR_SET_REQ  arg0=col  arg1=row
 *   FBTEXT_IPC_SCROLL_REQ      arg0=n_rows
 *   FBTEXT_IPC_CLEAR_REQ       (no args)
 *   FBTEXT_IPC_GEOMETRY_REQ    resp: arg0=cols arg1=rows
 */

/* IPC_EMPTY and IPC_OK values — must stay in sync with kernel ipc.h. */
#define ND_IPC_OK    0
#define ND_IPC_EMPTY 1

#define EARLY_LOG_BUF 4096

static fbtext_state_t g_state;
static uint8_t        g_early_log_buf[EARLY_LOG_BUF];
static uint8_t        g_console_ring_enabled = 1;

static int
str_len(const char *s)
{
    int n = 0;
    while (s[n]) { ++n; }
    return n;
}

static void
write_str(wasmos_driver_api_t *api, const char *s)
{
    api->console_write(s, str_len(s));
}

static void
write_hex32(wasmos_driver_api_t *api, const char *label, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[64];
    int i = 0;
    const char *p = label;
    while (*p) buf[i++] = *p++;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4) buf[i++] = hx[(v >> s) & 0xF];
    buf[i] = '\0';
    api->console_write(buf, i);
}

static void
replay_early_log(wasmos_driver_api_t *api)
{
    uint32_t total = api->early_log_size();
    if (total == 0) {
        return;
    }
    if (total > EARLY_LOG_BUF) {
        uint32_t skip = total - EARLY_LOG_BUF;
        api->early_log_copy(g_early_log_buf, skip, EARLY_LOG_BUF);
        total = EARLY_LOG_BUF;
    } else {
        api->early_log_copy(g_early_log_buf, 0, total);
    }

    /* Find the start offset such that only the last (rows-1) newlines worth
     * of content is replayed — fits on screen without any scrolling. */
    uint16_t max_rows = g_state.rows > 1 ? (uint16_t)(g_state.rows - 1) : 1;
    uint16_t nl_count = 0;
    uint32_t start    = total;        /* exclusive upper bound, scan left */
    while (start > 0) {
        start--;
        if (g_early_log_buf[start] == '\n') {
            nl_count++;
            if (nl_count >= max_rows) {
                start++;            /* replay from byte after this newline */
                break;
            }
        }
    }

    for (uint32_t i = start; i < total; i++) {
        fbtext_put_char(&g_state, (uint32_t)g_early_log_buf[i]);
    }
}

static int
drain_console_ring(console_ring_t *ring, uint32_t budget)
{
    if (!ring || ring->capacity == 0) {
        return 0;
    }
    uint32_t cap = ring->capacity;
    uint32_t rp = ring->read_pos;
    uint32_t wp = ring->write_pos;
    int drained = 0;
    uint32_t n = 0;
    while (rp != wp && n < budget) {
        fbtext_put_char(&g_state, ring->data[rp % cap]);
        rp++;
        n++;
        drained = 1;
    }
    ring->read_pos = rp;
    return drained;
}

int
initialize(wasmos_driver_api_t *api, int module_count, int arg2, int arg3)
{
    (void)module_count;
    (void)arg2;
    (void)arg3;

    write_str(api, "[framebuffer] probing\n");

    nd_framebuffer_info_t info;
    if (api->framebuffer_info(&info) != 0 ||
        info.framebuffer_base == 0 ||
        info.framebuffer_width == 0 ||
        info.framebuffer_height == 0 ||
        info.framebuffer_stride == 0) {
        write_str(api, "[framebuffer] not present\n");
        return 0;
    }

    /* Start from the boot-provided framebuffer geometry. The kernel only maps
     * the physical framebuffer range captured by the bootloader, so native
     * driver-side VBE mode changes must not expand the required map size.
     * TODO: add an explicit mode-set/update-framebuffer-info path before
     * allowing this driver to reprogram Bochs VBE after ExitBootServices().
     */
    uint32_t fb_width  = info.framebuffer_width;
    uint32_t fb_height = info.framebuffer_height;
    uint32_t fb_stride = info.framebuffer_stride;
    write_hex32(api, "[framebuffer] boot mode ", fb_width);
    write_hex32(api, "x", fb_height);
    write_hex32(api, " stride=", fb_stride);
    write_str(api, "\n");

    /* Compute map size from boot geometry (32bpp = 4 bytes/pixel). */
    uint32_t size = fb_stride * fb_height * 4u;
    size = (size + 0xFFFu) & ~0xFFFu;

    write_str(api, "[framebuffer] mapping\n");
    void *fb = api->framebuffer_map(size);
    if (!fb) {
        write_str(api, "[framebuffer] map failed\n");
        return -1;
    }

    /* Initialise the cell grid with the corrected geometry. */
    fbtext_render_init(&g_state, (uint32_t *)fb, fb_stride, fb_width, fb_height);
    fbtext_clear(&g_state);

    replay_early_log(api);

    write_str(api, "[framebuffer] ready\n");

    /* Create the IPC endpoint for cell-write requests. */
    uint32_t ep = api->ipc_create_endpoint();
    if (ep == (uint32_t)~0u) {
        write_str(api, "[framebuffer] ipc endpoint failed\n");
        return -1;
    }
    /* Derive our context id from current pid (context_id == pid for native). */
    uint32_t ctx = api->sched_current_pid();
    if (api->console_register_fb(ctx, ep) != 0) {
        write_str(api, "[framebuffer] register endpoint failed\n");
        return -1;
    }

    console_ring_t *ring = (console_ring_t *)api->shmem_map(api->console_ring_id());
    if (!ring) {
        write_str(api, "[framebuffer] console ring map failed\n");
        return -1;
    }

    /* Main loop: prioritize control IPC so tty switch clear/replay requests
     * are applied promptly even if console ring backlog is large. */
    nd_ipc_message_t msg;
    for (;;) {
        int rc = api->ipc_recv(ctx, ep, &msg);
        if (rc == ND_IPC_EMPTY) {
            /* Drain ring output in bounded chunks so control IPC gets regular
             * service windows even under sustained serial log throughput. */
            int had_ring = g_console_ring_enabled
                               ? drain_console_ring(ring, 256u)
                               : 0;
            if (!had_ring) {
                api->sched_yield();
            }
            continue;
        }
        if (rc != ND_IPC_OK) {
            /* Endpoint error — log and exit. */
            write_str(api, "[framebuffer] ipc error\n");
            break;
        }

        nd_ipc_message_t resp;
        resp.type        = FBTEXT_IPC_RESP;
        resp.source      = ep;
        resp.destination = msg.source;
        resp.request_id  = msg.request_id;
        resp.arg0        = 0;
        resp.arg1        = 0;
        resp.arg2        = 0;
        resp.arg3        = 0;

        switch (msg.type) {
        case FBTEXT_IPC_CELL_WRITE_REQ: {
            uint16_t col       = (uint16_t)msg.arg0;
            uint16_t row       = (uint16_t)msg.arg1;
            uint32_t codepoint = msg.arg2;
            uint8_t  fg        = (uint8_t)((msg.arg3 >> 8) & 0xF);
            uint8_t  bg        = (uint8_t)(msg.arg3 & 0xF);
            if (col < g_state.cols && row < g_state.rows) {
                fbtext_cell_t *cell = &g_state.cells[row * g_state.cols + col];
                cell->ch  = codepoint;
                cell->fg  = fg;
                cell->bg  = bg;
                fbtext_render_cell(&g_state, col, row);
            }
            break;
        }
        case FBTEXT_IPC_CURSOR_SET_REQ:
            if ((uint16_t)msg.arg0 < g_state.cols &&
                (uint16_t)msg.arg1 < g_state.rows) {
                g_state.cursor.col = (uint16_t)msg.arg0;
                g_state.cursor.row = (uint16_t)msg.arg1;
            }
            break;
        case FBTEXT_IPC_SCROLL_REQ:
            fbtext_scroll_up(&g_state, (uint16_t)msg.arg0);
            break;
        case FBTEXT_IPC_CLEAR_REQ:
            fbtext_clear(&g_state);
            break;
        case FBTEXT_IPC_CONSOLE_MODE_REQ:
            if (msg.arg0 != 0) {
                if (!g_console_ring_enabled) {
                    /* Re-enable in "live tail" mode: drop stale backlog so
                     * returning to tty0 does not replay minutes of old logs
                     * and starve subsequent tty control traffic. */
                    ring->read_pos = ring->write_pos;
                }
                g_console_ring_enabled = 1u;
            } else {
                g_console_ring_enabled = 0u;
            }
            break;
        case FBTEXT_IPC_GEOMETRY_REQ:
            resp.arg0 = g_state.cols;
            resp.arg1 = g_state.rows;
            break;
        default:
            resp.type = FBTEXT_IPC_ERROR;
            resp.arg0 = (uint32_t)-1;
            break;
        }

        if (msg.source != (uint32_t)~0u && msg.request_id != 0) {
            api->ipc_send(ctx, msg.source, &resp);
        }
    }

    return 0;
}
