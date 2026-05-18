const c = @cImport({
    @cInclude("wasmos_native_driver.h");
    @cInclude("wasmos_driver_abi.h");
    @cInclude("wasmos/gfx_ipc.h");
});

const IPC_OK: i32 = 0;
const IPC_EMPTY: i32 = 1;
const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;
const GFX_REQUEST_BASE: u32 = 0x7000;
const GFX_FB_LOOKUP_RETRIES: u32 = 2048;
const GFX_MAX_WINDOWS: usize = 32;
const GFX_WINDOW_MIN_DIM: u32 = 1;
const GFX_WINDOW_MAX_DIM: u32 = 8192;

var g_api: ?*c.wasmos_driver_api_t = null;
var g_proc_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_gfx_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_fb_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_next_window_id: u32 = 1;
var g_fb_info: c.nd_framebuffer_info_t = .{
    .framebuffer_base = 0,
    .framebuffer_size = 0,
    .framebuffer_width = 0,
    .framebuffer_height = 0,
    .framebuffer_stride = 0,
    .framebuffer_reserved = 0,
};
var g_fb_info_valid: bool = false;

const window_slot_t = struct {
    in_use: bool = false,
    owner_endpoint: u32 = IPC_ENDPOINT_NONE,
    window_id: u32 = 0,
    width: u32 = 0,
    height: u32 = 0,
};

var g_windows: [GFX_MAX_WINDOWS]window_slot_t = [_]window_slot_t{.{}} ** GFX_MAX_WINDOWS;

fn api() *c.wasmos_driver_api_t {
    return g_api.?;
}

fn ctxId() u32 {
    return api().sched_current_pid.?();
}

fn logMsg(msg: []const u8) void {
    _ = api().console_write.?(msg.ptr, @intCast(msg.len));
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
        if (msg.request_id == request_id) {
            break;
        }
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
        if (msg.request_id == request_id) {
            break;
        }
    }
    if (msg.type != c.SVC_IPC_LOOKUP_RESP) {
        return -1;
    }
    if (msg.arg0 == IPC_ENDPOINT_NONE) {
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
    if (api().ipc_send.?(ctxId(), destination, &req) != IPC_OK) {
        return -1;
    }

    while (true) {
        const rc = api().ipc_recv.?(ctxId(), g_gfx_endpoint, out);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) {
            return -1;
        }
        if (out.request_id == request_id) {
            return 0;
        }
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

fn reply_unsupported(msg: *const c.nd_ipc_message_t) void {
    reply_with_status(msg, c.GFX_STATUS_UNSUPPORTED, 0, 0, 0);
}

fn reply_with_status(msg: *const c.nd_ipc_message_t, status: i32, arg1: u32, arg2: u32, arg3: u32) void {
    var resp: c.nd_ipc_message_t = undefined;
    if (msg.source == IPC_ENDPOINT_NONE or msg.request_id == 0) {
        return;
    }
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
        if (g_windows[i].in_use and g_windows[i].window_id == id) {
            return i;
        }
    }
    return null;
}

fn window_alloc(owner_endpoint: u32, width: u32, height: u32) ?usize {
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (g_windows[i].in_use) {
            continue;
        }
        g_windows[i].in_use = true;
        g_windows[i].owner_endpoint = owner_endpoint;
        g_windows[i].window_id = g_next_window_id;
        g_windows[i].width = width;
        g_windows[i].height = height;
        g_next_window_id +%= 1;
        if (g_next_window_id == 0) {
            g_next_window_id = 1;
        }
        return i;
    }
    return null;
}

fn handle_create_window(msg: *const c.nd_ipc_message_t) void {
    const width: u32 = msg.arg0;
    const height: u32 = msg.arg1;
    if (width < GFX_WINDOW_MIN_DIM or height < GFX_WINDOW_MIN_DIM or
        width > GFX_WINDOW_MAX_DIM or height > GFX_WINDOW_MAX_DIM)
    {
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
    const window_id: u32 = msg.arg0;
    if (window_id == 0) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }
    const slot_idx = window_find_by_id(window_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    if (g_windows[slot_idx].owner_endpoint != msg.source) {
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    g_windows[slot_idx] = .{};
    reply_with_status(msg, c.GFX_STATUS_OK, 0, 0, 0);
}

fn fill_rect(x0: u32, y0: u32, w: u32, h: u32, color: u32) void {
    if (w == 0 or h == 0 or !g_fb_info_valid) {
        return;
    }
    const max_w = g_fb_info.framebuffer_width;
    const max_h = g_fb_info.framebuffer_height;
    if (x0 >= max_w or y0 >= max_h) {
        return;
    }
    const x1 = @min(x0 + w, max_w);
    const y1 = @min(y0 + h, max_h);
    var y = y0;
    while (y < y1) : (y += 1) {
        var x = x0;
        while (x < x1) : (x += 1) {
            _ = api().framebuffer_pixel.?(x, y, color);
        }
    }
}

fn compose_frame() i32 {
    if (!g_fb_info_valid) {
        return c.GFX_STATUS_IO;
    }
    const width = g_fb_info.framebuffer_width;
    const height = g_fb_info.framebuffer_height;
    const tile_w: u32 = if (width > 40) width / 2 else width;
    const tile_h: u32 = if (height > 40) height / 2 else height;
    const gap: u32 = 8;
    var active_idx: u32 = 0;

    fill_rect(0, 0, width, height, 0x101820);
    var i: usize = 0;
    while (i < g_windows.len) : (i += 1) {
        if (!g_windows[i].in_use) {
            continue;
        }
        const col = active_idx & 1;
        const row = active_idx >> 1;
        const origin_x = gap + col * tile_w;
        const origin_y = gap + row * tile_h;
        const max_w = if (tile_w > gap * 2) tile_w - gap * 2 else 1;
        const max_h = if (tile_h > gap * 2) tile_h - gap * 2 else 1;
        const draw_w = @min(g_windows[i].width, max_w);
        const draw_h = @min(g_windows[i].height, max_h);
        const tone = @as(u32, (g_windows[i].window_id * 37) & 0x7F);
        const body_color = 0x203040 | (tone << 16) | ((tone >> 1) << 8);
        fill_rect(origin_x, origin_y, draw_w, draw_h, body_color);
        fill_rect(origin_x, origin_y, draw_w, 2, 0xE0E0E0);
        fill_rect(origin_x, origin_y, 2, draw_h, 0xE0E0E0);
        fill_rect(origin_x + draw_w - 2, origin_y, 2, draw_h, 0x707070);
        fill_rect(origin_x, origin_y + draw_h - 2, draw_w, 2, 0x707070);
        active_idx += 1;
    }
    return c.GFX_STATUS_OK;
}

fn handle_present_window(msg: *const c.nd_ipc_message_t) void {
    const window_id: u32 = msg.arg0;
    if (window_id == 0) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }
    const slot_idx = window_find_by_id(window_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    if (g_windows[slot_idx].owner_endpoint != msg.source) {
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    // TODO(gfx-phase1): Use client buffer IDs + damage rects from PRESENT
    // payload for real blit/composition instead of placeholder rectangles.
    reply_with_status(msg, compose_frame(), 0, 0, 0);
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
    if (g_proc_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    g_gfx_endpoint = api().ipc_create_endpoint.?();
    if (g_gfx_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    if (svc_register("gfx", 1) != 0) {
        logMsg("[gfx] register failed\n");
        return -1;
    }

    if (lookup_fb_endpoint() != 0) {
        logMsg("[gfx] fb endpoint unavailable\n");
    } else {
        log_fb_geometry_probe();
        logMsg("[test] gfx compositor handshake ok\n");
    }
    if (api().framebuffer_info.?(&g_fb_info) == 0 and
        g_fb_info.framebuffer_width != 0 and
        g_fb_info.framebuffer_height != 0)
    {
        g_fb_info_valid = true;
    }

    while (true) {
        var msg: c.nd_ipc_message_t = undefined;
        const rc = api().ipc_recv.?(ctxId(), g_gfx_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) {
            return -1;
        }
        if (!gfx_header_valid(msg.arg2, msg.arg3)) {
            reply_unsupported(&msg);
            continue;
        }
        const opcode = gfx_header_opcode(msg.arg3);
        switch (opcode) {
            c.GFX_IPC_CREATE_WINDOW => handle_create_window(&msg),
            c.GFX_IPC_DESTROY_WINDOW => handle_destroy_window(&msg),
            c.GFX_IPC_PRESENT_WINDOW => handle_present_window(&msg),
            else => reply_unsupported(&msg),
        }
    }
}
