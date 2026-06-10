const c = @cImport({
    @cInclude("gfx_compositor_imports.h");
});
const sys = @import("libsys");


const IPC_OK: i32 = 0;
const IPC_EMPTY: i32 = 1;
const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;
const GFX_REQUEST_BASE: u32 = 0x7000;
const GFX_FB_LOOKUP_RETRIES: u32 = 2048;
const GFX_MAX_WINDOWS: usize = 32;
const GFX_MAX_BUFFERS: usize = 64;
const GFX_MAX_DAMAGE_RECTS: u32 = 256;
const GFX_MAX_EVENTS: usize = 128;
const GFX_MAX_GLYPH_CACHE: usize = 64;
const GFX_MAX_GLYPH_BYTES: usize = 4096;
const GFX_MAX_TITLE_LABEL: usize = 24;
const FONT_INIT_MAX_ATTEMPTS: u32 = 64;
const FONT_INIT_RETRY_MASK: u32 = 0x3F;
const TITLE_GLYPHS: []const u8 = "win 0123456789";
const GFX_TITLE_TEXT_ENABLED: bool = true;
/// Enable per-event serial traces for click/pointer/present debugging.
/// Set to false to suppress serial noise in production.
const GFX_TRACE: bool = false;
const GFX_WINDOW_MIN_DIM: u32 = 1;
const GFX_WINDOW_MAX_DIM: u32 = 8192;
const GFX_WINDOW_FLAG_SYSTEM: u32 = 1 << 0;
const GFX_WINDOW_Z_SYSTEM: u32 = 0xFFFF_FFFE;
const PAGE_SIZE: u64 = 4096;
const CURSOR_W: i32 = 9;
const CURSOR_H: i32 = 14;
const CHROME_BORDER: i32 = 1;
const CHROME_TITLE_H: i32 = 24;
const CHROME_CLOSE_SZ: i32 = 14;
const CHROME_CLOSE_PAD: i32 = 3;
const CHROME_MAX_SZ: i32 = 14;
const CHROME_MAX_PAD: i32 = 3;
const CHROME_BTN_GAP: i32 = 4;
const CHROME_MAX_HIT_W: i32 = 30;
const CHROME_RESIZE_HANDLE_SZ: i32 = 12;
const CHROME_TITLE_FONT_PX: u32 = 14;
const SCANCODE_MAP_LEN: usize = 58;

const key_layout_t = enum(u8) {
    us_qwerty = 0,
    de_nodeadkeys = 1,
};

const keymap_t = struct {
    plain: [SCANCODE_MAP_LEN]u8,
    shift: [SCANCODE_MAP_LEN]u8,
    altgr: [SCANCODE_MAP_LEN]u8,
};

var g_api: ?*c.wasmos_driver_api_t = null;
var g_proc_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_gfx_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_fb_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_vt_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_kbd_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_mouse_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_font_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_font_title_handle: u32 = 0;
var g_font_init_failed: bool = false;
var g_font_init_attempts: u32 = 0;
var g_font_prime_index: usize = 0;
var g_overlay_locked: bool = false;
var g_next_window_id: u32 = 1;
var g_next_z: u32 = 1;
var g_focused_window_id: u32 = 0;
var g_drag_window_id: u32 = 0;
var g_resize_window_id: u32 = 0;
var g_pointer_x: i32 = 0;
var g_pointer_y: i32 = 0;
var g_pointer_buttons: u32 = 0;
var g_rng_state: u32 = 0xA5A5_5A5A;
var g_damage_marker_logged: bool = false;
var g_window_owner_deny_logged: bool = false;
var g_buffer_owner_deny_logged: bool = false;
var g_kbd_subscribed: bool = false;
var g_mouse_subscribed: bool = false;
var g_shift_down: bool = false;
var g_altgr_down: bool = false;
var g_idle_housekeeping_counter: u32 = 0;
var g_total_handled_counter: u32 = 0;
var g_runtime_lookup_req_id: u32 = GFX_REQUEST_BASE + 0x4000;
var g_key_layout: key_layout_t = .de_nodeadkeys;
var g_title_dbg_open_fail_logged: bool = false;
var g_title_dbg_prime_fail_logged: bool = false;
var g_dirty_pending: bool = false;
var g_dirty_full: bool = false;
var g_dirty_rect: c.gfx_rect_t = .{ .x = 0, .y = 0, .w = 0, .h = 0 };

var g_fb_info: c.nd_framebuffer_info_t = .{
    .framebuffer_base = 0,
    .framebuffer_size = 0,
    .framebuffer_width = 0,
    .framebuffer_height = 0,
    .framebuffer_stride = 0,
    .framebuffer_gop_pixel_format = 0,
};
var g_fb_info_valid: bool = false;
var g_fb_pixels: ?[*]volatile u32 = null;
var g_backbuffer_pixels: ?[*]u32 = null;
var g_backbuffer_shmem_id: u32 = 0;
var g_backbuffer_capacity_bytes: u32 = 0;

const window_slot_t = struct {
    in_use: bool = false,
    owner_endpoint: u32 = IPC_ENDPOINT_NONE,
    window_id: u32 = 0,
    x: i32 = 0,
    y: i32 = 0,
    width: u32 = 0,
    height: u32 = 0,
    z: u32 = 0,
    generation: u32 = 1,
    current_buffer_id: u32 = 0,
    is_maximized: bool = false,
    restore_x: i32 = 0,
    restore_y: i32 = 0,
    restore_w: u32 = 0,
    restore_h: u32 = 0,
    flags: u32 = 0,
};

const buffer_state_t = enum(u8) {
    allocated = 0,
    acquired = 1,
};

const buffer_slot_t = struct {
    in_use: bool = false,
    owner_endpoint: u32 = IPC_ENDPOINT_NONE,
    buffer_id: u32 = 0,
    shmem_id: u32 = 0,
    width: u32 = 0,
    height: u32 = 0,
    stride_bytes: u32 = 0,
    state: buffer_state_t = .allocated,
    bound_window_id: u32 = 0,
    bound_window_generation: u32 = 0,
    // Cached shmem mapping — set at allocation, valid until the slot is freed.
    // Avoids kernel shmem_map/shmem_unmap on every composite call.
    mapped_pixels: ?[*]const u32 = null,
};

const gfx_event_t = struct {
    endpoint: u32 = IPC_ENDPOINT_NONE,
    event_type: u32 = 0,
    arg1: u32 = 0,
    arg2: u32 = 0,
    arg3: u32 = 0,
};

const glyph_cache_entry_t = struct {
    valid: bool = false,
    codepoint: u32 = 0,
    shmem_id: u32 = 0,
    w: i32 = 0,
    h: i32 = 0,
    x0: i16 = 0,
    y0: i16 = 0,
    mask_len: usize = 0,
    mask_data: [GFX_MAX_GLYPH_BYTES]u8 = [_]u8{0} ** GFX_MAX_GLYPH_BYTES,
};

var g_windows: [GFX_MAX_WINDOWS]window_slot_t = [_]window_slot_t{.{}} ** GFX_MAX_WINDOWS;
var g_buffers: [GFX_MAX_BUFFERS]buffer_slot_t = [_]buffer_slot_t{.{}} ** GFX_MAX_BUFFERS;
var g_events: [GFX_MAX_EVENTS]gfx_event_t = [_]gfx_event_t{.{}} ** GFX_MAX_EVENTS;
var g_glyph_cache: [GFX_MAX_GLYPH_CACHE]glyph_cache_entry_t = [_]glyph_cache_entry_t{.{}} ** GFX_MAX_GLYPH_CACHE;
const title_run_cache_entry_t = struct {
    valid: bool = false,
    window_id: u32 = 0,
    window_generation: u32 = 0,
    label_len: usize = 0,
    label: [GFX_MAX_TITLE_LABEL]u8 = [_]u8{0} ** GFX_MAX_TITLE_LABEL,
    w: i32 = 0,
    h: i32 = 0,
    x0: i16 = 0,
    y0: i16 = 0,
    advance_x: i32 = 0,
    mask_len: usize = 0,
    mask_data: [GFX_MAX_GLYPH_BYTES]u8 = [_]u8{0} ** GFX_MAX_GLYPH_BYTES,
};
var g_title_run_cache: [GFX_MAX_WINDOWS]title_run_cache_entry_t = [_]title_run_cache_entry_t{.{}} ** GFX_MAX_WINDOWS;
var g_font_text_shmem_id: u32 = 0;
var g_font_text_ptr: ?[*]u8 = null;
var g_font_text_cap: usize = 0;
var g_font_mask_shmem_id: u32 = 0;
var g_font_mask_ptr: ?[*]u8 = null;
var g_font_mask_cap: usize = 0;
var g_event_head: usize = 0;
var g_event_tail: usize = 0;
var g_ipc_loop: sys.NativeEventLoop = undefined;

fn api() *c.wasmos_driver_api_t {
    return g_api.?;
}

fn ctxId() u32 {
    return api().sched_current_pid.?();
}

fn logMsg(msg: []const u8) void {
    _ = api().console_write.?(msg.ptr, @intCast(msg.len));
}

fn logHex32(prefix: []const u8, v: u32) void {
    var buf: [64]u8 = undefined;
    var n: usize = 0;
    var i: usize = 0;
    while (i < prefix.len and n < buf.len) : (i += 1) {
        buf[n] = prefix[i];
        n += 1;
    }
    if (n + 10 >= buf.len) return;
    buf[n] = '0'; n += 1;
    buf[n] = 'x'; n += 1;
    const hex = "0123456789abcdef";
    var shift: i32 = 28;
    while (shift >= 0) : (shift -= 4) {
        const d: u8 = @intCast((v >> @intCast(shift)) & 0xF);
        buf[n] = hex[d];
        n += 1;
    }
    buf[n] = '\n';
    n += 1;
    _ = api().console_write.?(buf[0..n].ptr, @intCast(n));
}

fn logModeResolution(width: u32, height: u32) void {
    var buf: [48]u8 = undefined;
    var n: usize = 0;
    const prefix = "[gfx] fb mode ";
    var i: usize = 0;
    while (i < prefix.len and n < buf.len) : (i += 1) {
        buf[n] = prefix[i];
        n += 1;
    }

    n += append_u32_decimal(buf[n..], width);
    if (n < buf.len) {
        buf[n] = 'x';
        n += 1;
    }
    n += append_u32_decimal(buf[n..], height);
    if (n < buf.len) {
        buf[n] = '\n';
        n += 1;
    }
    _ = api().console_write.?(buf[0..n].ptr, @intCast(n));
}

fn append_u32_decimal(dst: []u8, v: u32) usize {
    if (dst.len == 0) return 0;
    if (v == 0) {
        dst[0] = '0';
        return 1;
    }
    var tmp: [10]u8 = undefined;
    var t: usize = 0;
    var cur = v;
    while (cur > 0 and t < tmp.len) : (t += 1) {
        tmp[t] = @as(u8, @intCast('0' + (cur % 10)));
        cur /= 10;
    }
    var out: usize = 0;
    while (out < t and out < dst.len) : (out += 1) {
        dst[out] = tmp[t - 1 - out];
    }
    return out;
}

fn packFbPixel(r: u32, g: u32, b: u32) u32 {
    const fmt = g_fb_info.framebuffer_gop_pixel_format & 0xF;
    // GOP PixelFormat:
    // 0 = PixelRedGreenBlueReserved8BitPerColor
    // 1 = PixelBlueGreenRedReserved8BitPerColor
    // Fallback: treat as BGR layout used by common OVMF/QEMU paths.
    return switch (fmt) {
        0 => (0xFF << 24) | (b << 16) | (g << 8) | r,
        1 => (0xFF << 24) | (r << 16) | (g << 8) | b,
        else => (0xFF << 24) | (r << 16) | (g << 8) | b,
    };
}

fn rand_u32() u32 {
    var x = g_rng_state;
    if (x == 0) x = 0x6D2B_79F5;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

fn svc_register(name: []const u8, request_id: u32) i32 {
    return sys.svcRegister(api(), g_proc_endpoint, g_gfx_endpoint, name, request_id);
}

fn svc_lookup(name: []const u8, request_id: u32) i32 {
    return sys.svcLookup(api(), g_proc_endpoint, g_gfx_endpoint, name, request_id);
}

fn lookup_fb_endpoint() i32 {
    g_fb_endpoint = sys.svcLookupEndpointRetry(api(), g_proc_endpoint, g_gfx_endpoint, "fb", GFX_REQUEST_BASE, GFX_FB_LOOKUP_RETRIES) orelse return -1;
    return 0;
}

fn lookup_vt_endpoint() i32 {
    g_vt_endpoint = sys.svcLookupEndpointRetry(api(), g_proc_endpoint, g_gfx_endpoint, "vt", GFX_REQUEST_BASE + 0x100, GFX_FB_LOOKUP_RETRIES) orelse return -1;
    return 0;
}

fn lookup_kbd_endpoint() i32 {
    g_kbd_endpoint = sys.svcLookupEndpointRetry(api(), g_proc_endpoint, g_gfx_endpoint, "kbd", GFX_REQUEST_BASE + 0x180, GFX_FB_LOOKUP_RETRIES) orelse return -1;
    return 0;
}

fn lookup_mouse_endpoint() i32 {
    g_mouse_endpoint = sys.svcLookupEndpointRetry(api(), g_proc_endpoint, g_gfx_endpoint, "mouse", GFX_REQUEST_BASE + 0x1C0, GFX_FB_LOOKUP_RETRIES) orelse return -1;
    return 0;
}

fn lookup_font_endpoint() i32 {
    g_font_endpoint = sys.svcLookupEndpointRetry(api(), g_proc_endpoint, g_gfx_endpoint, "font", GFX_REQUEST_BASE + 0x240, GFX_FB_LOOKUP_RETRIES) orelse return -1;
    return 0;
}

fn open_title_font_handle() i32 {
    if (g_font_endpoint == IPC_ENDPOINT_NONE) return -1;
    var reply: c.nd_ipc_message_t = undefined;
    const req_id = g_runtime_lookup_req_id;
    g_runtime_lookup_req_id +%= 1;
    if (font_ipc_call_budgeted(g_font_endpoint, req_id, c.FONT_IPC_OPEN_FONT_REQ, c.FONT_ID_ROBOTO, CHROME_TITLE_FONT_PX, 0, 0, &reply, 32) != 0) {
        return -1;
    }
    if (reply.type != c.FONT_IPC_RESP or @as(i32, @bitCast(reply.arg0)) != c.FONT_STATUS_OK) {
        return -1;
    }
    g_font_title_handle = reply.arg1;
    return if (g_font_title_handle == 0) -1 else 0;
}

fn ensure_font_title_ready_lazy() void {
    if (g_font_title_handle != 0 or g_font_init_failed) return;
    if (active_window_count() == 0) return;
    if ((g_idle_housekeeping_counter & FONT_INIT_RETRY_MASK) != 0) return;
    if (g_font_endpoint == IPC_ENDPOINT_NONE) {
        const ep = svc_lookup("font", g_runtime_lookup_req_id);
        g_runtime_lookup_req_id +%= 1;
        if (ep < 0) return;
        g_font_endpoint = @bitCast(ep);
    }
    if (open_title_font_handle() != 0) {
        if (!g_title_dbg_open_fail_logged) {
            g_title_dbg_open_fail_logged = true;
            logMsg("[dbg-title] font open failed\n");
        }
        return;
    }
    request_repaint_full();
}

fn subscribe_keyboard() i32 {
    if (g_kbd_endpoint == IPC_ENDPOINT_NONE) return -1;
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 3;
    if (ipc_call(g_kbd_endpoint, req_id, c.KBD_IPC_SUBSCRIBE_REQ, 0, 0, 0, 0, &reply) != 0) {
        return -1;
    }
    if (reply.type != c.KBD_IPC_SUBSCRIBE_RESP or @as(i32, @bitCast(reply.arg0)) != 0) {
        return -1;
    }
    g_kbd_subscribed = true;
    return 0;
}

fn subscribe_mouse() i32 {
    if (g_mouse_endpoint == IPC_ENDPOINT_NONE) return -1;
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 4;
    if (ipc_call(g_mouse_endpoint, req_id, c.MOUSE_IPC_SUBSCRIBE_REQ, 0, 0, 0, 0, &reply) != 0) {
        return -1;
    }
    if (reply.type != c.MOUSE_IPC_SUBSCRIBE_RESP or @as(i32, @bitCast(reply.arg0)) != 0) {
        return -1;
    }
    g_mouse_subscribed = true;
    return 0;
}

fn endpoint_alive(endpoint: u32) bool {
    if (endpoint == IPC_ENDPOINT_NONE) return false;
    if (api().ipc_endpoint_owner == null) return true;
    var owner_context_id: u32 = 0;
    return api().ipc_endpoint_owner.?(endpoint, &owner_context_id) == 0;
}

const KEYMAP_US = keymap_t{
    .plain = [_]u8{
        0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8',
        '9', '0', '-', '=', 0x08, 0x09, 'q', 'w', 'e', 'r',
        't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0x0A, 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
        '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
        'm', ',', '.', '/', 0, '*', 0, ' ',
    },
    .shift = [_]u8{
        0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*',
        '(', ')', '_', '+', 0x08, 0x09, 'Q', 'W', 'E', 'R',
        'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0x0A, 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
        '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
        'M', '<', '>', '?', 0, '*', 0, ' ',
    },
    .altgr = [_]u8{0} ** SCANCODE_MAP_LEN,
};

const KEYMAP_DE_NODEADKEYS = keymap_t{
    .plain = [_]u8{
        0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8',
        '9', '0', 0xDF, 0xB4, 0x08, 0x09, 'q', 'w', 'e', 'r',
        't', 'z', 'u', 'i', 'o', 'p', 0xFC, '+', 0x0A, 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0xF6,
        0xE4, '#', 0, '<', 'y', 'x', 'c', 'v', 'b', 'n',
        'm', ',', '.', '-', 0, '*', 0, ' ',
    },
    .shift = [_]u8{
        0, 0x1B, '!', '"', '#', '$', '%', '&', '/', '(',
        ')', '=', '?', '`', 0x08, 0x09, 'Q', 'W', 'E', 'R',
        'T', 'Z', 'U', 'I', 'O', 'P', 0xDC, '*', 0x0A, 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xD6,
        0xC4, '\'', 0, '>', 'Y', 'X', 'C', 'V', 'B', 'N',
        'M', ';', ':', '_', 0, '*', 0, ' ',
    },
    .altgr = [_]u8{
        0, 0, 0, 0, 0, 0, 0, '{', '[', ']',
        '}', 0, '\\', 0, 0, 0, '@', 0, 0, 0,
        0, 0xFC, 0, 0, 0, 0xF6, '~', 0, 0, 0,
        0, 0xDF, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, '|', 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    },
};

fn active_keymap() *const keymap_t {
    return switch (g_key_layout) {
        .us_qwerty => &KEYMAP_US,
        .de_nodeadkeys => &KEYMAP_DE_NODEADKEYS,
    };
}

fn scancode_to_ascii(scancode: u8, shifted: bool, altgr: bool) u8 {
    const km = active_keymap();
    if (scancode >= SCANCODE_MAP_LEN) return 0;
    if (altgr and km.altgr[scancode] != 0) return km.altgr[scancode];
    return if (shifted) km.shift[scancode] else km.plain[scancode];
}

fn prune_events_for_dead_endpoints() void {
    var i: usize = 0;
    while (i < g_events.len) : (i += 1) {
        const ep = g_events[i].endpoint;
        if (ep != IPC_ENDPOINT_NONE and !endpoint_alive(ep)) {
            g_events[i] = .{};
        }
    }
    while (g_event_head != g_event_tail and g_events[g_event_head].endpoint == IPC_ENDPOINT_NONE) {
        g_event_head = (g_event_head + 1) % g_events.len;
    }
    if (g_event_head == g_event_tail) {
        g_event_head = 0;
        g_event_tail = 0;
    }
}

fn cleanup_orphaned_state() void {
    var changed = false;
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (!g_windows[i].in_use) continue;
        if (endpoint_alive(g_windows[i].owner_endpoint)) continue;
        if (g_focused_window_id == g_windows[i].window_id) {
            g_focused_window_id = 0;
        }
        g_windows[i] = .{};
        changed = true;
    }

    i = 0;
    while (i < g_buffers.len) : (i += 1) {
        if (!g_buffers[i].in_use) continue;
        if (endpoint_alive(g_buffers[i].owner_endpoint)) continue;
        if (g_buffers[i].shmem_id != 0) {
            _ = api().shmem_unmap.?(g_buffers[i].shmem_id);
        }
        g_buffers[i] = .{};
        changed = true;
    }

    prune_events_for_dead_endpoints();
    if (changed) {
        sync_console_mode_for_windows();
        request_repaint_full();
    }
}

fn refresh_input_subscriptions_runtime() void {
    if (g_kbd_subscribed and !endpoint_alive(g_kbd_endpoint)) {
        g_kbd_subscribed = false;
        g_kbd_endpoint = IPC_ENDPOINT_NONE;
    }
    if (g_mouse_subscribed and !endpoint_alive(g_mouse_endpoint)) {
        g_mouse_subscribed = false;
        g_mouse_endpoint = IPC_ENDPOINT_NONE;
    }

    if (!g_kbd_subscribed) {
        if (g_kbd_endpoint == IPC_ENDPOINT_NONE or !endpoint_alive(g_kbd_endpoint)) {
            const ep = svc_lookup("kbd", g_runtime_lookup_req_id);
            g_runtime_lookup_req_id +%= 1;
            if (ep >= 0) g_kbd_endpoint = @bitCast(ep);
        }
        if (g_kbd_endpoint != IPC_ENDPOINT_NONE and subscribe_keyboard() != 0) {
            g_kbd_subscribed = false;
        }
    }
    if (!g_mouse_subscribed) {
        if (g_mouse_endpoint == IPC_ENDPOINT_NONE or !endpoint_alive(g_mouse_endpoint)) {
            const ep = svc_lookup("mouse", g_runtime_lookup_req_id);
            g_runtime_lookup_req_id +%= 1;
            if (ep >= 0) g_mouse_endpoint = @bitCast(ep);
        }
        if (g_mouse_endpoint != IPC_ENDPOINT_NONE and subscribe_mouse() != 0) {
            g_mouse_subscribed = false;
        }
    }
}

fn ipc_call(destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out: *c.nd_ipc_message_t) i32 {
    return ipc_call_budgeted(destination, request_id, msg_type, arg0, arg1, arg2, arg3, out, 1024);
}

fn ipc_call_budgeted(destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out: *c.nd_ipc_message_t, max_empty_polls: u32) i32 {
    const cb_store = struct {
        fn onResolve(user: ?*anyopaque, msg_raw: ?*const anyopaque) callconv(.c) void {
            const state: *struct { done: bool, resp: c.nd_ipc_message_t } = @ptrCast(@alignCast(user.?));
            const msg: *const c.nd_ipc_message_t = @ptrCast(@alignCast(msg_raw.?));
            state.done = true;
            state.resp = msg.*;
        }
    }.onResolve;
    var state = struct { done: bool, resp: c.nd_ipc_message_t }{ .done = false, .resp = undefined };
    if (sys.intentSendWithRequestId(&g_ipc_loop, destination, g_gfx_endpoint, request_id, msg_type, arg0, arg1, arg2, arg3, cb_store, @ptrCast(&state)) != 0) {
        return -1;
    }
    var empty_polls: u32 = 0;
    var total_polls: u32 = 0;
    const poll_limit = if (max_empty_polls == 0) 1 else max_empty_polls;
    // total_limit caps the loop even when continuous non-empty events (e.g. mouse
    // moves) keep preventing empty_polls from incrementing — without this, the
    // loop would spin forever as long as mouse input floods the queue.
    const total_limit: u32 = poll_limit * 64 + 256;
    while (!state.done) {
        const handled = sys.eventLoopPoll(&g_ipc_loop, 8);
        if (handled < 0) {
            sys.intentCancel(&g_ipc_loop, request_id);
            return -1;
        }
        total_polls +%= 1;
        if (handled == 0) {
            if (empty_polls >= poll_limit) {
                sys.intentCancel(&g_ipc_loop, request_id);
                return -1;
            }
            empty_polls +%= 1;
            api().sched_yield.?();
        }
        if (total_polls >= total_limit) {
            if (GFX_TRACE) { logMsg("[gfx-t] ipc_call_budgeted: total limit hit\n"); }
            sys.intentCancel(&g_ipc_loop, request_id);
            return -1;
        }
    }
    out.* = state.resp;
    return 0;
}

fn font_ipc_call_budgeted(destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out: *c.nd_ipc_message_t, max_empty_polls: u32) i32 {
    return ipc_call_budgeted(destination, request_id, msg_type, arg0, arg1, arg2, arg3, out, max_empty_polls);
}

fn log_fb_geometry_probe() void {
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 1;
    if (ipc_call(g_fb_endpoint, req_id, c.FBTEXT_IPC_GEOMETRY_REQ, 0, 0, 0, 0, &reply) != 0) {
        logMsg("[gfx] fb geometry probe failed\n");
        return;
    }
    if (reply.type == c.FBTEXT_IPC_RESP) {
        logMsg("[gfx] fb geometry cols/rows ok\n");
    }
}

fn log_fb_capabilities_probe() void {
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 10;
    if (ipc_call(g_fb_endpoint, req_id, c.FBTEXT_IPC_QUERY_CAPS_REQ, 0, 0, 0, 0, &reply) != 0) {
        return;
    }
    if (reply.type != c.FBTEXT_IPC_RESP) {
        return;
    }
    if ((reply.arg0 & c.FBTEXT_CAP_SET_RESOLUTION) != 0 and
        (reply.arg0 & c.FBTEXT_CAP_QUERY_MODES) != 0 and
        reply.arg1 != 0)
    {
        logMsg("[gfx] fb mode-switch ipc available\n");
        logHex32("[gfx] fb mode count=", reply.arg1);
        var idx: u32 = 0;
        const mode_count = if (reply.arg1 > 256) 256 else reply.arg1;
        while (idx < mode_count) : (idx += 1) {
            var mode_reply: c.nd_ipc_message_t = undefined;
            const mode_req_id: u32 = (GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 0x100) + idx;
            if (ipc_call(g_fb_endpoint, mode_req_id, c.FBTEXT_IPC_QUERY_MODES_REQ, idx, 0, 0, 0, &mode_reply) != 0 or
                mode_reply.type != c.FBTEXT_IPC_RESP)
            {
                break;
            }
            logModeResolution(mode_reply.arg0, mode_reply.arg1);
        }
    }
}

fn clamp_runtime_state_to_framebuffer() void {
    if (!g_fb_info_valid) return;
    const fb_w: i32 = @intCast(g_fb_info.framebuffer_width);
    const fb_h: i32 = @intCast(g_fb_info.framebuffer_height);
    const hi_x = if (fb_w > 0) fb_w - 1 else 0;
    const hi_y = if (fb_h > 0) fb_h - 1 else 0;
    g_pointer_x = clamp(g_pointer_x, 0, hi_x);
    g_pointer_y = clamp(g_pointer_y, 0, hi_y);

    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (!g_windows[i].in_use) continue;
        const win_w: i32 = @intCast(g_windows[i].width);
        const win_h: i32 = @intCast(g_windows[i].height);
        const max_w: i32 = if (fb_w > 1) fb_w else 1;
        const max_h: i32 = if (fb_h > 1) fb_h else 1;
        if (win_w > max_w) g_windows[i].width = @intCast(max_w);
        if (win_h > max_h) g_windows[i].height = @intCast(max_h);
        const clamped_win_w: i32 = @intCast(g_windows[i].width);
        const clamped_win_h: i32 = @intCast(g_windows[i].height);
        const pos_hi_x = if (fb_w > clamped_win_w) fb_w - clamped_win_w else 0;
        const pos_hi_y = if (fb_h > clamped_win_h) fb_h - clamped_win_h else 0;
        g_windows[i].x = clamp(g_windows[i].x, 0, pos_hi_x);
        g_windows[i].y = clamp(g_windows[i].y, 0, pos_hi_y);
    }
}

fn refresh_framebuffer_mapping() i32 {
    var info: c.nd_framebuffer_info_t = undefined;
    if (api().framebuffer_info.?(&info) != 0 or
        info.framebuffer_width == 0 or
        info.framebuffer_height == 0 or
        info.framebuffer_stride == 0)
    {
        return c.GFX_STATUS_IO;
    }
    const fb_size_u64: u64 = @as(u64, info.framebuffer_stride) * @as(u64, info.framebuffer_height) * 4;
    if (fb_size_u64 == 0 or fb_size_u64 > 0xFFFF_FFFF) {
        return c.GFX_STATUS_IO;
    }
    if (g_fb_pixels != null) {
        _ = api().buffer_release.?(c.ND_BUFFER_KIND_FRAMEBUFFER);
        g_fb_pixels = null;
    }
    const fb_ptr = api().buffer_borrow.?(c.ND_BUFFER_KIND_FRAMEBUFFER, 0, c.ND_BUFFER_BORROW_READ | c.ND_BUFFER_BORROW_WRITE, @intCast(fb_size_u64));
    if (fb_ptr == null) {
        return c.GFX_STATUS_IO;
    }
    g_fb_info = info;
    g_fb_info_valid = true;
    g_fb_pixels = @ptrCast(@alignCast(fb_ptr.?));
    clamp_runtime_state_to_framebuffer();
    return c.GFX_STATUS_OK;
}

fn ensure_backbuffer_capacity(required_bytes: u32) i32 {
    if (required_bytes == 0) return c.GFX_STATUS_IO;
    if (g_backbuffer_pixels != null and g_backbuffer_capacity_bytes >= required_bytes) return c.GFX_STATUS_OK;

    const required_pages: u64 = (@as(u64, required_bytes) + (PAGE_SIZE - 1)) / PAGE_SIZE;
    if (required_pages == 0) return c.GFX_STATUS_IO;

    var shmem_id: u32 = 0;
    var mapped_ptr: ?*anyopaque = null;
    if (api().shmem_create.?(required_pages, 0, &shmem_id, @ptrCast(&mapped_ptr)) != 0 or shmem_id == 0 or mapped_ptr == null) {
        logMsg("[gfx] backbuffer shmem_create failed\n");
        return c.GFX_STATUS_IO;
    }

    // TODO(gfx-backbuffer-lifetime): native driver ABI does not expose
    // shmem-destroy yet, so replaced backbuffers cannot be reclaimed.
    g_backbuffer_shmem_id = shmem_id;
    g_backbuffer_pixels = @ptrCast(@alignCast(mapped_ptr.?));
    g_backbuffer_capacity_bytes = required_bytes;
    return c.GFX_STATUS_OK;
}

fn try_switch_to_gfx_tty() void {
    if (g_vt_endpoint == IPC_ENDPOINT_NONE) return;
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 2;
    if (ipc_call(g_vt_endpoint, req_id, c.VT_IPC_SWITCH_TTY, 0, 0, 0, 0, &reply) == 0 and
        reply.type == c.VT_IPC_RESP)
    {
        logMsg("[gfx] switched to tty0 for compositor output\n");
    }
}

fn try_restore_cli_tty() void {
    if (g_vt_endpoint == IPC_ENDPOINT_NONE) return;
    if (GFX_TRACE) { logMsg("[gfx-t] try_restore_cli_tty ENTER\n"); }
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 3;
    if (ipc_call(g_vt_endpoint, req_id, c.VT_IPC_SWITCH_TTY, 1, 0, 0, 0, &reply) == 0 and
        reply.type == c.VT_IPC_RESP)
    {
        if (GFX_TRACE) { logMsg("[gfx-t] try_restore_cli_tty OK\n"); }
        logMsg("[gfx] restored tty1 for CLI\n");
    } else {
        if (GFX_TRACE) { logMsg("[gfx-t] try_restore_cli_tty FAILED\n"); }
    }
}

fn active_window_count() usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (g_windows[i].in_use) count += 1;
    }
    return count;
}

fn active_presented_window_count() usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (g_windows[i].in_use and g_windows[i].current_buffer_id != 0) count += 1;
    }
    return count;
}

fn event_push(endpoint: u32, event_type: u32, arg1: u32, arg2: u32, arg3: u32) void {
    const next_tail = (g_event_tail + 1) % g_events.len;
    if (next_tail == g_event_head) {
        g_event_head = (g_event_head + 1) % g_events.len;
    }
    g_events[g_event_tail] = .{
        .endpoint = endpoint,
        .event_type = event_type,
        .arg1 = arg1,
        .arg2 = arg2,
        .arg3 = arg3,
    };
    g_event_tail = next_tail;
}

fn event_pop_for(endpoint: u32, out: *gfx_event_t) bool {
    var i = g_event_head;
    while (i != g_event_tail) : (i = (i + 1) % g_events.len) {
        if (g_events[i].endpoint != endpoint) continue;
        out.* = g_events[i];
        g_events[i] = .{};
        while (g_event_head != g_event_tail and g_events[g_event_head].endpoint == IPC_ENDPOINT_NONE) {
            g_event_head = (g_event_head + 1) % g_events.len;
        }
        if (g_event_head == g_event_tail) {
            g_event_head = 0;
            g_event_tail = 0;
        }
        return true;
    }
    return false;
}

fn event_drop_pointer_for(endpoint: u32) void {
    var i: usize = 0;
    while (i < g_events.len) : (i += 1) {
        if (g_events[i].endpoint == endpoint and g_events[i].event_type == c.GFX_EVENT_POINTER) {
            g_events[i] = .{};
        }
    }
    while (g_event_head != g_event_tail and g_events[g_event_head].endpoint == IPC_ENDPOINT_NONE) {
        g_event_head = (g_event_head + 1) % g_events.len;
    }
    if (g_event_head == g_event_tail) {
        g_event_head = 0;
        g_event_tail = 0;
    }
}

fn event_drop_resize_for_window(endpoint: u32, window_id: u32) void {
    var i: usize = 0;
    while (i < g_events.len) : (i += 1) {
        if (g_events[i].endpoint == endpoint and
            g_events[i].event_type == c.GFX_EVENT_RESIZE and
            g_events[i].arg1 == window_id)
        {
            g_events[i] = .{};
        }
    }
    while (g_event_head != g_event_tail and g_events[g_event_head].endpoint == IPC_ENDPOINT_NONE) {
        g_event_head = (g_event_head + 1) % g_events.len;
    }
    if (g_event_head == g_event_tail) {
        g_event_head = 0;
        g_event_tail = 0;
    }
}

fn event_drop_for(endpoint: u32) void {
    var i: usize = 0;
    while (i < g_events.len) : (i += 1) {
        if (g_events[i].endpoint == endpoint) {
            g_events[i] = .{};
        }
    }
    while (g_event_head != g_event_tail and g_events[g_event_head].endpoint == IPC_ENDPOINT_NONE) {
        g_event_head = (g_event_head + 1) % g_events.len;
    }
    if (g_event_head == g_event_tail) {
        g_event_head = 0;
        g_event_tail = 0;
    }
}

fn focus_window(window_idx: usize) void {
    const win = g_windows[window_idx];
    if (!win.in_use) return;
    if (g_focused_window_id == win.window_id) return;

    if (g_focused_window_id != 0) {
        if (window_find_by_id(g_focused_window_id)) |old_idx| {
            const old = g_windows[old_idx];
            event_push(old.owner_endpoint, c.GFX_EVENT_FOCUS_LOST, old.window_id, 0, 0);
        }
    }

    g_focused_window_id = win.window_id;
    event_push(win.owner_endpoint, c.GFX_EVENT_FOCUS_GAINED, win.window_id, 0, 0);
}

fn blur_focused_window() bool {
    if (g_focused_window_id == 0) return false;
    if (window_find_by_id(g_focused_window_id)) |focused_idx| {
        const focused = g_windows[focused_idx];
        event_push(focused.owner_endpoint, c.GFX_EVENT_FOCUS_LOST, focused.window_id, 0, 0);
    }
    g_focused_window_id = 0;
    return true;
}

fn window_topmost_at(x: i32, y: i32) ?usize {
    var best: ?usize = null;
    var best_z: u32 = 0;
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (!g_windows[i].in_use) continue;
        const win = g_windows[i];
        const wx2 = win.x + @as(i32, @intCast(win.width));
        const wy2 = win.y + @as(i32, @intCast(win.height));
        if (x < win.x or y < win.y or x >= wx2 or y >= wy2) continue;
        if (best == null or win.z >= best_z) {
            best = i;
            best_z = win.z;
        }
    }
    return best;
}

fn raise_window(window_idx: usize) void {
    g_next_z +%= 1;
    if (g_next_z == 0) g_next_z = 1;
    g_windows[window_idx].z = g_next_z;
}

fn pack_s16_pair(dx: i32, dy: i32) u32 {
    const dx16: u16 = @bitCast(@as(i16, @truncate(dx)));
    const dy16: u16 = @bitCast(@as(i16, @truncate(dy)));
    return @as(u32, dx16) | (@as(u32, dy16) << 16);
}

fn pack_u16_pair(a: u32, b: u32) u32 {
    const a16: u16 = @intCast(a & 0xFFFF);
    const b16: u16 = @intCast(b & 0xFFFF);
    return @as(u32, a16) | (@as(u32, b16) << 16);
}

fn clamp(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

fn rect_union(a: c.gfx_rect_t, b: c.gfx_rect_t) c.gfx_rect_t {
    const x0 = if (a.x < b.x) a.x else b.x;
    const y0 = if (a.y < b.y) a.y else b.y;
    const a_x1 = a.x + a.w;
    const a_y1 = a.y + a.h;
    const b_x1 = b.x + b.w;
    const b_y1 = b.y + b.h;
    const x1 = if (a_x1 > b_x1) a_x1 else b_x1;
    const y1 = if (a_y1 > b_y1) a_y1 else b_y1;
    return .{ .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
}

fn request_repaint_rect(r: c.gfx_rect_t) void {
    if (r.w <= 0 or r.h <= 0) return;
    if (g_dirty_full) return;
    if (!g_dirty_pending) {
        g_dirty_rect = r;
        g_dirty_pending = true;
        return;
    }
    g_dirty_rect = rect_union(g_dirty_rect, r);
}

fn request_repaint_full() void {
    g_dirty_pending = true;
    g_dirty_full = true;
}

fn flush_repaint_if_pending() void {
    if (!g_dirty_pending) return;
    if (GFX_TRACE) {
        if (g_dirty_full) { logMsg("[gfx-t] repaint full\n"); } else { logMsg("[gfx-t] repaint rect\n"); }
    }
    if (g_dirty_full) {
        _ = compose_full();
    } else {
        _ = compose_region(g_dirty_rect);
    }
    g_dirty_pending = false;
    g_dirty_full = false;
    g_dirty_rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
}

fn pointer_update_position(dx: i32, dy: i32) void {
    if (!g_fb_info_valid) return;
    const max_x: i32 = @intCast(g_fb_info.framebuffer_width);
    const max_y: i32 = @intCast(g_fb_info.framebuffer_height);
    const hi_x = if (max_x > 0) max_x - 1 else 0;
    const hi_y = if (max_y > 0) max_y - 1 else 0;
    g_pointer_x = clamp(g_pointer_x + dx, 0, hi_x);
    g_pointer_y = clamp(g_pointer_y + dy, 0, hi_y);
}

fn handle_mouse_resize(dx: i32, dy: i32, left_down_now: bool) void {
    if (!left_down_now or g_resize_window_id == 0 or (dx == 0 and dy == 0)) return;
    if (window_find_by_id(g_resize_window_id)) |resize_idx| {
        const old_wr = rect_from_window(g_windows[resize_idx]);
        var new_w: i32 = @as(i32, @intCast(g_windows[resize_idx].width)) + dx;
        var new_h: i32 = @as(i32, @intCast(g_windows[resize_idx].height)) + dy;
        const min_w: i32 = @intCast(GFX_WINDOW_MIN_DIM);
        const min_h: i32 = @intCast(GFX_WINDOW_MIN_DIM);
        var max_w: i32 = @intCast(GFX_WINDOW_MAX_DIM);
        var max_h: i32 = @intCast(GFX_WINDOW_MAX_DIM);
        if (g_fb_info_valid) {
            const fb_w: i32 = @intCast(g_fb_info.framebuffer_width);
            const fb_h: i32 = @intCast(g_fb_info.framebuffer_height);
            const bound_w = fb_w - g_windows[resize_idx].x;
            const bound_h = fb_h - g_windows[resize_idx].y;
            if (bound_w < max_w) max_w = bound_w;
            if (bound_h < max_h) max_h = bound_h;
        }
        if (max_w < min_w) max_w = min_w;
        if (max_h < min_h) max_h = min_h;
        new_w = clamp(new_w, min_w, max_w);
        new_h = clamp(new_h, min_h, max_h);
        const old_w = g_windows[resize_idx].width;
        const old_h = g_windows[resize_idx].height;
        g_windows[resize_idx].width = @intCast(new_w);
        g_windows[resize_idx].height = @intCast(new_h);
        const new_wr = rect_from_window(g_windows[resize_idx]);
        request_repaint_rect(old_wr);
        request_repaint_rect(new_wr);
        if (g_windows[resize_idx].width != old_w or g_windows[resize_idx].height != old_h) {
            g_windows[resize_idx].is_maximized = false;
            const win = g_windows[resize_idx];
            event_push(win.owner_endpoint, c.GFX_EVENT_RESIZE, win.window_id, pack_u16_pair(win.width, win.height), 0);
        }
    } else {
        g_resize_window_id = 0;
    }
}

fn handle_mouse_drag(dx: i32, dy: i32, left_down_now: bool) void {
    if (!left_down_now or g_drag_window_id == 0 or (dx == 0 and dy == 0)) return;
    if (window_find_by_id(g_drag_window_id)) |drag_idx| {
        const old_wr = rect_from_window(g_windows[drag_idx]);
        if (g_fb_info_valid) {
            const max_x: i32 = @intCast(g_fb_info.framebuffer_width);
            const max_y: i32 = @intCast(g_fb_info.framebuffer_height);
            const ww: i32 = @intCast(g_windows[drag_idx].width);
            const wh: i32 = @intCast(g_windows[drag_idx].height);
            const hi_x = if (max_x > ww) max_x - ww else 0;
            const hi_y = if (max_y > wh) max_y - wh else 0;
            g_windows[drag_idx].x = clamp(g_windows[drag_idx].x + dx, 0, hi_x);
            g_windows[drag_idx].y = clamp(g_windows[drag_idx].y + dy, 0, hi_y);
        } else {
            g_windows[drag_idx].x += dx;
            g_windows[drag_idx].y += dy;
        }
        const new_wr = rect_from_window(g_windows[drag_idx]);
        request_repaint_rect(old_wr);
        request_repaint_rect(new_wr);
    } else {
        g_drag_window_id = 0;
    }
}

fn handle_mouse_press_transition(left_down_now: bool, left_down_prev: bool) void {
    if (!left_down_now or left_down_prev) return;
    if (window_topmost_at(g_pointer_x, g_pointer_y)) |idx| {
        raise_window(idx);
        focus_window(idx);
        if ((g_windows[idx].flags & GFX_WINDOW_FLAG_SYSTEM) != 0) {
            g_drag_window_id = 0;
            g_resize_window_id = 0;
        } else {
            const hit_close = point_in_rect(g_pointer_x, g_pointer_y, window_close_hit_rect(g_windows[idx]));
            const hit_max = point_in_rect(g_pointer_x, g_pointer_y, window_max_hit_rect(g_windows[idx]));
            const hit_resize = point_in_rect(g_pointer_x, g_pointer_y, window_resize_rect(g_windows[idx]));
            const hit_title = point_in_rect(g_pointer_x, g_pointer_y, window_title_rect(g_windows[idx]));
            if (hit_close) {
                const win = g_windows[idx];
                g_drag_window_id = 0;
                g_resize_window_id = 0;
                event_drop_for(win.owner_endpoint);
                event_push(win.owner_endpoint, c.GFX_EVENT_CLOSE_REQUEST, win.window_id, 0, 0);
            } else if (hit_max) {
                g_drag_window_id = 0;
                g_resize_window_id = 0;
                _ = try_toggle_maximize(idx);
            } else if (hit_resize) {
                g_resize_window_id = g_windows[idx].window_id;
                g_drag_window_id = 0;
            } else if (hit_title) {
                g_drag_window_id = g_windows[idx].window_id;
                g_resize_window_id = 0;
            }
        }
        request_repaint_full();
    } else {
        if (blur_focused_window()) {
            request_repaint_full();
        }
        g_drag_window_id = 0;
        g_resize_window_id = 0;
    }
}

fn maybe_emit_pointer_event(dx: i32, dy: i32, buttons: u32, prev_buttons: u32) void {
    if (g_focused_window_id == 0) return;
    if (window_find_by_id(g_focused_window_id)) |focused_idx| {
        const focused = g_windows[focused_idx];
        const cr = window_content_rect(focused);
        if (cr.w <= 0 or cr.h <= 0) return;
        const in_content = point_in_rect(g_pointer_x, g_pointer_y, cr);
        if (!in_content) {
            // Outside content: only deliver button-up transitions so the app can
            // reset its pressed state; suppress pure movement and button-down events.
            const any_button_released = (prev_buttons & ~buttons) != 0;
            if (!any_button_released) return;
        }
        if (dx == 0 and dy == 0 and buttons == prev_buttons) return;
        const rel_x = clamp(g_pointer_x - cr.x, 0, cr.w - 1);
        const rel_y = clamp(g_pointer_y - cr.y, 0, cr.h - 1);
        if (GFX_TRACE and (buttons & 0x1) != 0 and (prev_buttons & 0x1) == 0) {
            logMsg("[dbg-gfx] pointer btn-push queued\n");
        }
        event_push(focused.owner_endpoint, c.GFX_EVENT_POINTER, pack_u16_pair(@intCast(rel_x), @intCast(rel_y)), buttons, 0);
    }
}

fn handle_mouse_notify(msg: *const c.nd_ipc_message_t) void {
    const old_x = g_pointer_x;
    const old_y = g_pointer_y;
    const dx8: i8 = @bitCast(@as(u8, @truncate(msg.arg0)));
    const dy8: i8 = @bitCast(@as(u8, @truncate(msg.arg1)));
    const dx: i32 = @as(i32, dx8);
    const dy: i32 = @as(i32, dy8);
    const buttons: u32 = msg.arg2 & 0x7;

    pointer_update_position(dx, dy);

    if (g_overlay_locked and (old_x != g_pointer_x or old_y != g_pointer_y)) {
        if (GFX_TRACE) { logMsg("[gfx-t] cursor move\n"); }
        request_repaint_rect(cursor_rect_at(old_x, old_y));
        request_repaint_rect(cursor_rect_at(g_pointer_x, g_pointer_y));
    }

    const prev_buttons = g_pointer_buttons;
    const left_down_now = (buttons & 0x1) != 0;
    const left_down_prev = (prev_buttons & 0x1) != 0;

    if (!left_down_now and left_down_prev) {
        g_drag_window_id = 0;
        g_resize_window_id = 0;
    }

    handle_mouse_resize(dx, dy, left_down_now);
    handle_mouse_drag(dx, dy, left_down_now);
    handle_mouse_press_transition(left_down_now, left_down_prev);
    g_pointer_buttons = buttons;
    maybe_emit_pointer_event(dx, dy, buttons, prev_buttons);
}

fn fb_set_overlay_lock(lock: bool) void {
    if (g_fb_endpoint == IPC_ENDPOINT_NONE) return;
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 9;
    _ = ipc_call(g_fb_endpoint, req_id, c.FBTEXT_IPC_GFX_OVERLAY_REQ, if (lock) 1 else 0, 0, 0, 0, &reply);
}

fn sync_console_mode_for_windows() void {
    const cnt = active_presented_window_count();
    const has_presented_windows = cnt > 0;
    if (has_presented_windows and !g_overlay_locked) {
        if (GFX_TRACE) { logMsg("[gfx-t] sync: lock\n"); }
        try_switch_to_gfx_tty();
        fb_set_overlay_lock(true);
        g_overlay_locked = true;
        request_repaint_full();
        return;
    }
    if (!has_presented_windows and g_overlay_locked) {
        if (GFX_TRACE) { logMsg("[gfx-t] sync: restore cnt=0 locked=true\n"); }
        fb_set_overlay_lock(false);
        g_overlay_locked = false;
        try_restore_cli_tty();
    }
}

fn reply_with_status(msg: *const c.nd_ipc_message_t, status: i32, arg1: u32, arg2: u32, arg3: u32) void {
    var resp: c.nd_ipc_message_t = undefined;
    if (msg.source == IPC_ENDPOINT_NONE or msg.request_id == 0) return;
    resp.type = c.GFX_IPC_RESP;
    resp.source = g_gfx_endpoint;
    resp.destination = msg.source;
    resp.request_id = msg.request_id;
    resp.arg0 = @bitCast(status);
    resp.arg1 = arg1;
    resp.arg2 = arg2;
    resp.arg3 = arg3;
    // Retry until delivered: a dropped reply leaves the client blocked forever.
    // The client is blocked in wasmos_ipc_select_one so its endpoint is empty
    // and the send should succeed quickly; the loop is a safety net for any
    // transient IPC_ERR_FULL burst.
    var tries: u32 = 0;
    while (api().ipc_send.?(ctxId(), msg.source, &resp) != 0) {
        tries +%= 1;
        if (GFX_TRACE and tries == 64) {
            logMsg("[gfx-t] reply_with_status: 64 retries — may hang\n");
        }
        api().sched_yield.?();
        if (tries > 4096) break; // last-resort guard against truly broken endpoints
    }
}

fn reply_unsupported(msg: *const c.nd_ipc_message_t) void {
    reply_with_status(msg, c.GFX_STATUS_UNSUPPORTED, 0, 0, 0);
}

fn gfx_header_valid(magic: u32, ver_opcode: u32) bool {
    const version: u16 = @intCast(ver_opcode >> 16);
    return magic == c.GFX_IPC_ABI_MAGIC and version == c.GFX_IPC_ABI_VERSION;
}

fn gfx_header_opcode(ver_opcode: u32) u16 {
    return @intCast(ver_opcode & 0xFFFF);
}

fn window_find_by_id(id: u32) ?usize {
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (g_windows[i].in_use and g_windows[i].window_id == id) return i;
    }
    return null;
}

fn window_alloc(owner_endpoint: u32, width: u32, height: u32) ?usize {
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (g_windows[i].in_use) continue;
        g_windows[i].in_use = true;
        g_windows[i].owner_endpoint = owner_endpoint;
        g_windows[i].window_id = g_next_window_id;
        g_windows[i].width = width;
        g_windows[i].height = height;
        g_windows[i].z = g_next_z;
        g_windows[i].generation = 1;
        g_windows[i].current_buffer_id = 0;
        g_title_run_cache[i] = .{};

        if (g_fb_info_valid) {
            const off = @as(i32, @intCast((g_windows[i].z & 0xF) * 20));
            const max_x: i32 = @intCast(g_fb_info.framebuffer_width);
            const max_y: i32 = @intCast(g_fb_info.framebuffer_height);
            g_windows[i].x = @min(24 + off, if (max_x > 1) max_x - 1 else 0);
            g_windows[i].y = @min(24 + off, if (max_y > 1) max_y - 1 else 0);
        } else {
            g_windows[i].x = 16;
            g_windows[i].y = 16;
        }

        g_next_window_id +%= 1;
        if (g_next_window_id == 0) g_next_window_id = 1;
        g_next_z +%= 1;
        if (g_next_z == 0) g_next_z = 1;
        return i;
    }
    return null;
}

fn buffer_find_by_id(buffer_id: u32) ?usize {
    var i: usize = 0;
    while (i < g_buffers.len) : (i += 1) {
        if (g_buffers[i].in_use and g_buffers[i].buffer_id == buffer_id) return i;
    }
    return null;
}

fn detach_buffer_from_windows(buffer_id: u32) bool {
    var changed = false;
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (!g_windows[i].in_use) continue;
        if (g_windows[i].current_buffer_id == buffer_id) {
            g_windows[i].current_buffer_id = 0;
            changed = true;
        }
    }
    return changed;
}

fn buffer_generate_id() ?u32 {
    var attempts: u32 = 0;
    while (attempts < 64) : (attempts += 1) {
        const id = rand_u32();
        if (id == 0) continue;
        if (buffer_find_by_id(id) == null) return id;
    }
    return null;
}

fn buffer_alloc(owner_endpoint: u32, width: u32, height: u32) ?usize {
    var slot_idx: ?usize = null;
    var i: usize = 0;
    while (i < g_buffers.len) : (i += 1) {
        if (!g_buffers[i].in_use) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx == null) return null;

    const stride_u64 = @as(u64, width) * 4;
    const bytes_u64 = stride_u64 * @as(u64, height);
    if (stride_u64 == 0 or bytes_u64 == 0 or stride_u64 > 0xFFFF_FFFF or bytes_u64 > 0xFFFF_FFFF) {
        return null;
    }
    const pages = (bytes_u64 + (PAGE_SIZE - 1)) / PAGE_SIZE;
    if (pages == 0) return null;

    const buffer_id = buffer_generate_id() orelse return null;
    var shmem_id: u32 = 0;
    var mapped_ptr: ?*anyopaque = null;
    if (api().shmem_create.?(pages, 0, &shmem_id, @ptrCast(&mapped_ptr)) != 0 or shmem_id == 0 or mapped_ptr == null) {
        logMsg("[gfx] alloc shmem_create failed\n");
        return null;
    }
    var owner_context_id: u32 = 0;
    if (api().ipc_endpoint_owner == null or
        api().ipc_endpoint_owner.?(owner_endpoint, &owner_context_id) != 0)
    {
        logMsg("[gfx] alloc endpoint_owner failed\n");
        return null;
    }
    if (owner_context_id != 0) {
        if (api().shmem_grant == null or
            api().shmem_grant.?(shmem_id, owner_context_id) != 0)
        {
            logMsg("[gfx] alloc shmem_grant failed\n");
            return null;
        }
    }

    const idx = slot_idx.?;
    g_buffers[idx].in_use = true;
    g_buffers[idx].owner_endpoint = owner_endpoint;
    g_buffers[idx].buffer_id = buffer_id;
    g_buffers[idx].shmem_id = shmem_id;
    g_buffers[idx].width = width;
    g_buffers[idx].height = height;
    g_buffers[idx].stride_bytes = @intCast(stride_u64);
    g_buffers[idx].state = .allocated;
    g_buffers[idx].bound_window_id = 0;
    g_buffers[idx].mapped_pixels = @ptrCast(@alignCast(mapped_ptr.?));
    g_buffers[idx].bound_window_generation = 0;
    return idx;
}

fn window_buffer_in_use(buffer_id: u32) bool {
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (g_windows[i].in_use and g_windows[i].current_buffer_id == buffer_id) return true;
    }
    return false;
}

fn fill_rect(x0: i32, y0: i32, w: i32, h: i32, color: u32) void {
    if (!g_fb_info_valid or g_backbuffer_pixels == null or w <= 0 or h <= 0) return;
    var sx = x0;
    var sy = y0;
    var ex = x0 + w;
    var ey = y0 + h;
    const max_x: i32 = @intCast(g_fb_info.framebuffer_width);
    const max_y: i32 = @intCast(g_fb_info.framebuffer_height);
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (ex > max_x) ex = max_x;
    if (ey > max_y) ey = max_y;
    if (sx >= ex or sy >= ey) return;

    const stride: usize = @intCast(g_fb_info.framebuffer_stride);
    const fb = g_backbuffer_pixels.?;
    const row_w: usize = @intCast(ex - sx);
    var y = sy;
    while (y < ey) : (y += 1) {
        const row_base: usize = @as(usize, @intCast(y)) * stride + @as(usize, @intCast(sx));
        var col: usize = 0;
        while (col + 4 <= row_w) : (col += 4) {
            fb[row_base + col]     = color;
            fb[row_base + col + 1] = color;
            fb[row_base + col + 2] = color;
            fb[row_base + col + 3] = color;
        }
        while (col < row_w) : (col += 1) {
            fb[row_base + col] = color;
        }
    }
}

fn rect_intersects(a: c.gfx_rect_t, b: c.gfx_rect_t) bool {
    const a_x2 = a.x + a.w;
    const a_y2 = a.y + a.h;
    const b_x2 = b.x + b.w;
    const b_y2 = b.y + b.h;
    return a.x < b_x2 and b.x < a_x2 and a.y < b_y2 and b.y < a_y2;
}

fn rect_from_window(slot: window_slot_t) c.gfx_rect_t {
    return .{ .x = slot.x, .y = slot.y, .w = @intCast(slot.width), .h = @intCast(slot.height) };
}

fn cursor_rect_at(x: i32, y: i32) c.gfx_rect_t {
    return .{ .x = x, .y = y, .w = CURSOR_W, .h = CURSOR_H };
}

fn draw_cursor_overlay(region: c.gfx_rect_t) void {
    if (!g_fb_info_valid or g_backbuffer_pixels == null) return;
    const cr = cursor_rect_at(g_pointer_x, g_pointer_y);
    if (!rect_intersects(region, cr)) {
        if (GFX_TRACE) { logMsg("[gfx-t] cursor no-intersect\n"); }
        return;
    }

    var y: i32 = 0;
    while (y < CURSOR_H) : (y += 1) {
        var x: i32 = 0;
        while (x < CURSOR_W) : (x += 1) {
            if (x > y) continue;
            const px = g_pointer_x + x;
            const py = g_pointer_y + y;
            if (px < region.x or py < region.y or px >= region.x + region.w or py >= region.y + region.h) continue;
            const edge = (x == 0 or y == 0 or x == y);
            const color: u32 = if (edge) 0xFF000000 else 0xFFFFFFFF;
            fill_rect(px, py, 1, 1, color);
        }
    }
}

fn draw_window_placeholder(win: window_slot_t, clip: c.gfx_rect_t) void {
    const tone = @as(u32, (win.window_id * 37) & 0x7F);
    const body_color = 0x203040 | (tone << 16) | ((tone >> 1) << 8);
    fill_rect(clip.x, clip.y, clip.w, clip.h, body_color);
}

fn window_close_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    const close_y = win.y + (CHROME_TITLE_H - CHROME_CLOSE_SZ) / 2;
    return .{
        .x = win.x + ww - CHROME_CLOSE_PAD - CHROME_CLOSE_SZ,
        .y = close_y,
        .w = CHROME_CLOSE_SZ,
        .h = CHROME_CLOSE_SZ,
    };
}

fn window_max_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    const max_y = win.y + (CHROME_TITLE_H - CHROME_MAX_SZ) / 2;
    return .{
        .x = win.x + ww - CHROME_CLOSE_PAD - CHROME_CLOSE_SZ - CHROME_BTN_GAP - CHROME_MAX_PAD - CHROME_MAX_SZ,
        .y = max_y,
        .w = CHROME_MAX_SZ,
        .h = CHROME_MAX_SZ,
    };
}

fn window_close_hit_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    // Starts at the left edge of the inter-button gap so the zone never
    // overlaps the visual maximize button, regardless of padding constants.
    const hit_x = win.x + ww - CHROME_CLOSE_PAD - CHROME_CLOSE_SZ - CHROME_BTN_GAP;
    return .{
        .x = hit_x,
        .y = win.y,
        .w = (win.x + ww) - hit_x,
        .h = CHROME_TITLE_H,
    };
}

fn window_max_hit_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    const close_x = win.x + ww - CHROME_CLOSE_PAD - CHROME_CLOSE_SZ - CHROME_BTN_GAP;
    return .{
        .x = close_x - CHROME_MAX_HIT_W,
        .y = win.y,
        .w = CHROME_MAX_HIT_W,
        .h = CHROME_TITLE_H,
    };
}

fn window_title_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    const close_x = ww - CHROME_CLOSE_PAD - CHROME_CLOSE_SZ - CHROME_BTN_GAP;
    const title_w = close_x - CHROME_MAX_HIT_W - CHROME_BORDER;
    return .{
        .x = win.x + CHROME_BORDER,
        .y = win.y,
        .w = if (title_w > 0) title_w else 0,
        .h = CHROME_TITLE_H,
    };
}

fn window_resize_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    const wh: i32 = @intCast(win.height);
    return .{
        .x = win.x + ww - CHROME_RESIZE_HANDLE_SZ,
        .y = win.y + wh - CHROME_RESIZE_HANDLE_SZ,
        .w = CHROME_RESIZE_HANDLE_SZ,
        .h = CHROME_RESIZE_HANDLE_SZ,
    };
}

fn window_content_rect(win: window_slot_t) c.gfx_rect_t {
    if ((win.flags & GFX_WINDOW_FLAG_SYSTEM) != 0) {
        return .{ .x = win.x, .y = win.y, .w = @intCast(win.width), .h = @intCast(win.height) };
    }
    const ww: i32 = @intCast(win.width);
    const wh: i32 = @intCast(win.height);
    const cw = ww - (CHROME_BORDER * 2);
    const ch = wh - CHROME_TITLE_H - CHROME_BORDER;
    return .{
        .x = win.x + CHROME_BORDER,
        .y = win.y + CHROME_TITLE_H,
        .w = if (cw > 0) cw else 0,
        .h = if (ch > 0) ch else 0,
    };
}

fn point_in_rect(x: i32, y: i32, r: c.gfx_rect_t) bool {
    return x >= r.x and y >= r.y and x < r.x + r.w and y < r.y + r.h;
}

fn draw_window_chrome(win: window_slot_t, clip: c.gfx_rect_t, focused: bool) void {
    const wr = rect_from_window(win);
    if (!rect_intersects(clip, wr)) return;

    const border_color: u32 = if (focused) 0xFF7EC8FF else 0xFF5A6A7A;
    const title_color: u32 = if (focused) 0xFF173A53 else 0xFF202A33;
    const close_bg: u32 = 0xFFC43A3A;
    const close_fg: u32 = 0xFFFFFFFF;
    const max_bg: u32 = 0xFF3E7D46;
    const max_fg: u32 = 0xFFFFFFFF;

    fill_rect(win.x, win.y, @intCast(win.width), CHROME_BORDER, border_color);
    fill_rect(win.x, win.y, CHROME_BORDER, @intCast(win.height), border_color);
    fill_rect(win.x + @as(i32, @intCast(win.width)) - CHROME_BORDER, win.y, CHROME_BORDER, @intCast(win.height), border_color);
    fill_rect(win.x, win.y + @as(i32, @intCast(win.height)) - CHROME_BORDER, @intCast(win.width), CHROME_BORDER, border_color);
    const ww: i32 = @intCast(win.width);
    fill_rect(win.x + CHROME_BORDER, win.y + CHROME_BORDER, ww - (CHROME_BORDER * 2), CHROME_TITLE_H - CHROME_BORDER, title_color);

    const cr = window_close_rect(win);
    const mr = window_max_rect(win);
    fill_rect(cr.x, cr.y, cr.w, cr.h, close_bg);
    fill_rect(cr.x + 2, cr.y + 2, cr.w - 4, 1, close_fg);
    fill_rect(cr.x + 2, cr.y + cr.h - 3, cr.w - 4, 1, close_fg);
    fill_rect(cr.x + 2, cr.y + 2, 1, cr.h - 4, close_fg);
    fill_rect(cr.x + cr.w - 3, cr.y + 2, 1, cr.h - 4, close_fg);

    fill_rect(mr.x, mr.y, mr.w, mr.h, max_bg);
    if (!win.is_maximized) {
        fill_rect(mr.x + 2, mr.y + 2, mr.w - 4, mr.h - 4, max_fg);
        fill_rect(mr.x + 3, mr.y + 3, mr.w - 6, mr.h - 6, max_bg);
    } else {
        fill_rect(mr.x + 2, mr.y + 3, mr.w - 4, mr.h - 4, max_fg);
        fill_rect(mr.x + 3, mr.y + 2, mr.w - 4, mr.h - 4, max_fg);
        fill_rect(mr.x + 4, mr.y + 4, mr.w - 6, mr.h - 6, max_bg);
    }

    if (GFX_TITLE_TEXT_ENABLED) {
        draw_window_title_text(win, clip);
    }
}

fn resize_window_and_notify(window_idx: usize, x: i32, y: i32, width: u32, height: u32) void {
    const old_wr = rect_from_window(g_windows[window_idx]);
    g_windows[window_idx].generation +%= 1;
    if (g_windows[window_idx].generation == 0) g_windows[window_idx].generation = 1;
    g_windows[window_idx].current_buffer_id = 0;
    g_windows[window_idx].x = x;
    g_windows[window_idx].y = y;
    g_windows[window_idx].width = width;
    g_windows[window_idx].height = height;
    const new_wr = rect_from_window(g_windows[window_idx]);
    request_repaint_rect(old_wr);
    request_repaint_rect(new_wr);
    const win = g_windows[window_idx];
    event_drop_resize_for_window(win.owner_endpoint, win.window_id);
    event_push(win.owner_endpoint, c.GFX_EVENT_RESIZE, win.window_id, pack_u16_pair(win.width, win.height), 0);
}

fn try_toggle_maximize(window_idx: usize) bool {
    if (!g_fb_info_valid) return false;
    const fb_w = g_fb_info.framebuffer_width;
    const fb_h = g_fb_info.framebuffer_height;
    if (fb_w == 0 or fb_h == 0) return false;

    if (!g_windows[window_idx].is_maximized) {
        g_windows[window_idx].restore_x = g_windows[window_idx].x;
        g_windows[window_idx].restore_y = g_windows[window_idx].y;
        g_windows[window_idx].restore_w = g_windows[window_idx].width;
        g_windows[window_idx].restore_h = g_windows[window_idx].height;
        g_windows[window_idx].is_maximized = true;
        resize_window_and_notify(window_idx, 0, 0, fb_w, fb_h);
        return true;
    }

    var restore_w = g_windows[window_idx].restore_w;
    var restore_h = g_windows[window_idx].restore_h;
    if (restore_w < GFX_WINDOW_MIN_DIM or restore_h < GFX_WINDOW_MIN_DIM) return false;
    if (restore_w > GFX_WINDOW_MAX_DIM) restore_w = GFX_WINDOW_MAX_DIM;
    if (restore_h > GFX_WINDOW_MAX_DIM) restore_h = GFX_WINDOW_MAX_DIM;
    if (restore_w > fb_w) restore_w = fb_w;
    if (restore_h > fb_h) restore_h = fb_h;

    const max_x: i32 = if (fb_w > restore_w) @as(i32, @intCast(fb_w - restore_w)) else 0;
    const max_y: i32 = if (fb_h > restore_h) @as(i32, @intCast(fb_h - restore_h)) else 0;
    const restore_x = clamp(g_windows[window_idx].restore_x, 0, max_x);
    const restore_y = clamp(g_windows[window_idx].restore_y, 0, max_y);
    g_windows[window_idx].is_maximized = false;
    resize_window_and_notify(window_idx, restore_x, restore_y, restore_w, restore_h);
    return true;
}

fn blend_u8(dst: u32, src: u32, alpha: u8) u32 {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    const a: u32 = alpha;
    const inv: u32 = 255 - a;
    const dr: u32 = (dst >> 16) & 0xFF;
    const dg: u32 = (dst >> 8) & 0xFF;
    const db: u32 = dst & 0xFF;
    const sr: u32 = (src >> 16) & 0xFF;
    const sg: u32 = (src >> 8) & 0xFF;
    const sb: u32 = src & 0xFF;
    const r: u32 = (sr * a + dr * inv) / 255;
    const g: u32 = (sg * a + dg * inv) / 255;
    const b: u32 = (sb * a + db * inv) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

fn draw_glyph_mask(dst_x: i32, dst_y: i32, glyph_w: i32, glyph_h: i32, mask: [*]const u8, clip: c.gfx_rect_t, color: u32) void {
    if (g_backbuffer_pixels == null or !g_fb_info_valid) return;
    const fb = g_backbuffer_pixels.?;
    const stride: usize = @intCast(g_fb_info.framebuffer_stride);
    var y: i32 = 0;
    while (y < glyph_h) : (y += 1) {
        const py = dst_y + y;
        if (py < clip.y or py >= clip.y + clip.h) continue;
        if (py < 0 or py >= @as(i32, @intCast(g_fb_info.framebuffer_height))) continue;
        var x: i32 = 0;
        while (x < glyph_w) : (x += 1) {
            const px = dst_x + x;
            if (px < clip.x or px >= clip.x + clip.w) continue;
            if (px < 0 or px >= @as(i32, @intCast(g_fb_info.framebuffer_width))) continue;
            const a = mask[@as(usize, @intCast(y * glyph_w + x))];
            if (a == 0) continue;
            const idx: usize = @as(usize, @intCast(py)) * stride + @as(usize, @intCast(px));
            fb[idx] = blend_u8(fb[idx], color, a);
        }
    }
}

fn glyph_cache_lookup(codepoint: u32) ?usize {
    var i: usize = 0;
    while (i < g_glyph_cache.len) : (i += 1) {
        if (g_glyph_cache[i].valid and g_glyph_cache[i].codepoint == codepoint) return i;
    }
    return null;
}

fn glyph_cache_slot_for_new() ?usize {
    var i: usize = 0;
    while (i < g_glyph_cache.len) : (i += 1) {
        if (!g_glyph_cache[i].valid) return i;
    }
    return null;
}

fn ensure_font_shmem_buffer(shmem_id: *u32, mapped_ptr: *?[*]u8, cap: *usize, need_bytes: usize) bool {
    if (need_bytes == 0) return false;
    if (shmem_id.* != 0 and mapped_ptr.* != null and cap.* >= need_bytes) return true;
    var new_id: u32 = 0;
    var raw: ?*anyopaque = null;
    const pages: usize = (need_bytes + 4095) / 4096;
    if (api().shmem_create.?(pages, 0, &new_id, @ptrCast(&raw)) != 0 or new_id == 0 or raw == null) return false;
    // TODO(gfx-font-shmem): old SHMEM IDs are not reclaimed on growth.
    shmem_id.* = new_id;
    mapped_ptr.* = @ptrCast(@alignCast(raw.?));
    cap.* = pages * 4096;
    return true;
}

fn font_measure_and_raster_text(text: []const u8, out_w: *i32, out_h: *i32, out_x0: *i16, out_y0: *i16, out_adv: *i32) bool {
    if (g_font_endpoint == IPC_ENDPOINT_NONE or g_font_title_handle == 0) return false;
    if (text.len == 0) return false;
    if (!ensure_font_shmem_buffer(&g_font_text_shmem_id, &g_font_text_ptr, &g_font_text_cap, text.len + 1)) return false;
    var i: usize = 0;
    while (i < text.len) : (i += 1) g_font_text_ptr.?[i] = text[i];
    g_font_text_ptr.?[text.len] = 0;

    var reply: c.nd_ipc_message_t = undefined;
    const req_id_measure = g_runtime_lookup_req_id;
    g_runtime_lookup_req_id +%= 1;
    if (font_ipc_call_budgeted(g_font_endpoint, req_id_measure, c.FONT_IPC_MEASURE_GLYPH_REQ, g_font_title_handle, g_font_text_shmem_id, @intCast(text.len), 0, &reply, 32) != 0) {
        return false;
    }
    if (reply.type != c.FONT_IPC_RESP or @as(i32, @bitCast(reply.arg0)) != c.FONT_STATUS_OK) {
        return false;
    }
    const packed_wh = reply.arg1;
    const packed_xy = reply.arg2;
    const w: i32 = @as(i32, @intCast(packed_wh & 0xFFFF));
    const h: i32 = @as(i32, @intCast((packed_wh >> 16) & 0xFFFF));
    const x0: i16 = @bitCast(@as(u16, @truncate(packed_xy & 0xFFFF)));
    const y0: i16 = @bitCast(@as(u16, @truncate((packed_xy >> 16) & 0xFFFF)));
    const adv: i32 = @bitCast(reply.arg3);

    out_w.* = w;
    out_h.* = h;
    out_x0.* = x0;
    out_y0.* = y0;
    out_adv.* = adv;

    if (w <= 0 or h <= 0) return true;
    const pixel_count_i32 = w * h;
    if (pixel_count_i32 <= 0) return false;
    const pixel_count: usize = @intCast(pixel_count_i32);
    if (pixel_count > GFX_MAX_GLYPH_BYTES) return false;
    if (!ensure_font_shmem_buffer(&g_font_mask_shmem_id, &g_font_mask_ptr, &g_font_mask_cap, pixel_count)) return false;

    const req_id_into = g_runtime_lookup_req_id;
    g_runtime_lookup_req_id +%= 1;
    if (font_ipc_call_budgeted(g_font_endpoint, req_id_into, c.FONT_IPC_RASTER_GLYPH_INTO_REQ, g_font_title_handle, g_font_text_shmem_id, @intCast(text.len), g_font_mask_shmem_id, &reply, 32) != 0) {
        return false;
    }
    if (reply.type != c.FONT_IPC_RESP or @as(i32, @bitCast(reply.arg0)) != c.FONT_STATUS_OK) {
        return false;
    }
    return true;
}

fn build_window_title_label(window_id: u32, out: *[GFX_MAX_TITLE_LABEL]u8) usize {
    const prefix = "win ";
    var n: usize = 0;
    while (n < prefix.len) : (n += 1) out[n] = prefix[n];
    var id = window_id;
    var tmp: [10]u8 = undefined;
    var digits: usize = 0;
    while (true) {
        tmp[digits] = @intCast('0' + (id % 10));
        digits += 1;
        id /= 10;
        if (id == 0 or digits == tmp.len) break;
    }
    var d: usize = 0;
    while (d < digits and n < out.len) : (d += 1) {
        out[n] = tmp[digits - 1 - d];
        n += 1;
    }
    return n;
}

fn title_cache_valid_for_window(slot_idx: usize, win: window_slot_t, label: []const u8) bool {
    const e = g_title_run_cache[slot_idx];
    if (!e.valid) return false;
    if (e.window_id != win.window_id or e.window_generation != win.generation) return false;
    if (e.label_len != label.len) return false;
    var i: usize = 0;
    while (i < label.len) : (i += 1) {
        if (e.label[i] != label[i]) return false;
    }
    return true;
}

fn refresh_title_cache(slot_idx: usize, win: window_slot_t, label: []const u8) bool {
    var w: i32 = 0;
    var h: i32 = 0;
    var x0: i16 = 0;
    var y0: i16 = 0;
    var adv: i32 = 0;
    if (!font_measure_and_raster_text(label, &w, &h, &x0, &y0, &adv)) return false;

    var entry = title_run_cache_entry_t{
        .valid = true,
        .window_id = win.window_id,
        .window_generation = win.generation,
        .label_len = label.len,
        .w = w,
        .h = h,
        .x0 = x0,
        .y0 = y0,
        .advance_x = adv,
        .mask_len = 0,
    };
    var i: usize = 0;
    while (i < label.len) : (i += 1) entry.label[i] = label[i];

    if (w > 0 and h > 0) {
        const pixel_count_i32 = w * h;
        if (pixel_count_i32 <= 0) return false;
        const pixel_count: usize = @intCast(pixel_count_i32);
        if (pixel_count > entry.mask_data.len or g_font_mask_ptr == null) return false;
        const src = g_font_mask_ptr.?;
        var j: usize = 0;
        while (j < pixel_count) : (j += 1) entry.mask_data[j] = src[j];
        entry.mask_len = pixel_count;
    }
    g_title_run_cache[slot_idx] = entry;
    return true;
}

fn codepoint_to_utf8(cp: u32, buf: [*]u8) usize {
    if (cp < 0x80) {
        buf[0] = @intCast(cp);
        return 1;
    } else if (cp < 0x800) {
        buf[0] = @intCast(0xC0 | (cp >> 6));
        buf[1] = @intCast(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = @intCast(0xE0 | (cp >> 12));
        buf[1] = @intCast(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = @intCast(0x80 | (cp & 0x3F));
        return 3;
    } else {
        buf[0] = @intCast(0xF0 | (cp >> 18));
        buf[1] = @intCast(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = @intCast(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = @intCast(0x80 | (cp & 0x3F));
        return 4;
    }
}

fn glyph_cache_insert_from_font(codepoint: u32) bool {
    if (glyph_cache_lookup(codepoint) != null) return true;
    if (g_font_endpoint == IPC_ENDPOINT_NONE or g_font_title_handle == 0) return false;
    const slot = glyph_cache_slot_for_new() orelse return false;
    if (!ensure_font_shmem_buffer(&g_font_text_shmem_id, &g_font_text_ptr, &g_font_text_cap, 5)) return false;
    const utf8_len = codepoint_to_utf8(codepoint, g_font_text_ptr.?);
    g_font_text_ptr.?[utf8_len] = 0;

    var reply: c.nd_ipc_message_t = undefined;
    const req_id = g_runtime_lookup_req_id;
    g_runtime_lookup_req_id +%= 1;
    if (font_ipc_call_budgeted(g_font_endpoint, req_id, c.FONT_IPC_MEASURE_GLYPH_REQ, g_font_title_handle, g_font_text_shmem_id, @intCast(utf8_len), 0, &reply, 32) != 0) {
        return false;
    }
    if (reply.type != c.FONT_IPC_RESP or @as(i32, @bitCast(reply.arg0)) != c.FONT_STATUS_OK) {
        return false;
    }

    const packed_wh = reply.arg1;
    const packed_xy = reply.arg2;
    const w: i32 = @as(i32, @intCast(packed_wh & 0xFFFF));
    const h: i32 = @as(i32, @intCast((packed_wh >> 16) & 0xFFFF));
    const x0: i16 = @bitCast(@as(u16, @truncate(packed_xy & 0xFFFF)));
    const y0: i16 = @bitCast(@as(u16, @truncate((packed_xy >> 16) & 0xFFFF)));
    var entry = glyph_cache_entry_t{
        .valid = true,
        .codepoint = codepoint,
        .shmem_id = 0,
        .w = w,
        .h = h,
        .x0 = x0,
        .y0 = y0,
        .mask_len = 0,
    };
    if (w > 0 and h > 0) {
        const pixel_count_i32 = w * h;
        if (pixel_count_i32 < 0) {
            return false;
        }
        const pixel_count: usize = @intCast(pixel_count_i32);
        if (pixel_count > GFX_MAX_GLYPH_BYTES) {
            return false;
        }
        if (!ensure_font_shmem_buffer(&g_font_mask_shmem_id, &g_font_mask_ptr, &g_font_mask_cap, pixel_count)) return false;
        const req_id_into = g_runtime_lookup_req_id;
        g_runtime_lookup_req_id +%= 1;
        if (font_ipc_call_budgeted(g_font_endpoint, req_id_into, c.FONT_IPC_RASTER_GLYPH_INTO_REQ, g_font_title_handle, g_font_text_shmem_id, @intCast(utf8_len), g_font_mask_shmem_id, &reply, 32) != 0) {
            return false;
        }
        if (reply.type != c.FONT_IPC_RESP or @as(i32, @bitCast(reply.arg0)) != c.FONT_STATUS_OK) {
            return false;
        }
        const mask_src: [*]const u8 = g_font_mask_ptr.?;
        var j: usize = 0;
        while (j < pixel_count) : (j += 1) {
            entry.mask_data[j] = mask_src[j];
        }
        entry.mask_len = pixel_count;
    }
    g_glyph_cache[slot] = entry;
    return true;
}

fn glyph_cache_get(codepoint: u32) ?*const glyph_cache_entry_t {
    if (glyph_cache_lookup(codepoint)) |idx| return &g_glyph_cache[idx];
    return null;
}

fn prime_title_glyph_step() void {
    _ = TITLE_GLYPHS;
}

fn init_title_glyph_cache_startup() void {
    if (!GFX_TITLE_TEXT_ENABLED) return;
    const ep = svc_lookup("font", g_runtime_lookup_req_id);
    g_runtime_lookup_req_id +%= 1;
    if (ep < 0) return;
    g_font_endpoint = @bitCast(ep);
    _ = open_title_font_handle();
}

fn draw_window_title_text(win: window_slot_t, clip: c.gfx_rect_t) void {
    _ = clip;
    if (g_font_endpoint == IPC_ENDPOINT_NONE or g_font_title_handle == 0) return;
    const slot_idx = window_find_by_id(win.window_id) orelse return;
    const cr = window_close_rect(win);
    const title_clip = window_title_rect(win);
    const pen_x: i32 = win.x + CHROME_BORDER + 4;
    const base_y: i32 = win.y + CHROME_TITLE_H - 7;
    var label: [GFX_MAX_TITLE_LABEL]u8 = undefined;
    const n = build_window_title_label(win.window_id, &label);
    if (n == 0) return;

    if (!title_cache_valid_for_window(slot_idx, win, label[0..n])) {
        if (!refresh_title_cache(slot_idx, win, label[0..n])) return;
    }
    const entry = g_title_run_cache[slot_idx];
    if (entry.w <= 0 or entry.h <= 0 or entry.mask_len == 0) return;
    draw_glyph_mask(
        pen_x + @as(i32, entry.x0),
        base_y + @as(i32, entry.y0),
        entry.w,
        entry.h,
        @ptrCast(&entry.mask_data[0]),
        title_clip,
        0xFFFFFFFF,
    );
    if (pen_x >= cr.x - 4) return;
}

fn draw_window_buffer(win: window_slot_t, buf: buffer_slot_t, clip: c.gfx_rect_t) bool {
    if (g_backbuffer_pixels == null) return false;
    // Use the permanently-cached mapping set at buffer allocation.
    const src_pixels = buf.mapped_pixels orelse {
        if (GFX_TRACE) { logMsg("[gfx-t] draw_window_buffer: mapped_pixels null!\n"); }
        return false;
    };
    const dst_pixels = g_backbuffer_pixels.?;
    const dst_stride: usize = @intCast(g_fb_info.framebuffer_stride);
    const cr = window_content_rect(win);
    if (cr.w <= 0 or cr.h <= 0 or !rect_intersects(clip, cr)) return true;
    const x0 = if (clip.x > cr.x) clip.x else cr.x;
    const y0 = if (clip.y > cr.y) clip.y else cr.y;
    const x1 = if (clip.x + clip.w < cr.x + cr.w) clip.x + clip.w else cr.x + cr.w;
    const y1 = if (clip.y + clip.h < cr.y + cr.h) clip.y + clip.h else cr.y + cr.h;
    if (x0 >= x1 or y0 >= y1) return true;

    const sx0: usize = @intCast(x0 - cr.x);
    const sy0: usize = @intCast(y0 - cr.y);
    const w: usize = @intCast(x1 - x0);
    const h: usize = @intCast(y1 - y0);
    const buf_w: usize = @intCast(buf.width);
    const fmt = g_fb_info.framebuffer_gop_pixel_format & 0xF;

    if (fmt == 1) {
        // Framebuffer is BGRX (format 1).  Client pixels are stored as
        // 0xAARRGGBB — same byte layout as the framebuffer expects (BGRA).
        // Direct row-by-row bulk copy; alpha channel is forced to 0xFF by
        // ORing the top byte (client alpha is ignored, all surfaces opaque).
        var row: usize = 0;
        while (row < h) : (row += 1) {
            const sy = sy0 + row;
            if (sy >= @as(usize, @intCast(buf.height))) break;
            const src_row = src_pixels + sy * buf_w + sx0;
            const dst_row = dst_pixels + (@as(usize, @intCast(y0)) + row) * dst_stride + @as(usize, @intCast(x0));
            // Force alpha=0xFF and copy RGB in one pass.
            var col: usize = 0;
            while (col + 4 <= w) : (col += 4) {
                dst_row[col]     = src_row[col]     | 0xFF000000;
                dst_row[col + 1] = src_row[col + 1] | 0xFF000000;
                dst_row[col + 2] = src_row[col + 2] | 0xFF000000;
                dst_row[col + 3] = src_row[col + 3] | 0xFF000000;
            }
            while (col < w) : (col += 1) {
                dst_row[col] = src_row[col] | 0xFF000000;
            }
        }
    } else {
        // Format 0 (RGBX) or unknown: need R↔B swap.
        var row: usize = 0;
        while (row < h) : (row += 1) {
            const sy = sy0 + row;
            if (sy >= @as(usize, @intCast(buf.height))) break;
            const src_row = src_pixels + sy * buf_w + sx0;
            const dst_row = dst_pixels + (@as(usize, @intCast(y0)) + row) * dst_stride + @as(usize, @intCast(x0));
            var col: usize = 0;
            while (col < w) : (col += 1) {
                const px = src_row[col];
                const b: u32 = px & 0xFF;
                const g_ch: u32 = (px >> 8) & 0xFF;
                const r: u32 = (px >> 16) & 0xFF;
                dst_row[col] = 0xFF000000 | (b << 16) | (g_ch << 8) | r;
            }
        }
    }
    return true;
}

fn compose_region(region: c.gfx_rect_t) i32 {
    if (!g_fb_info_valid or g_fb_pixels == null or region.w <= 0 or region.h <= 0) return c.GFX_STATUS_OK;

    fill_rect(region.x, region.y, region.w, region.h, 0x101820);

    var order: [GFX_MAX_WINDOWS]usize = undefined;
    var count: usize = 0;
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (!g_windows[i].in_use) continue;
        order[count] = i;
        count += 1;
    }
    var a: usize = 0;
    while (a < count) : (a += 1) {
        var b: usize = a + 1;
        while (b < count) : (b += 1) {
            if (g_windows[order[b]].z < g_windows[order[a]].z) {
                const t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
        }
    }

    var k: usize = 0;
    while (k < count) : (k += 1) {
        const win = g_windows[order[k]];
        var clip = region;
        const wr = rect_from_window(win);
        if (!rect_intersects(clip, wr)) continue;
        const x0 = if (clip.x > wr.x) clip.x else wr.x;
        const y0 = if (clip.y > wr.y) clip.y else wr.y;
        const x1 = if (clip.x + clip.w < wr.x + wr.w) clip.x + clip.w else wr.x + wr.w;
        const y1 = if (clip.y + clip.h < wr.y + wr.h) clip.y + clip.h else wr.y + wr.h;
        if (x0 >= x1 or y0 >= y1) continue;
        clip = .{ .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };

        var rendered = false;
        if (win.current_buffer_id != 0) {
            if (buffer_find_by_id(win.current_buffer_id)) |buf_idx| {
                const buf = g_buffers[buf_idx];
                rendered = draw_window_buffer(win, buf, clip);
            }
        }
        if (!rendered) {
            draw_window_placeholder(win, clip);
        }
        if ((win.flags & GFX_WINDOW_FLAG_SYSTEM) == 0) {
            draw_window_chrome(win, clip, g_focused_window_id == win.window_id);
        }
    }

    if (g_overlay_locked) {
        draw_cursor_overlay(region);
    }

    const src = g_backbuffer_pixels orelse return c.GFX_STATUS_OK;
    const dst = g_fb_pixels.?;
    const stride: usize = @intCast(g_fb_info.framebuffer_stride);
    const max_x: i32 = @intCast(g_fb_info.framebuffer_width);
    const max_y: i32 = @intCast(g_fb_info.framebuffer_height);
    var sx = region.x;
    var sy = region.y;
    var ex = region.x + region.w;
    var ey = region.y + region.h;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (ex > max_x) ex = max_x;
    if (ey > max_y) ey = max_y;
    if (sx >= ex or sy >= ey) return c.GFX_STATUS_OK;

    const width_pixels: usize = @intCast(ex - sx);
    const col_off: usize = @intCast(sx);

    if (col_off == 0 and width_pixels == stride) {
        // Full-width region: entire rows are contiguous — single bulk copy.
        const off: usize = @as(usize, @intCast(sy)) * stride;
        const len: usize = @as(usize, @intCast(ey - sy)) * stride;
        @memcpy(dst[off .. off + len], src[off .. off + len]);
    } else {
        // Partial-width region: one @memcpy per row (still a bulk copy vs per-pixel).
        var y: i32 = sy;
        while (y < ey) : (y += 1) {
            const row_base: usize = @as(usize, @intCast(y)) * stride;
            const off = row_base + col_off;
            @memcpy(dst[off .. off + width_pixels], src[off .. off + width_pixels]);
        }
    }

    return c.GFX_STATUS_OK;
}

fn compose_full() i32 {
    if (!g_fb_info_valid) return c.GFX_STATUS_IO;
    return compose_region(.{
        .x = 0,
        .y = 0,
        .w = @intCast(g_fb_info.framebuffer_width),
        .h = @intCast(g_fb_info.framebuffer_height),
    });
}

fn validate_window_dims(width: u32, height: u32) bool {
    return width >= GFX_WINDOW_MIN_DIM and height >= GFX_WINDOW_MIN_DIM and
        width <= GFX_WINDOW_MAX_DIM and height <= GFX_WINDOW_MAX_DIM;
}

fn handle_create_window(msg: *const c.nd_ipc_message_t) void {
    const width = msg.arg0;
    const height = msg.arg1;
    if (!validate_window_dims(width, height)) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }
    const slot_idx = window_alloc(msg.source, width, height) orelse {
        reply_with_status(msg, c.GFX_STATUS_BUSY, 0, 0, 0);
        return;
    };
    const win = g_windows[slot_idx];
    reply_with_status(msg, c.GFX_STATUS_OK, win.window_id, win.width, win.height);
}

fn handle_destroy_window(msg: *const c.nd_ipc_message_t) void {
    const window_id = msg.arg0;
    if (window_id == 0) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }
    const slot_idx = window_find_by_id(window_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    if (g_windows[slot_idx].owner_endpoint != msg.source) {
        if (!g_window_owner_deny_logged) {
            g_window_owner_deny_logged = true;
            logMsg("[test] gfx window owner deny ok\n");
        }
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    if (g_focused_window_id == window_id) {
        g_focused_window_id = 0;
    }
    g_windows[slot_idx] = .{};
    sync_console_mode_for_windows();
    request_repaint_full();
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
}

fn handle_resize_window(msg: *const c.nd_ipc_message_t) void {
    const window_id = msg.arg0;
    const width = msg.arg1;
    const height = msg.arg2;
    if (window_id == 0 or !validate_window_dims(width, height)) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }
    const slot_idx = window_find_by_id(window_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    if (g_windows[slot_idx].owner_endpoint != msg.source) {
        if (!g_window_owner_deny_logged) {
            g_window_owner_deny_logged = true;
            logMsg("[test] gfx window owner deny ok\n");
        }
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    g_windows[slot_idx].is_maximized = false;
    resize_window_and_notify(slot_idx, g_windows[slot_idx].x, g_windows[slot_idx].y, width, height);
    reply_with_status(msg, c.GFX_STATUS_OK, width, height, 0);
}

fn handle_alloc_shared_buffer(msg: *const c.nd_ipc_message_t) void {
    const window_id = msg.arg0;
    const width = msg.arg1;
    const height = msg.arg2;
    if (!validate_window_dims(width, height)) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }

    if (window_id != 0) {
        const window_idx = window_find_by_id(window_id) orelse {
            reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
            return;
        };
        if (g_windows[window_idx].owner_endpoint != msg.source) {
            if (!g_window_owner_deny_logged) {
                g_window_owner_deny_logged = true;
                logMsg("[test] gfx window owner deny ok\n");
            }
            reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
            return;
        }
        if (g_windows[window_idx].width != width or g_windows[window_idx].height != height) {
            reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
            return;
        }
    }

    const buf_idx = buffer_alloc(msg.source, width, height) orelse {
        reply_with_status(msg, c.GFX_STATUS_BUSY, 0, 0, 0);
        return;
    };
    const buf = g_buffers[buf_idx];

    if (window_id != 0) {
        if (window_find_by_id(window_id)) |window_idx| {
            g_buffers[buf_idx].bound_window_id = window_id;
            g_buffers[buf_idx].bound_window_generation = g_windows[window_idx].generation;
        }
    }

    reply_with_status(msg, c.GFX_STATUS_OK, buf.buffer_id, buf.shmem_id, buf.stride_bytes);
}

fn handle_release_shared_buffer(msg: *const c.nd_ipc_message_t) void {
    const buffer_id = msg.arg0;
    if (buffer_id == 0) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }
    const buf_idx = buffer_find_by_id(buffer_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    if (g_buffers[buf_idx].owner_endpoint != msg.source) {
        if (!g_buffer_owner_deny_logged) {
            g_buffer_owner_deny_logged = true;
            logMsg("[test] gfx buffer owner deny ok\n");
        }
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }

    if (window_buffer_in_use(buffer_id)) {
        reply_with_status(msg, c.GFX_STATUS_BUSY, 0, 0, 0);
        return;
    }

    const changed = detach_buffer_from_windows(buffer_id);
    if (g_buffers[buf_idx].shmem_id != 0) {
        _ = api().shmem_unmap.?(g_buffers[buf_idx].shmem_id);
    }
    g_buffers[buf_idx] = .{};
    if (changed) {
        request_repaint_full();
    }
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
}

fn handle_present_window(msg: *const c.nd_ipc_message_t) void {
    const window_id = msg.arg0;
    const buffer_id = msg.arg1;
    const damage_count = msg.arg2;
    const damage_shmem_id = msg.arg3;

    if (window_id == 0 or buffer_id == 0) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }

    const window_idx = window_find_by_id(window_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    if (g_windows[window_idx].owner_endpoint != msg.source) {
        if (!g_window_owner_deny_logged) {
            g_window_owner_deny_logged = true;
            logMsg("[test] gfx window owner deny ok\n");
        }
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }

    const buf_idx = buffer_find_by_id(buffer_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const buf = g_buffers[buf_idx];
    if (buf.owner_endpoint != msg.source) {
        if (!g_buffer_owner_deny_logged) {
            g_buffer_owner_deny_logged = true;
            logMsg("[test] gfx buffer owner deny ok\n");
        }
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }

    if (buf.width < g_windows[window_idx].width or buf.height < g_windows[window_idx].height) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }

    if (buf.bound_window_id != 0) {
        if (buf.bound_window_id != window_id or
            buf.bound_window_generation != g_windows[window_idx].generation)
        {
            reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
            return;
        }
    }

    if (buf.state == .acquired and g_windows[window_idx].current_buffer_id != buffer_id) {
        reply_with_status(msg, c.GFX_STATUS_BUSY, 0, 0, 0);
        return;
    }

    g_windows[window_idx].current_buffer_id = buffer_id;
    g_buffers[buf_idx].state = .acquired;
    g_buffers[buf_idx].bound_window_id = window_id;
    g_buffers[buf_idx].bound_window_generation = g_windows[window_idx].generation;
    if (GFX_TRACE) {
        if (g_overlay_locked) logMsg("[gfx-t] present win ok locked\n") else logMsg("[gfx-t] present win ok UNLOCKED\n");
    }
    focus_window(window_idx);
    sync_console_mode_for_windows();

    if (damage_count == 0 or damage_shmem_id == 0 or damage_count > GFX_MAX_DAMAGE_RECTS) {
        request_repaint_full();
        if (GFX_TRACE) { logMsg("[gfx-t] present-reply OK\n"); }
        reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
        return;
    }

    const dmg_ptr_raw = api().shmem_map.?(damage_shmem_id);
    if (dmg_ptr_raw == null) {
        request_repaint_full();
        reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
        return;
    }
    defer _ = api().shmem_unmap.?(damage_shmem_id);
    const dmg_rects: [*]const c.gfx_rect_t = @ptrCast(@alignCast(dmg_ptr_raw.?));

    var i: u32 = 0;
    while (i < damage_count) : (i += 1) {
        const r = dmg_rects[@intCast(i)];
        if (r.w <= 0 or r.h <= 0 or r.x < 0 or r.y < 0) {
            request_repaint_full();
            reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
            return;
        }
        const rw: u32 = @intCast(r.w);
        const rh: u32 = @intCast(r.h);
        const rx: u32 = @intCast(r.x);
        const ry: u32 = @intCast(r.y);
        if (rx >= g_windows[window_idx].width or ry >= g_windows[window_idx].height or
            rw > (g_windows[window_idx].width - rx) or rh > (g_windows[window_idx].height - ry))
        {
            request_repaint_full();
            reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
            return;
        }

        const screen_rect = c.gfx_rect_t{
            .x = g_windows[window_idx].x + CHROME_BORDER + r.x,
            .y = g_windows[window_idx].y + CHROME_TITLE_H + r.y,
            .w = r.w,
            .h = r.h,
        };
        request_repaint_rect(screen_rect);
    }

    if (!g_damage_marker_logged) {
        g_damage_marker_logged = true;
        logMsg("[test] gfx damage present path ok\n");
    }
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
}

fn handle_poll_event(msg: *const c.nd_ipc_message_t) void {
    if (GFX_TRACE) { logMsg("[gfx-t] poll-event\n"); }
    var ev: gfx_event_t = .{};
    if (event_pop_for(msg.source, &ev)) {
        reply_with_status(msg, c.GFX_STATUS_OK, ev.event_type, ev.arg1, ev.arg2);
        return;
    }
    reply_with_status(msg, c.GFX_STATUS_OK, c.GFX_EVENT_NONE, 0, 0);
}

fn handle_set_display_mode(msg: *const c.nd_ipc_message_t) void {
    if (g_fb_endpoint == IPC_ENDPOINT_NONE) {
        reply_with_status(msg, c.GFX_STATUS_IO, 0, 0, 0);
        return;
    }
    const width = msg.arg0;
    const height = msg.arg1;
    if (width == 0 or height == 0) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }

    var caps_reply: c.nd_ipc_message_t = undefined;
    const caps_req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 11;
    if (ipc_call(g_fb_endpoint, caps_req_id, c.FBTEXT_IPC_QUERY_CAPS_REQ, 0, 0, 0, 0, &caps_reply) != 0 or
        caps_reply.type != c.FBTEXT_IPC_RESP)
    {
        reply_with_status(msg, c.GFX_STATUS_IO, 0, 0, 0);
        return;
    }
    if ((caps_reply.arg0 & c.FBTEXT_CAP_SET_RESOLUTION) == 0) {
        reply_with_status(msg, c.GFX_STATUS_UNSUPPORTED, 0, 0, 0);
        return;
    }

    var mode_reply: c.nd_ipc_message_t = undefined;
    const mode_req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 12;
    if (ipc_call(g_fb_endpoint, mode_req_id, c.FBTEXT_IPC_SET_RESOLUTION_REQ, width, height, 0, 0, &mode_reply) != 0) {
        reply_with_status(msg, c.GFX_STATUS_IO, 0, 0, 0);
        return;
    }
    if (mode_reply.type != c.FBTEXT_IPC_RESP) {
        const fb_status: i32 = @bitCast(mode_reply.arg0);
        if (fb_status == c.GFX_STATUS_UNSUPPORTED or fb_status == -3) {
            reply_with_status(msg, c.GFX_STATUS_UNSUPPORTED, 0, 0, 0);
            return;
        }
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }

    const refresh_rc = refresh_framebuffer_mapping();
    if (refresh_rc != c.GFX_STATUS_OK) {
        reply_with_status(msg, refresh_rc, 0, 0, 0);
        return;
    }
    const fb_bytes_u64: u64 = @as(u64, g_fb_info.framebuffer_stride) * @as(u64, g_fb_info.framebuffer_height) * 4;
    if (fb_bytes_u64 == 0 or fb_bytes_u64 > 0xFFFF_FFFF or
        ensure_backbuffer_capacity(@intCast(fb_bytes_u64)) != c.GFX_STATUS_OK)
    {
        reply_with_status(msg, c.GFX_STATUS_IO, 0, 0, 0);
        return;
    }
    request_repaint_full();
    reply_with_status(msg, c.GFX_STATUS_OK, g_fb_info.framebuffer_width, g_fb_info.framebuffer_height, g_fb_info.framebuffer_stride);
}

fn handle_list_windows(msg: *const c.nd_ipc_message_t) void {
    const idx_req: usize = @intCast(if (msg.arg0 >= 0) @as(u32, @intCast(msg.arg0)) else 0);
    var count: usize = 0;
    var k: usize = 0;
    while (k < g_windows.len) : (k += 1) {
        if (!g_windows[k].in_use) continue;
        if (count == idx_req) {
            var total: usize = count + 1;
            var m: usize = k + 1;
            while (m < g_windows.len) : (m += 1) {
                if (g_windows[m].in_use) total += 1;
            }
            reply_with_status(msg, c.GFX_STATUS_OK,
                @intCast(g_windows[k].window_id),
                @intCast(g_windows[k].owner_endpoint),
                @intCast(total));
            return;
        }
        count += 1;
    }
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, @intCast(count));
}

fn handle_focus_window(msg: *const c.nd_ipc_message_t) void {
    const window_id: u32 = @intCast(if (msg.arg0 > 0) @as(u32, @intCast(msg.arg0)) else 0);
    const slot_idx = window_find_by_id(window_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    raise_window(slot_idx);
    focus_window(slot_idx);
    request_repaint_full();
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
}

fn handle_set_window_flags(msg: *const c.nd_ipc_message_t) void {
    const window_id: u32 = @intCast(if (msg.arg0 > 0) @as(u32, @intCast(msg.arg0)) else 0);
    const flags: u32 = @bitCast(msg.arg1);
    const slot_idx = window_find_by_id(window_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    if (g_windows[slot_idx].owner_endpoint != msg.source) {
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    g_windows[slot_idx].flags = flags;
    if ((flags & GFX_WINDOW_FLAG_SYSTEM) != 0) {
        g_windows[slot_idx].z = GFX_WINDOW_Z_SYSTEM;
        g_windows[slot_idx].x = 0;
        g_windows[slot_idx].y = 0;
    }
    request_repaint_full();
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
}

fn handle_get_display_info(msg: *const c.nd_ipc_message_t) void {
    if (!g_fb_info_valid) {
        reply_with_status(msg, c.GFX_STATUS_IO, 0, 0, 0);
        return;
    }
    reply_with_status(msg, c.GFX_STATUS_OK,
        @intCast(g_fb_info.framebuffer_width),
        @intCast(g_fb_info.framebuffer_height),
        0);
}

fn handle_ipc_dispatch(msg: *const c.nd_ipc_message_t) void {
    var opcode: u16 = @intCast(msg.type & 0xFFFF);
    if (gfx_header_valid(msg.arg2, msg.arg3)) {
        opcode = gfx_header_opcode(msg.arg3);
    }
    switch (opcode) {
        c.KBD_IPC_KEY_NOTIFY => {
            const scancode: u8 = @intCast(msg.arg0 & 0xFF);
            const keyup = (msg.arg1 & 1) != 0;
            const extended = (msg.arg2 & 1) != 0;
            if (scancode == 0x2A or scancode == 0x36) {
                g_shift_down = !keyup;
            }
            if (scancode == 0x38 and extended) {
                g_altgr_down = !keyup;
            }
            if (g_focused_window_id != 0) {
                if (window_find_by_id(g_focused_window_id)) |focused_idx| {
                    const focused = g_windows[focused_idx];
                    const key_flags: u32 = (if ((msg.arg1 & 1) == 0) @as(u32, 1) else @as(u32, 0)) | ((msg.arg2 & 1) << 1);
                    var key_code: u32 = 0;
                    if (!extended) {
                        key_code = scancode_to_ascii(scancode, g_shift_down, g_altgr_down);
                    }
                    event_push(focused.owner_endpoint, c.GFX_EVENT_KEY, key_code, key_flags, 0);
                }
            }
        },
        c.MOUSE_IPC_MOVE_NOTIFY => handle_mouse_notify(msg),
        c.GFX_IPC_CREATE_WINDOW => handle_create_window(msg),
        c.GFX_IPC_DESTROY_WINDOW => handle_destroy_window(msg),
        c.GFX_IPC_RESIZE_WINDOW => handle_resize_window(msg),
        c.GFX_IPC_ALLOC_SHARED_BUFFER => handle_alloc_shared_buffer(msg),
        c.GFX_IPC_RELEASE_SHARED_BUFFER => handle_release_shared_buffer(msg),
        c.GFX_IPC_PRESENT_WINDOW => handle_present_window(msg),
        c.GFX_IPC_POLL_EVENT => handle_poll_event(msg),
        c.GFX_IPC_SET_DISPLAY_MODE => handle_set_display_mode(msg),
        c.GFX_IPC_LIST_WINDOWS => handle_list_windows(msg),
        c.GFX_IPC_FOCUS_WINDOW => handle_focus_window(msg),
        c.GFX_IPC_SET_WINDOW_FLAGS => handle_set_window_flags(msg),
        c.GFX_IPC_GET_DISPLAY_INFO => handle_get_display_info(msg),
        else => reply_unsupported(msg),
    }
}

fn register_ipc_handlers() i32 {
    const cb = struct {
        fn onMessage(user: ?*anyopaque, msg_raw: ?*const anyopaque) callconv(.c) void {
            _ = user;
            if (msg_raw) |m| {
                const msg: *const c.nd_ipc_message_t = @ptrCast(@alignCast(m));
                handle_ipc_dispatch(msg);
            }
        }
    }.onMessage;
    if (sys.eventRegister(&g_ipc_loop, c.KBD_IPC_KEY_NOTIFY, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.MOUSE_IPC_MOVE_NOTIFY, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_CREATE_WINDOW, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_DESTROY_WINDOW, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_RESIZE_WINDOW, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_ALLOC_SHARED_BUFFER, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_RELEASE_SHARED_BUFFER, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_PRESENT_WINDOW, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_POLL_EVENT, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_SET_DISPLAY_MODE, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_LIST_WINDOWS, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_FOCUS_WINDOW, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_SET_WINDOW_FLAGS, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.GFX_IPC_GET_DISPLAY_INFO, cb, null) != 0) return -1;
    return 0;
}

pub export fn initialize(driver_api: *c.wasmos_driver_api_t, module_count: c_int, arg2: c_int, arg3: c_int) c_int {
    _ = arg2;
    _ = arg3;

    g_api = driver_api;
    if (driver_api.abi_magic != c.WASMOS_NATIVE_ABI_MAGIC or
        driver_api.abi_version != c.WASMOS_NATIVE_ABI_VERSION)
    {
        return -2;
    }

    g_proc_endpoint = @bitCast(module_count);
    if (g_proc_endpoint == IPC_ENDPOINT_NONE) return -1;

    g_rng_state ^= g_proc_endpoint ^ @as(u32, @intCast(@intFromPtr(driver_api)));

    g_gfx_endpoint = api().ipc_create_endpoint.?();
    if (g_gfx_endpoint == IPC_ENDPOINT_NONE) return -1;
    sys.eventLoopInit(&g_ipc_loop, api(), g_gfx_endpoint, GFX_REQUEST_BASE + 0x8000);
    if (register_ipc_handlers() != 0) return -1;

    if (svc_register("gfx", 1) != 0) {
        logMsg("[gfx] register failed\n");
        return -1;
    }

    if (lookup_fb_endpoint() != 0) {
        logMsg("[gfx] fb endpoint unavailable\n");
    } else {
        _ = lookup_vt_endpoint();
        if (lookup_kbd_endpoint() == 0) {
            _ = subscribe_keyboard();
        }
        if (lookup_mouse_endpoint() == 0) {
            _ = subscribe_mouse();
        }
        log_fb_geometry_probe();
        log_fb_capabilities_probe();
        logMsg("[test] gfx compositor handshake ok\n");
    }

    if (refresh_framebuffer_mapping() == c.GFX_STATUS_OK) {
        const fb_bytes_u64: u64 = @as(u64, g_fb_info.framebuffer_stride) * @as(u64, g_fb_info.framebuffer_height) * 4;
        if (fb_bytes_u64 == 0 or fb_bytes_u64 > 0xFFFF_FFFF or
            ensure_backbuffer_capacity(@intCast(fb_bytes_u64)) != c.GFX_STATUS_OK)
        {
            logMsg("[gfx] backbuffer init failed\n");
            return -1;
        }
        g_pointer_x = @intCast(g_fb_info.framebuffer_width / 2);
        g_pointer_y = @intCast(g_fb_info.framebuffer_height / 2);
    } else {
        logMsg("[gfx] framebuffer borrow failed\n");
    }
    init_title_glyph_cache_startup();
    _ = api().proc_notify_ready.?();

    while (true) {
        const handled = sys.eventLoopPoll(&g_ipc_loop, 32);
        if (handled < 0) return -1;
        if (handled > 0) {
            g_total_handled_counter +%= @intCast(handled);
            if ((g_total_handled_counter & 0xFF) == 0) {
                refresh_input_subscriptions_runtime();
            }
        }
        if (handled == 0) {
            if (GFX_TITLE_TEXT_ENABLED) {
                ensure_font_title_ready_lazy();
                prime_title_glyph_step();
            }
            flush_repaint_if_pending();
            g_idle_housekeeping_counter +%= 1;
            if ((g_idle_housekeeping_counter & 0x3F) == 0) {
                cleanup_orphaned_state();
            }
            api().sched_yield.?();
            continue;
        }
        flush_repaint_if_pending();
    }
}
