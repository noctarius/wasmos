const c = @cImport({
    @cInclude("gfx_compositor_imports.h");
});

const IPC_OK: i32 = 0;
const IPC_EMPTY: i32 = 1;
const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;
const GFX_REQUEST_BASE: u32 = 0x7000;
const GFX_FB_LOOKUP_RETRIES: u32 = 2048;
const GFX_MAX_WINDOWS: usize = 32;
const GFX_MAX_BUFFERS: usize = 64;
const GFX_MAX_DAMAGE_RECTS: u32 = 256;
const GFX_MAX_EVENTS: usize = 128;
const GFX_WINDOW_MIN_DIM: u32 = 1;
const GFX_WINDOW_MAX_DIM: u32 = 8192;
const PAGE_SIZE: u64 = 4096;
const CURSOR_W: i32 = 9;
const CURSOR_H: i32 = 14;
const CHROME_BORDER: i32 = 1;
const CHROME_TITLE_H: i32 = 14;
const CHROME_CLOSE_SZ: i32 = 10;
const CHROME_CLOSE_PAD: i32 = 2;
const CHROME_CLOSE_HIT_W: i32 = 24;
const CHROME_RESIZE_HANDLE_SZ: i32 = 12;

var g_api: ?*c.wasmos_driver_api_t = null;
var g_proc_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_gfx_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_fb_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_vt_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_kbd_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_mouse_endpoint: u32 = IPC_ENDPOINT_NONE;
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
var g_idle_housekeeping_counter: u32 = 0;
var g_runtime_lookup_req_id: u32 = GFX_REQUEST_BASE + 0x4000;
var g_close_emit_logged: bool = false;

var g_fb_info: c.nd_framebuffer_info_t = .{
    .framebuffer_base = 0,
    .framebuffer_size = 0,
    .framebuffer_width = 0,
    .framebuffer_height = 0,
    .framebuffer_stride = 0,
    .framebuffer_reserved = 0,
};
var g_fb_info_valid: bool = false;
var g_fb_pixels: ?[*]volatile u32 = null;

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
};

const gfx_event_t = struct {
    endpoint: u32 = IPC_ENDPOINT_NONE,
    event_type: u32 = 0,
    arg1: u32 = 0,
    arg2: u32 = 0,
    arg3: u32 = 0,
};

var g_windows: [GFX_MAX_WINDOWS]window_slot_t = [_]window_slot_t{.{}} ** GFX_MAX_WINDOWS;
var g_buffers: [GFX_MAX_BUFFERS]buffer_slot_t = [_]buffer_slot_t{.{}} ** GFX_MAX_BUFFERS;
var g_events: [GFX_MAX_EVENTS]gfx_event_t = [_]gfx_event_t{.{}} ** GFX_MAX_EVENTS;
var g_event_head: usize = 0;
var g_event_tail: usize = 0;

fn api() *c.wasmos_driver_api_t {
    return g_api.?;
}

fn ctxId() u32 {
    return api().sched_current_pid.?();
}

fn logMsg(msg: []const u8) void {
    _ = api().console_write.?(msg.ptr, @intCast(msg.len));
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

fn packName16(name: []const u8, out: *[4]u32) void {
    out.* = .{ 0, 0, 0, 0 };
    var i: usize = 0;
    while (i < name.len and i < 16) : (i += 1) {
        const slot: usize = i / 4;
        const shift: u5 = @intCast((i % 4) * 8);
        out[slot] |= (@as(u32, name[i]) << shift);
    }
}

fn svc_register(name: []const u8, request_id: u32) i32 {
    var args: [4]u32 = undefined;
    var msg: c.nd_ipc_message_t = undefined;
    packName16(name, &args);

    msg.type = c.SVC_IPC_REGISTER_REQ;
    msg.source = g_gfx_endpoint;
    msg.destination = g_proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];
    if (api().ipc_send.?(ctxId(), g_proc_endpoint, &msg) != IPC_OK) {
        return -1;
    }
    while (true) {
        const rc = api().ipc_recv.?(ctxId(), g_gfx_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) {
            return -1;
        }
        if (msg.request_id == request_id) break;
    }
    if (msg.type != c.SVC_IPC_REGISTER_RESP) {
        return -1;
    }
    return @bitCast(msg.arg0);
}

fn svc_lookup(name: []const u8, request_id: u32) i32 {
    var args: [4]u32 = undefined;
    var msg: c.nd_ipc_message_t = undefined;
    packName16(name, &args);

    msg.type = c.SVC_IPC_LOOKUP_REQ;
    msg.source = g_gfx_endpoint;
    msg.destination = g_proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];
    if (api().ipc_send.?(ctxId(), g_proc_endpoint, &msg) != IPC_OK) {
        return -1;
    }
    while (true) {
        const rc = api().ipc_recv.?(ctxId(), g_gfx_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) {
            return -1;
        }
        if (msg.request_id == request_id) break;
    }
    if (msg.type != c.SVC_IPC_LOOKUP_RESP or msg.arg0 == IPC_ENDPOINT_NONE) {
        return -1;
    }
    return @bitCast(msg.arg0);
}

fn lookup_fb_endpoint() i32 {
    var i: u32 = 0;
    while (i < GFX_FB_LOOKUP_RETRIES) : (i += 1) {
        const ep = svc_lookup("fb", GFX_REQUEST_BASE + i);
        if (ep >= 0) {
            g_fb_endpoint = @bitCast(ep);
            return 0;
        }
        api().sched_yield.?();
    }
    return -1;
}

fn lookup_vt_endpoint() i32 {
    var i: u32 = 0;
    while (i < GFX_FB_LOOKUP_RETRIES) : (i += 1) {
        const ep = svc_lookup("vt", GFX_REQUEST_BASE + 0x100 + i);
        if (ep >= 0) {
            g_vt_endpoint = @bitCast(ep);
            return 0;
        }
        api().sched_yield.?();
    }
    return -1;
}

fn lookup_kbd_endpoint() i32 {
    var i: u32 = 0;
    while (i < GFX_FB_LOOKUP_RETRIES) : (i += 1) {
        const ep = svc_lookup("kbd", GFX_REQUEST_BASE + 0x180 + i);
        if (ep >= 0) {
            g_kbd_endpoint = @bitCast(ep);
            return 0;
        }
        api().sched_yield.?();
    }
    return -1;
}

fn lookup_mouse_endpoint() i32 {
    var i: u32 = 0;
    while (i < GFX_FB_LOOKUP_RETRIES) : (i += 1) {
        const ep = svc_lookup("mouse", GFX_REQUEST_BASE + 0x1C0 + i);
        if (ep >= 0) {
            g_mouse_endpoint = @bitCast(ep);
            return 0;
        }
        api().sched_yield.?();
    }
    return -1;
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
        g_buffers[i] = .{};
        changed = true;
    }

    prune_events_for_dead_endpoints();
    if (changed) {
        sync_console_mode_for_windows();
        _ = compose_full();
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
    var req: c.nd_ipc_message_t = undefined;
    req.type = msg_type;
    req.source = g_gfx_endpoint;
    req.destination = destination;
    req.request_id = request_id;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.arg3 = arg3;
    if (api().ipc_send.?(ctxId(), destination, &req) != IPC_OK) return -1;

    while (true) {
        const rc = api().ipc_recv.?(ctxId(), g_gfx_endpoint, out);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) return -1;
        if (out.request_id == request_id) return 0;
    }
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

fn handle_mouse_notify(msg: *const c.nd_ipc_message_t) void {
    const old_x = g_pointer_x;
    const old_y = g_pointer_y;
    const dx8: i8 = @bitCast(@as(u8, @truncate(msg.arg0)));
    const dy8: i8 = @bitCast(@as(u8, @truncate(msg.arg1)));
    const dx: i32 = @as(i32, dx8);
    const dy: i32 = @as(i32, dy8);
    const buttons: u32 = msg.arg2 & 0x7;

    if (g_fb_info_valid) {
        const max_x: i32 = @intCast(g_fb_info.framebuffer_width);
        const max_y: i32 = @intCast(g_fb_info.framebuffer_height);
        const hi_x = if (max_x > 0) max_x - 1 else 0;
        const hi_y = if (max_y > 0) max_y - 1 else 0;
        g_pointer_x = clamp(g_pointer_x + dx, 0, hi_x);
        g_pointer_y = clamp(g_pointer_y + dy, 0, hi_y);
    }

    if (g_overlay_locked and (old_x != g_pointer_x or old_y != g_pointer_y)) {
        _ = compose_region(cursor_rect_at(old_x, old_y));
        _ = compose_region(cursor_rect_at(g_pointer_x, g_pointer_y));
    }

    const prev_buttons = g_pointer_buttons;
    const left_down_now = (buttons & 0x1) != 0;
    const left_down_prev = (prev_buttons & 0x1) != 0;

    if (!left_down_now and left_down_prev) {
        g_drag_window_id = 0;
        g_resize_window_id = 0;
    }

    if (left_down_now and g_resize_window_id != 0 and (dx != 0 or dy != 0)) {
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
            _ = compose_region(old_wr);
            _ = compose_region(new_wr);
            if (g_windows[resize_idx].width != old_w or g_windows[resize_idx].height != old_h) {
                const win = g_windows[resize_idx];
                event_push(win.owner_endpoint, c.GFX_EVENT_RESIZE, win.window_id, pack_u16_pair(win.width, win.height), 0);
            }
        } else {
            g_resize_window_id = 0;
        }
    }

    if (left_down_now and g_drag_window_id != 0 and (dx != 0 or dy != 0)) {
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
            _ = compose_region(old_wr);
            _ = compose_region(new_wr);
        } else {
            g_drag_window_id = 0;
        }
    }

    if (left_down_now and !left_down_prev) {
        if (window_topmost_at(g_pointer_x, g_pointer_y)) |idx| {
            const hit_close = point_in_rect(g_pointer_x, g_pointer_y, window_close_hit_rect(g_windows[idx]));
            const hit_resize = point_in_rect(g_pointer_x, g_pointer_y, window_resize_rect(g_windows[idx]));
            const hit_title = point_in_rect(g_pointer_x, g_pointer_y, window_title_rect(g_windows[idx]));
            raise_window(idx);
            focus_window(idx);
            if (hit_close) {
                const win = g_windows[idx];
                if (!g_close_emit_logged) {
                    g_close_emit_logged = true;
                    logMsg("[gfx] close-request emitted\n");
                }
                event_drop_pointer_for(win.owner_endpoint);
                event_push(win.owner_endpoint, c.GFX_EVENT_CLOSE_REQUEST, win.window_id, 0, 0);
            } else if (hit_resize) {
                g_resize_window_id = g_windows[idx].window_id;
                g_drag_window_id = 0;
            } else if (hit_title) {
                g_drag_window_id = g_windows[idx].window_id;
                g_resize_window_id = 0;
            }
            _ = compose_full();
        } else {
            if (blur_focused_window()) {
                _ = compose_full();
            }
            g_drag_window_id = 0;
            g_resize_window_id = 0;
        }
    }
    g_pointer_buttons = buttons;

    if (g_focused_window_id != 0) {
        if (window_find_by_id(g_focused_window_id)) |focused_idx| {
            const focused = g_windows[focused_idx];
            if (dx != 0 or dy != 0 or buttons != prev_buttons) {
                event_push(focused.owner_endpoint, c.GFX_EVENT_POINTER, pack_s16_pair(dx, dy), buttons, 0);
            }
        }
    }
}

fn fb_set_overlay_lock(lock: bool) void {
    if (g_fb_endpoint == IPC_ENDPOINT_NONE) return;
    var reply: c.nd_ipc_message_t = undefined;
    const req_id: u32 = GFX_REQUEST_BASE + GFX_FB_LOOKUP_RETRIES + 9;
    _ = ipc_call(g_fb_endpoint, req_id, c.FBTEXT_IPC_GFX_OVERLAY_REQ, if (lock) 1 else 0, 0, 0, 0, &reply);
}

fn sync_console_mode_for_windows() void {
    const has_presented_windows = active_presented_window_count() > 0;
    if (has_presented_windows and !g_overlay_locked) {
        try_switch_to_gfx_tty();
        fb_set_overlay_lock(true);
        g_overlay_locked = true;
        if (g_fb_info_valid) {
            _ = compose_region(.{
                .x = 0,
                .y = 0,
                .w = @intCast(g_fb_info.framebuffer_width),
                .h = @intCast(g_fb_info.framebuffer_height),
            });
        }
        return;
    }
    if (!has_presented_windows and g_overlay_locked) {
        fb_set_overlay_lock(false);
        g_overlay_locked = false;
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
    _ = api().ipc_send.?(ctxId(), msg.source, &resp);
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
    if (api().shmem_create.?(pages, 0, &shmem_id, @ptrCast(&mapped_ptr)) != 0 or shmem_id == 0) {
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
    if (!g_fb_info_valid or g_fb_pixels == null or w <= 0 or h <= 0) return;
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
    const fb = g_fb_pixels.?;
    var y = sy;
    while (y < ey) : (y += 1) {
        const row_base: usize = @as(usize, @intCast(y)) * stride;
        var x = sx;
        while (x < ex) : (x += 1) {
            const idx: usize = row_base + @as(usize, @intCast(x));
            fb[idx] = color;
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
    if (!g_fb_info_valid or g_fb_pixels == null) return;
    const cr = cursor_rect_at(g_pointer_x, g_pointer_y);
    if (!rect_intersects(region, cr)) return;

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
    return .{
        .x = win.x + ww - CHROME_CLOSE_PAD - CHROME_CLOSE_SZ,
        .y = win.y + CHROME_CLOSE_PAD,
        .w = CHROME_CLOSE_SZ,
        .h = CHROME_CLOSE_SZ,
    };
}

fn window_close_hit_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    return .{
        .x = win.x + ww - CHROME_CLOSE_HIT_W,
        .y = win.y,
        .w = CHROME_CLOSE_HIT_W,
        .h = CHROME_TITLE_H,
    };
}

fn window_title_rect(win: window_slot_t) c.gfx_rect_t {
    const ww: i32 = @intCast(win.width);
    return .{
        .x = win.x + CHROME_BORDER,
        .y = win.y,
        .w = ww - (CHROME_BORDER * 2),
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

    fill_rect(win.x, win.y, @intCast(win.width), CHROME_BORDER, border_color);
    fill_rect(win.x, win.y, CHROME_BORDER, @intCast(win.height), border_color);
    fill_rect(win.x + @as(i32, @intCast(win.width)) - CHROME_BORDER, win.y, CHROME_BORDER, @intCast(win.height), border_color);
    fill_rect(win.x, win.y + @as(i32, @intCast(win.height)) - CHROME_BORDER, @intCast(win.width), CHROME_BORDER, border_color);
    const ww: i32 = @intCast(win.width);
    fill_rect(win.x + CHROME_BORDER, win.y + CHROME_BORDER, ww - (CHROME_BORDER * 2), CHROME_TITLE_H - CHROME_BORDER, title_color);

    const cr = window_close_rect(win);
    fill_rect(cr.x, cr.y, cr.w, cr.h, close_bg);
    fill_rect(cr.x + 2, cr.y + 2, cr.w - 4, 1, close_fg);
    fill_rect(cr.x + 2, cr.y + cr.h - 3, cr.w - 4, 1, close_fg);
    fill_rect(cr.x + 2, cr.y + 2, 1, cr.h - 4, close_fg);
    fill_rect(cr.x + cr.w - 3, cr.y + 2, 1, cr.h - 4, close_fg);
}

fn draw_window_buffer(win: window_slot_t, buf: buffer_slot_t, clip: c.gfx_rect_t) bool {
    if (g_fb_pixels == null) return false;
    const src_ptr_raw = api().shmem_map.?(buf.shmem_id);
    if (src_ptr_raw == null) return false;
    defer _ = api().shmem_unmap.?(buf.shmem_id);

    const src_pixels: [*]const u32 = @ptrCast(@alignCast(src_ptr_raw.?));
    const dst_pixels = g_fb_pixels.?;
    const dst_stride: usize = @intCast(g_fb_info.framebuffer_stride);

    var y: i32 = 0;
    while (y < clip.h) : (y += 1) {
        var x: i32 = 0;
        while (x < clip.w) : (x += 1) {
            const sx_i32 = (clip.x - win.x) + x;
            const sy_i32 = (clip.y - win.y) + y;
            if (sx_i32 < 0 or sy_i32 < 0) continue;
            const sx: u32 = @intCast(sx_i32);
            const sy: u32 = @intCast(sy_i32);
            if (sx >= buf.width or sy >= buf.height) continue;
            const idx_u64 = @as(u64, sy) * @as(u64, buf.width) + @as(u64, sx);
            const src_idx: usize = @intCast(idx_u64);
            const dx: usize = @intCast(clip.x + x);
            const dy: usize = @intCast(clip.y + y);
            const dst_idx: usize = dy * dst_stride + dx;
            dst_pixels[dst_idx] = src_pixels[src_idx];
        }
    }
    return true;
}

fn compose_region(region: c.gfx_rect_t) i32 {
    if (!g_fb_info_valid or region.w <= 0 or region.h <= 0) return c.GFX_STATUS_OK;

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
        draw_window_chrome(win, clip, g_focused_window_id == win.window_id);
    }

    if (g_overlay_locked) {
        draw_cursor_overlay(region);
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
    _ = compose_full();
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
    g_windows[slot_idx].generation +%= 1;
    if (g_windows[slot_idx].generation == 0) g_windows[slot_idx].generation = 1;
    g_windows[slot_idx].current_buffer_id = 0;
    g_windows[slot_idx].width = width;
    g_windows[slot_idx].height = height;
    _ = compose_full();
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
    // TODO(gfx-buffer-release): native driver ABI currently has map/unmap but
    // no shmem-destroy primitive; this releases compositor references and
    // invalidates handle usage, but backing pages are not reclaimed yet.
    g_buffers[buf_idx] = .{};
    if (changed) {
        _ = compose_full();
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
    focus_window(window_idx);
    sync_console_mode_for_windows();

    if (damage_count == 0 or damage_shmem_id == 0 or damage_count > GFX_MAX_DAMAGE_RECTS) {
        reply_with_status(msg, compose_full(), 0, 0, 0);
        return;
    }

    const dmg_ptr_raw = api().shmem_map.?(damage_shmem_id);
    if (dmg_ptr_raw == null) {
        reply_with_status(msg, compose_full(), 0, 0, 0);
        return;
    }
    defer _ = api().shmem_unmap.?(damage_shmem_id);
    const dmg_rects: [*]const c.gfx_rect_t = @ptrCast(@alignCast(dmg_ptr_raw.?));

    var i: u32 = 0;
    while (i < damage_count) : (i += 1) {
        const r = dmg_rects[@intCast(i)];
        if (r.w <= 0 or r.h <= 0 or r.x < 0 or r.y < 0) {
            reply_with_status(msg, compose_full(), 0, 0, 0);
            return;
        }
        const rw: u32 = @intCast(r.w);
        const rh: u32 = @intCast(r.h);
        const rx: u32 = @intCast(r.x);
        const ry: u32 = @intCast(r.y);
        if (rx >= g_windows[window_idx].width or ry >= g_windows[window_idx].height or
            rw > (g_windows[window_idx].width - rx) or rh > (g_windows[window_idx].height - ry))
        {
            reply_with_status(msg, compose_full(), 0, 0, 0);
            return;
        }

        const screen_rect = c.gfx_rect_t{
            .x = g_windows[window_idx].x + r.x,
            .y = g_windows[window_idx].y + r.y,
            .w = r.w,
            .h = r.h,
        };
        _ = compose_region(screen_rect);
    }

    if (!g_damage_marker_logged) {
        g_damage_marker_logged = true;
        logMsg("[test] gfx damage present path ok\n");
    }
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
}

fn handle_poll_event(msg: *const c.nd_ipc_message_t) void {
    var ev: gfx_event_t = .{};
    if (event_pop_for(msg.source, &ev)) {
        reply_with_status(msg, c.GFX_STATUS_OK, ev.event_type, ev.arg1, ev.arg2);
        return;
    }
    reply_with_status(msg, c.GFX_STATUS_OK, c.GFX_EVENT_NONE, 0, 0);
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
        logMsg("[test] gfx compositor handshake ok\n");
    }

    if (api().framebuffer_info.?(&g_fb_info) == 0 and
        g_fb_info.framebuffer_width != 0 and
        g_fb_info.framebuffer_height != 0)
    {
        g_fb_info_valid = true;
        g_pointer_x = @intCast(g_fb_info.framebuffer_width / 2);
        g_pointer_y = @intCast(g_fb_info.framebuffer_height / 2);
        const fb_size_u64: u64 = @as(u64, g_fb_info.framebuffer_stride) *
            @as(u64, g_fb_info.framebuffer_height) * 4;
        if (fb_size_u64 > 0 and fb_size_u64 <= 0xFFFF_FFFF) {
            const fb_ptr = api().buffer_borrow.?(c.ND_BUFFER_KIND_FRAMEBUFFER, 0, c.ND_BUFFER_BORROW_READ | c.ND_BUFFER_BORROW_WRITE, @intCast(fb_size_u64));
            if (fb_ptr != null) {
                g_fb_pixels = @ptrCast(@alignCast(fb_ptr.?));
            } else {
                logMsg("[gfx] framebuffer borrow failed\n");
            }
        }
    }

    while (true) {
        var msg: c.nd_ipc_message_t = undefined;
        const rc = api().ipc_recv.?(ctxId(), g_gfx_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            g_idle_housekeeping_counter +%= 1;
            if ((g_idle_housekeeping_counter & 0x3F) == 0) {
                refresh_input_subscriptions_runtime();
                cleanup_orphaned_state();
            }
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) return -1;

        var opcode: u16 = @intCast(msg.type & 0xFFFF);
        if (gfx_header_valid(msg.arg2, msg.arg3)) {
            opcode = gfx_header_opcode(msg.arg3);
        }

        switch (opcode) {
            c.KBD_IPC_KEY_NOTIFY => {
                if (g_focused_window_id != 0) {
                    if (window_find_by_id(g_focused_window_id)) |focused_idx| {
                        const focused = g_windows[focused_idx];
                        const key_flags = (msg.arg1 & 1) | ((msg.arg2 & 1) << 1);
                        event_push(focused.owner_endpoint, c.GFX_EVENT_KEY, msg.arg0, key_flags, 0);
                    }
                }
            },
            c.MOUSE_IPC_MOVE_NOTIFY => handle_mouse_notify(&msg),
            c.GFX_IPC_CREATE_WINDOW => handle_create_window(&msg),
            c.GFX_IPC_DESTROY_WINDOW => handle_destroy_window(&msg),
            c.GFX_IPC_RESIZE_WINDOW => handle_resize_window(&msg),
            c.GFX_IPC_ALLOC_SHARED_BUFFER => handle_alloc_shared_buffer(&msg),
            c.GFX_IPC_RELEASE_SHARED_BUFFER => handle_release_shared_buffer(&msg),
            c.GFX_IPC_PRESENT_WINDOW => handle_present_window(&msg),
            c.GFX_IPC_POLL_EVENT => handle_poll_event(&msg),
            else => reply_unsupported(&msg),
        }
    }
}
