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
const GFX_WINDOW_MIN_DIM: u32 = 1;
const GFX_WINDOW_MAX_DIM: u32 = 8192;
const PAGE_SIZE: u64 = 4096;

var g_api: ?*c.wasmos_driver_api_t = null;
var g_proc_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_gfx_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_fb_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_next_window_id: u32 = 1;
var g_next_z: u32 = 1;
var g_rng_state: u32 = 0xA5A5_5A5A;
var g_damage_marker_logged: bool = false;

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
    x: i32 = 0,
    y: i32 = 0,
    width: u32 = 0,
    height: u32 = 0,
    z: u32 = 0,
    current_buffer_id: u32 = 0,
};

const buffer_slot_t = struct {
    in_use: bool = false,
    owner_endpoint: u32 = IPC_ENDPOINT_NONE,
    buffer_id: u32 = 0,
    shmem_id: u32 = 0,
    width: u32 = 0,
    height: u32 = 0,
    stride_bytes: u32 = 0,
};

var g_windows: [GFX_MAX_WINDOWS]window_slot_t = [_]window_slot_t{.{}} ** GFX_MAX_WINDOWS;
var g_buffers: [GFX_MAX_BUFFERS]buffer_slot_t = [_]buffer_slot_t{.{}} ** GFX_MAX_BUFFERS;

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
        return null;
    }
    if (mapped_ptr != null) {
        _ = api().shmem_unmap.?(shmem_id);
    }

    const idx = slot_idx.?;
    g_buffers[idx].in_use = true;
    g_buffers[idx].owner_endpoint = owner_endpoint;
    g_buffers[idx].buffer_id = buffer_id;
    g_buffers[idx].shmem_id = shmem_id;
    g_buffers[idx].width = width;
    g_buffers[idx].height = height;
    g_buffers[idx].stride_bytes = @intCast(stride_u64);
    return idx;
}

fn fill_rect(x0: i32, y0: i32, w: i32, h: i32, color: u32) void {
    if (!g_fb_info_valid or w <= 0 or h <= 0) return;
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

    var y = sy;
    while (y < ey) : (y += 1) {
        var x = sx;
        while (x < ex) : (x += 1) {
            _ = api().framebuffer_pixel.?(@intCast(x), @intCast(y), color);
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

fn draw_window_placeholder(win: window_slot_t, clip: c.gfx_rect_t) void {
    const tone = @as(u32, (win.window_id * 37) & 0x7F);
    const body_color = 0x203040 | (tone << 16) | ((tone >> 1) << 8);
    fill_rect(clip.x, clip.y, clip.w, clip.h, body_color);
}

fn draw_window_buffer(win: window_slot_t, buf: buffer_slot_t, clip: c.gfx_rect_t) bool {
    const src_ptr_raw = api().shmem_map.?(buf.shmem_id);
    if (src_ptr_raw == null) return false;
    defer _ = api().shmem_unmap.?(buf.shmem_id);

    const src_pixels: [*]const u32 = @ptrCast(@alignCast(src_ptr_raw.?));

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
            const idx: usize = @intCast(idx_u64);
            _ = api().framebuffer_pixel.?(@intCast(clip.x + x), @intCast(clip.y + y), src_pixels[idx]);
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
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    g_windows[slot_idx] = .{};
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
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
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
            g_windows[window_idx].current_buffer_id = buf.buffer_id;
        }
    }

    reply_with_status(msg, c.GFX_STATUS_OK, buf.buffer_id, buf.shmem_id, buf.stride_bytes);
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
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }

    const buf_idx = buffer_find_by_id(buffer_id) orelse {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const buf = g_buffers[buf_idx];
    if (buf.owner_endpoint != msg.source) {
        reply_with_status(msg, c.GFX_STATUS_PERMISSION, 0, 0, 0);
        return;
    }

    if (buf.width < g_windows[window_idx].width or buf.height < g_windows[window_idx].height) {
        reply_with_status(msg, c.GFX_STATUS_INVALID, 0, 0, 0);
        return;
    }

    g_windows[window_idx].current_buffer_id = buffer_id;

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
        if (rc != IPC_OK) return -1;

        if (!gfx_header_valid(msg.arg2, msg.arg3)) {
            reply_unsupported(&msg);
            continue;
        }

        switch (gfx_header_opcode(msg.arg3)) {
            c.GFX_IPC_CREATE_WINDOW => handle_create_window(&msg),
            c.GFX_IPC_DESTROY_WINDOW => handle_destroy_window(&msg),
            c.GFX_IPC_RESIZE_WINDOW => handle_resize_window(&msg),
            c.GFX_IPC_ALLOC_SHARED_BUFFER => handle_alloc_shared_buffer(&msg),
            c.GFX_IPC_PRESENT_WINDOW => handle_present_window(&msg),
            else => reply_unsupported(&msg),
        }
    }
}
