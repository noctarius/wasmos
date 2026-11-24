#include "fbtext_server.h"
#include "../include/wasmos_driver_abi.h"
#include "fbtext_internal.h"

/*
 * The FBTEXT server shared by every framebuffer driver variant (fbtext_server.h).
 *
 *   1. Probe and map the physical framebuffer.
 *   2. Initialise the cell grid and font.
 *   3. Replay the kernel early-log ring buffer so the full boot log appears.
 *   4. Serve FBTEXT control IPC.
 *
 * The internal rendering code lives in render.c. Message arguments are
 * documented per opcode in abi/opcodes.yaml (subsystem `fbtext`).
 */

/* IPC_EMPTY and IPC_OK values — must stay in sync with kernel ipc.h. */
#define ND_IPC_OK 0
#define ND_IPC_EMPTY 1

/* Staging buffer for the early-log replay, and therefore the cap on how much of
 * the kernel log is replayed at all: a longer log is TRUNCATED to its most
 * recent EARLY_LOG_BUF bytes, with the oldest output skipped rather than shown. */
#define EARLY_LOG_BUF 4096

/* Idle-block bound so the main loop reaches idle/hlt instead of yield-spinning.
 * Every screen change arrives as IPC, which wakes the endpoint immediately, so
 * this only caps how long the loop sleeps with nothing to do. */
#define FB_IDLE_WAIT_MS 200u

static fbtext_state_t g_state;
static uint8_t g_early_log_buf[EARLY_LOG_BUF];
static uint8_t g_text_plane_enabled = 1;
static uint8_t g_gfx_overlay_lock = 0;
/* Bytes of the firmware framebuffer this driver maps; every runtime mode must
 * fit in it. */
static uint32_t g_fb_bytes_limit = 0;

/* Bulk grid blit: the VT grants READ access to a cell-grid xfer-buffer, and
 * FBTEXT_IPC_BLIT_GRID_REQ repaints the whole grid from it in one shot. The
 * per-cell CELL_WRITE alternative is an IPC storm that wedges tty switches. */
static const fbtext_blit_cell_t* g_blit_grid = 0;
/* The endpoint that attached the blit grid; it lays its grid out for the
 * driver's geometry and is told (FBTEXT_IPC_GEOMETRY_CHANGED) when that changes. */
static uint32_t g_blit_client = (uint32_t)~0u;

/* The display presenter (FBTEXT_IPC_PRESENTER_REQ), on a machine whose scanout
 * is shown only when flushed (virtio-gpu); ~0 when none. Every draw is reported
 * to it as DISPLAY_IPC_FLUSH_REQ through a descriptor buffer this driver owns
 * and lends it READ. One flush is in flight at a time -- the buffer is rewritten
 * only after the answer -- and damage drawn meanwhile accumulates in g_damage. */
static uint32_t g_presenter = (uint32_t)~0u;
static uint32_t g_flush_buffer_id = 0;
static wasmos_display_flush_desc_t* g_flush_desc = 0;
static uint8_t g_flush_in_flight = 0;
/* The request id of the flush in flight; an answer counts only when it carries
 * it and comes from g_presenter, so a late answer to an earlier flush cannot
 * free the descriptor while the presenter may still read it. */
static uint32_t g_flush_in_flight_id = 0;
static wasmos_display_rect_t g_damage = {0, 0, 0, 0};
/* Request ids of flushes this driver sends; never 0, which marks a message that
 * expects no answer. */
static uint32_t g_flush_request_id = 1;

/* A PRESENTER_REQ is accepted only from the endpoint the service registry has
 * bound to "display": IPC authenticates an endpoint, not its authority, so the
 * registry is asked (SVC_IPC_LOOKUP_REQ) and the request answered when it has
 * answered. One candidate is checked at a time; a newer request replaces an
 * older one still being checked, which is refused. */
static uint32_t g_proc_ep = (uint32_t)~0u;
static uint32_t g_presenter_candidate = (uint32_t)~0u;
static uint32_t g_presenter_candidate_req = 0;
static uint32_t g_presenter_lookup_id = 0;
/* Request ids of this driver's registry lookups: a range of their own, so a
 * registry answer is never taken for a flush answer or the other way round. */
#define PRESENTER_LOOKUP_ID_BASE 0x7E000000u

typedef struct {
    uint16_t width;
    uint16_t height;
} fb_mode_t;

static const fb_mode_t g_modes[] = {{640u, 480u},
                                    {800u, 600u},
                                    {1024u, 768u},
                                    {1152u, 864u},
                                    {1280u, 720u},
                                    {1280u, 800u},
                                    {1280u, 1024u},
                                    {1366u, 768u},
                                    {1440u, 900u},
                                    {1600u, 900u}};
#define FB_MODE_COUNT ((uint32_t)(sizeof(g_modes) / sizeof(g_modes[0])))

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

static void write_str(wasmos_driver_api_t* api, const char* s) {
    api->console_write(s, str_len(s));
}

static void write_hex32(wasmos_driver_api_t* api, const char* label, uint32_t v) {
    static const char hx[] = "0123456789ABCDEF";
    char buf[64];
    int i = 0;
    const char* p = label;
    while (*p)
        buf[i++] = *p++;
    buf[i++] = '0';
    buf[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4)
        buf[i++] = hx[(v >> s) & 0xF];
    buf[i] = '\0';
    api->console_write(buf, i);
}

/* A mode is offered only when its framebuffer (stride = width) fits the
 * firmware framebuffer this driver maps. */
static int mode_fits_boot_map(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || g_fb_bytes_limit == 0) {
        return 0;
    }
    uint64_t bytes = (uint64_t)width * (uint64_t)height * 4u;
    return bytes <= (uint64_t)g_fb_bytes_limit;
}

static int mode_listed(uint32_t width, uint32_t height) {
    for (uint32_t i = 0; i < FB_MODE_COUNT; ++i) {
        if ((uint32_t)g_modes[i].width == width && (uint32_t)g_modes[i].height == height) {
            return 1;
        }
    }
    return 0;
}

/* The index-th offered mode (stride = width). Returns 0, or -1 past the end. */
static int query_mode_by_index(uint32_t index, uint32_t* out_width, uint32_t* out_height) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < FB_MODE_COUNT; ++i) {
        uint32_t w = (uint32_t)g_modes[i].width;
        uint32_t h = (uint32_t)g_modes[i].height;
        if (!mode_fits_boot_map(w, h)) {
            continue;
        }
        if (seen == index) {
            *out_width = w;
            *out_height = h;
            return 0;
        }
        ++seen;
    }
    return -1;
}

static uint32_t count_supported_modes(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < FB_MODE_COUNT; ++i) {
        if (mode_fits_boot_map((uint32_t)g_modes[i].width, (uint32_t)g_modes[i].height)) {
            ++count;
        }
    }
    return count;
}

static void damage_add(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (g_presenter == (uint32_t)~0u || w == 0u || h == 0u) {
        return;
    }
    if (g_damage.width == 0u || g_damage.height == 0u) {
        g_damage = (wasmos_display_rect_t){x, y, w, h};
        return;
    }
    uint32_t x1 = g_damage.x + g_damage.width > x + w ? g_damage.x + g_damage.width : x + w;
    uint32_t y1 = g_damage.y + g_damage.height > y + h ? g_damage.y + g_damage.height : y + h;
    g_damage.x = g_damage.x < x ? g_damage.x : x;
    g_damage.y = g_damage.y < y ? g_damage.y : y;
    g_damage.width = x1 - g_damage.x;
    g_damage.height = y1 - g_damage.y;
}

static void damage_screen(void) {
    damage_add(0, 0, (uint32_t)g_state.cols * CELL_W, (uint32_t)g_state.rows * CELL_H);
}

static void damage_cell(uint32_t col, uint32_t row) {
    damage_add(col * CELL_W, row * CELL_H, CELL_W, CELL_H);
}

/* Sends the accumulated damage when no flush is in flight. A send the
 * presenter's queue refuses keeps the damage for the next attempt. */
static void send_reply(wasmos_driver_api_t* api, uint32_t ctx, uint32_t ep, uint32_t dest,
                       uint32_t type, uint32_t request_id, uint32_t arg0) {
    nd_ipc_message_t m;
    m.type = type;
    m.source = ep;
    m.destination = dest;
    m.request_id = request_id;
    m.arg0 = arg0;
    m.arg1 = 0;
    m.arg2 = 0;
    m.arg3 = 0;
    (void)api->ipc_send(ctx, dest, &m);
}

static void flush_damage(wasmos_driver_api_t* api, uint32_t ctx, uint32_t ep) {
    if (g_presenter == (uint32_t)~0u || g_flush_in_flight || !g_flush_desc ||
        g_damage.width == 0u || g_damage.height == 0u) {
        return;
    }
    g_flush_desc->version = DISPLAY_FLUSH_DESC_VERSION;
    g_flush_desc->count = 1u;
    g_flush_desc->rects[0] = g_damage;
    nd_ipc_message_t m;
    m.type = DISPLAY_IPC_FLUSH_REQ;
    m.source = ep;
    m.destination = g_presenter;
    m.request_id = g_flush_request_id;
    m.arg0 = g_flush_buffer_id;
    m.arg1 = 0;
    m.arg2 = DISPLAY_FLUSH_DESC_SIZE(1);
    m.arg3 = 0;
    if (api->ipc_send(ctx, g_presenter, &m) != 0) {
        return;
    }
    g_flush_in_flight_id = g_flush_request_id;
    g_flush_request_id = g_flush_request_id == 0x7DFFFFFFu ? 1u : g_flush_request_id + 1u;
    g_flush_in_flight = 1u;
    g_damage = (wasmos_display_rect_t){0, 0, 0, 0};
}

/* Asks the registry which endpoint "display" is bound to, for the candidate
 * presenter recorded in g_presenter_candidate. Returns 0, or a packed
 * WASMOS_ERR_* code when the question cannot be asked. */
static int32_t verify_presenter(wasmos_driver_api_t* api, uint32_t ctx, uint32_t ep) {
    if (g_proc_ep == (uint32_t)~0u) {
        return WASMOS_ERR_DRIVER_NO_PROC_ENDPOINT;
    }
    nd_ipc_message_t m;
    m.type = SVC_IPC_LOOKUP_REQ;
    m.source = ep;
    m.destination = g_proc_ep;
    g_presenter_lookup_id = PRESENTER_LOOKUP_ID_BASE + ((g_presenter_lookup_id + 1u) & 0xFFFFu);
    m.request_id = g_presenter_lookup_id;
    /* "display", packed as the registry's 16-byte name: four little-endian
     * words, zero-filled. */
    m.arg0 = 0x70736964u; /* "disp" */
    m.arg1 = 0x0079616Cu; /* "lay\0" */
    m.arg2 = 0;
    m.arg3 = 0;
    return api->ipc_send(ctx, g_proc_ep, &m) == 0 ? 0 : WASMOS_ERR_DRIVER_REGISTER;
}

/* FBTEXT_IPC_PRESENTER_REQ, once verified: `presenter` becomes the endpoint damage is reported
 * to. A fresh descriptor buffer is lent to it; releasing the previous one
 * cascade-revokes its loan to an earlier presenter. The whole screen is then
 * reported once. Returns 0, or a packed WASMOS_ERR_* code. */
static int32_t attach_presenter(wasmos_driver_api_t* api, uint32_t presenter) {
    if (g_flush_buffer_id != 0u) {
        (void)api->xfer_buffer_release(g_flush_buffer_id);
        g_flush_buffer_id = 0;
        g_flush_desc = 0;
    }
    g_presenter = (uint32_t)~0u;
    g_flush_in_flight = 0u;
    g_flush_in_flight_id = 0u;
    uint32_t id = 0;
    void* raw = api->xfer_buffer_acquire(ND_BUFFER_KIND_XFER, 4096u, &id);
    if (!raw || id == 0u) {
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    if (api->xfer_buffer_borrow(presenter, id, ND_BUFFER_BORROW_READ) <= 0) {
        (void)api->xfer_buffer_release(id);
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    g_flush_buffer_id = id;
    g_flush_desc = (wasmos_display_flush_desc_t*)raw;
    g_presenter = presenter;
    damage_screen();
    return 0;
}

/* Programs width x height through the backend and rebuilds the grid on success.
 * The kernel's framebuffer info is updated to match, so a consumer that reads it
 * -- the compositor remaps from it -- follows the change. Returns 0 or the packed
 * code to answer SET_RESOLUTION with. */
static int32_t set_resolution(wasmos_driver_api_t* api, const fbtext_backend_t* backend,
                              uint32_t width, uint32_t height) {
    if (!mode_listed(width, height)) {
        return WASMOS_ERR_GFX_MODE_SET_FAILED;
    }
    if (!mode_fits_boot_map(width, height)) {
        return WASMOS_ERR_GFX_MODE_TOO_LARGE;
    }
    int32_t rc = backend->set_mode(api, width, height, width);
    if (rc != 0) {
        return rc;
    }
    rc = api->framebuffer_set_geometry(width, height, width);
    if (rc != 0) {
        return rc;
    }
    fbtext_render_init(&g_state, g_state.fb, width, width, height);
    fbtext_clear(&g_state);
    return 0;
}

static void replay_early_log(wasmos_driver_api_t* api) {
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
    uint32_t start = total; /* exclusive upper bound, scan left */
    while (start > 0) {
        start--;
        if (g_early_log_buf[start] == '\n') {
            nl_count++;
            if (nl_count >= max_rows) {
                start++; /* replay from byte after this newline */
                break;
            }
        }
    }

    for (uint32_t i = start; i < total; i++) {
        fbtext_put_char(&g_state, (uint32_t)g_early_log_buf[i]);
    }
}

int fbtext_serve(wasmos_driver_api_t* api, const fbtext_backend_t* backend) {
    /* The ABI stamp is checked before any other api field: a mismatched table
     * would put the function pointers at different offsets than this build
     * expects. */
    if (!api || !backend || api->abi_magic != WASMOS_NATIVE_ABI_MAGIC ||
        api->abi_version != WASMOS_NATIVE_ABI_VERSION || !api->console_write ||
        !api->xfer_buffer_acquire || !api->framebuffer_set_geometry) {
        return -2;
    }

    write_str(api, backend->tag);
    write_str(api, " probing\n");

    nd_framebuffer_info_t info;
    if (api->framebuffer_info(&info) != 0 || info.framebuffer_base == 0 ||
        info.framebuffer_width == 0 || info.framebuffer_height == 0 ||
        info.framebuffer_stride == 0) {
        write_str(api, "[framebuffer] not present\n");
        return 0;
    }

    uint32_t fb_width = info.framebuffer_width;
    uint32_t fb_height = info.framebuffer_height;
    uint32_t fb_stride = info.framebuffer_stride;
    write_hex32(api, "[framebuffer] boot mode ", fb_width);
    write_hex32(api, "x", fb_height);
    write_hex32(api, " stride=", fb_stride);
    write_str(api, "\n");

    /* Before the framebuffer is mapped or cleared: a backend that declines
     * leaves the pixels to whichever driver the framebuffer does belong to. */
    if (backend->init) {
        int32_t init_rc = backend->init(api, &info);
        if (init_rc == FBTEXT_BACKEND_DECLINED) {
            write_str(api, backend->tag);
            write_str(api, " the framebuffer is not this backend's; declining\n");
            return 0;
        }
        if (init_rc != 0) {
            return init_rc;
        }
    }

    /* Map the whole framebuffer the firmware reported, which may hold more than
     * the boot mode needs (edk2's ramfb sizes it for its largest mode); every
     * runtime mode must fit in it. Never less than the boot geometry. */
    uint64_t boot_bytes = (uint64_t)fb_stride * fb_height * 4u;
    uint64_t fb_bytes = info.framebuffer_size > boot_bytes ? info.framebuffer_size : boot_bytes;
    if (fb_bytes > 0xFFFFF000u) {
        fb_bytes = 0xFFFFF000u;
    }
    g_fb_bytes_limit = (uint32_t)fb_bytes;
    uint32_t size = (g_fb_bytes_limit + 0xFFFu) & ~0xFFFu;

    write_str(api, "[framebuffer] mapping\n");
    /* The framebuffer is an owned xfer_buffer of kind=framebuffer (backed by the
     * hardware framebuffer); acquire + map it, never borrowed. Held for the
     * driver's lifetime, so the buffer_id is not retained. */
    uint32_t fb_buffer_id = 0;
    void* fb = api->xfer_buffer_acquire(ND_BUFFER_KIND_FRAMEBUFFER, size, &fb_buffer_id);
    if (!fb) {
        write_str(api, "[framebuffer] map failed\n");
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }

    fbtext_render_init(&g_state, (uint32_t*)fb, fb_stride, fb_width, fb_height);
    fbtext_clear(&g_state);

    replay_early_log(api);

    write_str(api, "[framebuffer] ready\n");

    if (api->spawn_info) {
        wasmos_spawn_info_t info_sp;
        char args_unused[4];
        if (api->spawn_info(&info_sp, args_unused, sizeof(args_unused)) == 0) {
            g_proc_ep = info_sp.proc_endpoint;
        }
    }
    uint32_t ep = api->ipc_create_endpoint();
    if (ep == (uint32_t)~0u) {
        write_str(api, "[framebuffer] ipc endpoint failed\n");
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    /* context_id == pid for a native driver. */
    uint32_t ctx = api->sched_current_pid();
    if (api->console_register_fb(ctx, ep) != 0) {
        write_str(api, "[framebuffer] register endpoint failed\n");
        return WASMOS_ERR_DRIVER_REGISTER;
    }

    api->proc_notify_ready();

    /* The driver is a pure blit surface: everything it paints arrives as IPC
     * from the vt (cells, blits, scroll, clear) or is the early-log replay
     * above, so there is nothing to poll for and the idle path just blocks. */
    nd_ipc_message_t msg;
    for (;;) {
        int rc = api->ipc_recv(ctx, ep, &msg);
        if (rc == ND_IPC_EMPTY) {
            if (api->ipc_wait) {
                api->ipc_wait(ctx, ep, FB_IDLE_WAIT_MS);
            } else {
                api->sched_yield();
            }
            continue;
        }
        if (rc != ND_IPC_OK) {
            write_str(api, "[framebuffer] ipc error\n");
            break;
        }

        nd_ipc_message_t resp;
        resp.type = FBTEXT_IPC_RESP;
        resp.source = ep;
        resp.destination = msg.source;
        resp.request_id = msg.request_id;
        resp.arg0 = 0;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;

        switch (msg.type) {
        case FBTEXT_IPC_CELL_WRITE_REQ: {
            if (!g_text_plane_enabled || g_gfx_overlay_lock) {
                break;
            }
            uint16_t col = (uint16_t)msg.arg0;
            uint16_t row = (uint16_t)msg.arg1;
            uint32_t codepoint = msg.arg2;
            uint8_t fg = (uint8_t)((msg.arg3 >> 8) & 0xF);
            uint8_t bg = (uint8_t)(msg.arg3 & 0xF);
            if (col < g_state.cols && row < g_state.rows) {
                fbtext_cell_t* cell = &g_state.cells[row * g_state.cols + col];
                cell->ch = codepoint;
                cell->fg = fg;
                cell->bg = bg;
                fbtext_render_cell(&g_state, col, row);
                damage_cell(col, row);
            }
            break;
        }
        case FBTEXT_IPC_CURSOR_SET_REQ:
            if (!g_text_plane_enabled || g_gfx_overlay_lock) {
                break;
            }
            if ((uint16_t)msg.arg0 < g_state.cols && (uint16_t)msg.arg1 < g_state.rows) {
                g_state.cursor.col = (uint16_t)msg.arg0;
                g_state.cursor.row = (uint16_t)msg.arg1;
            }
            break;
        case FBTEXT_IPC_SCROLL_REQ:
            if (!g_text_plane_enabled || g_gfx_overlay_lock) {
                break;
            }
            fbtext_scroll_up(&g_state, (uint16_t)msg.arg0);
            damage_screen();
            break;
        case FBTEXT_IPC_CLEAR_REQ:
            if (!g_text_plane_enabled || g_gfx_overlay_lock) {
                break;
            }
            fbtext_clear(&g_state);
            damage_screen();
            break;
        case FBTEXT_IPC_GEOMETRY_REQ:
            resp.arg0 = g_state.cols;
            resp.arg1 = g_state.rows;
            break;
        case FBTEXT_IPC_GFX_OVERLAY_REQ:
            g_gfx_overlay_lock = (msg.arg0 != 0) ? 1u : 0u;
            break;
        case FBTEXT_IPC_BLIT_ATTACH_REQ:
            /* Map the VT's cell-grid buffer once; render from it on BLIT_GRID. */
            if (api->xfer_buffer_map_borrowed) {
                const void* p =
                    api->xfer_buffer_map_borrowed(ND_BUFFER_KIND_XFER, msg.arg0, msg.arg1);
                if (p) {
                    g_blit_grid = (const fbtext_blit_cell_t*)p;
                    g_blit_client = msg.source;
                }
            }
            break;
        case FBTEXT_IPC_BLIT_GRID_REQ: {
            if (!g_text_plane_enabled || g_gfx_overlay_lock || !g_blit_grid) {
                break;
            }
            /* Clamp the grid stride/extent to the driver's geometry so a bad request
             * can never read past the granted buffer (bounded by cols*rows). */
            uint32_t stride = (uint32_t)msg.arg0;
            if (stride > g_state.cols) {
                stride = g_state.cols;
            }
            uint32_t rows = (uint32_t)msg.arg1;
            if (rows > g_state.rows) {
                rows = g_state.rows;
            }
            for (uint32_t row = 0; row < rows; ++row) {
                for (uint32_t col = 0; col < stride; ++col) {
                    const fbtext_blit_cell_t* s = &g_blit_grid[row * stride + col];
                    fbtext_cell_t* d = &g_state.cells[row * g_state.cols + col];
                    d->ch = s->ch;
                    d->fg = s->fg;
                    d->bg = s->bg;
                    d->attr = s->attr;
                    fbtext_render_cell(&g_state, (uint16_t)col, (uint16_t)row);
                }
            }
            damage_add(0, 0, stride * CELL_W, rows * CELL_H);
            break;
        }
        case FBTEXT_IPC_QUERY_CAPS_REQ:
            if (backend->set_mode) {
                resp.arg0 = FBTEXT_CAP_SET_RESOLUTION | FBTEXT_CAP_QUERY_MODES;
                resp.arg1 = count_supported_modes();
            }
            break;
        case FBTEXT_IPC_QUERY_MODES_REQ: {
            uint32_t w = 0;
            uint32_t h = 0;
            if (!backend->set_mode) {
                resp.type = FBTEXT_IPC_ERROR;
                resp.arg0 = (uint32_t)WASMOS_ERR_GFX_NO_RUNTIME_MODES;
                break;
            }
            if (query_mode_by_index(msg.arg0, &w, &h) != 0) {
                resp.type = FBTEXT_IPC_ERROR;
                resp.arg0 = (uint32_t)WASMOS_ERR_GFX_BAD_MODE_INDEX;
                break;
            }
            resp.arg0 = w;
            resp.arg1 = h;
            resp.arg2 = w;
            break;
        }
        case FBTEXT_IPC_SET_RESOLUTION_REQ: {
            if (!backend->set_mode) {
                resp.type = FBTEXT_IPC_ERROR;
                resp.arg0 = (uint32_t)WASMOS_ERR_GFX_NO_RUNTIME_MODES;
                break;
            }
            int32_t set_rc = set_resolution(api, backend, msg.arg0, msg.arg1);
            if (set_rc != 0) {
                resp.type = FBTEXT_IPC_ERROR;
                resp.arg0 = (uint32_t)set_rc;
                break;
            }
            resp.arg0 = msg.arg0;
            resp.arg1 = msg.arg1;
            damage_screen();
            /* TODO(fbtext-geometry-notify): the notify is sent once; when the
             * vt's queue is full it is lost and the vt keeps its old grid until
             * the next framebuffer bind. */
            if (g_blit_client != (uint32_t)~0u) {
                nd_ipc_message_t note = resp;
                note.type = FBTEXT_IPC_GEOMETRY_CHANGED;
                note.destination = g_blit_client;
                note.request_id = 0;
                note.arg0 = g_state.cols;
                note.arg1 = g_state.rows;
                api->ipc_send(ctx, g_blit_client, &note);
            }
            break;
        }
        case FBTEXT_IPC_PRESENTER_REQ: {
            /* Answered once the registry has (SVC_IPC_LOOKUP_RESP below). A
             * candidate still being checked is refused in favour of this one. */
            if (g_presenter_candidate != (uint32_t)~0u) {
                send_reply(api,
                           ctx,
                           ep,
                           g_presenter_candidate,
                           FBTEXT_IPC_ERROR,
                           g_presenter_candidate_req,
                           (uint32_t)WASMOS_ERR_DISPLAY_NOT_PRESENTER);
            }
            g_presenter_candidate = msg.source;
            g_presenter_candidate_req = msg.request_id;
            int32_t verify_rc = verify_presenter(api, ctx, ep);
            if (verify_rc != 0) {
                g_presenter_candidate = (uint32_t)~0u;
                resp.type = FBTEXT_IPC_ERROR;
                resp.arg0 = (uint32_t)verify_rc;
                break;
            }
            continue;
        }
        case SVC_IPC_LOOKUP_RESP: {
            if (msg.source != g_proc_ep || msg.request_id != g_presenter_lookup_id ||
                g_presenter_candidate == (uint32_t)~0u) {
                continue;
            }
            uint32_t candidate = g_presenter_candidate;
            uint32_t candidate_req = g_presenter_candidate_req;
            g_presenter_candidate = (uint32_t)~0u;
            int32_t attach_rc = msg.arg0 == candidate ? attach_presenter(api, candidate)
                                                      : WASMOS_ERR_DISPLAY_NOT_PRESENTER;
            send_reply(api,
                       ctx,
                       ep,
                       candidate,
                       attach_rc == 0 ? FBTEXT_IPC_RESP : FBTEXT_IPC_ERROR,
                       candidate_req,
                       (uint32_t)attach_rc);
            flush_damage(api, ctx, ep);
            continue;
        }
        case DISPLAY_IPC_RESP:
        case DISPLAY_IPC_ERROR:
            /* The presenter answered the flush in flight: the descriptor buffer
             * is this driver's again. Any other answer -- another sender, an
             * earlier flush -- leaves the flag alone. A refused flush is not
             * retried; the presenter's own periodic present covers it. Not a
             * request, so no reply. */
            if (g_flush_in_flight && msg.source == g_presenter &&
                msg.request_id == g_flush_in_flight_id) {
                g_flush_in_flight = 0u;
            }
            flush_damage(api, ctx, ep);
            continue;
        default:
            resp.type = FBTEXT_IPC_ERROR;
            resp.arg0 = (uint32_t)WASMOS_ERR_GFX_UNSUPPORTED_REQUEST;
            break;
        }
        flush_damage(api, ctx, ep);

        if (msg.source != (uint32_t)~0u && msg.request_id != 0) {
            api->ipc_send(ctx, msg.source, &resp);
        }
    }

    return WASMOS_ERR_DRIVER_EVENT_LOOP;
}
