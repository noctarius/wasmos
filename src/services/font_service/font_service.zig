const c = @cImport({
    @cInclude("font_service_imports.h");
});
const sys = @import("libsys");

const IPC_OK: i32 = 0;
const IPC_EMPTY: i32 = 1;
const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;
const REQ_BASE: u32 = 0xA000;
const PM_FS_BUFFER_SIZE: usize = 256 * 1024;
const MAX_FONTS: usize = 3;
const MAX_HANDLES: usize = 16;
const O_RDONLY: u32 = 0;
const RASTER_SCRATCH_BYTES: usize = 4096;

const font_handle_t = struct {
    in_use: bool = false,
    owner_endpoint: u32 = IPC_ENDPOINT_NONE,
    handle_id: u32 = 0,
    font_id: u32 = 0,
    px_size: u32 = 0,
};

const loaded_font_t = struct {
    available: bool = false,
    font_id: u32 = 0,
    shmem_id: u32 = 0,
    ptr: ?[*]const u8 = null,
    len: usize = 0,
    font_info: c.stbtt_fontinfo = undefined,
    font_info_ready: bool = false,
    units_per_em: u16 = 0,
    ascent: i16 = 0,
    descent: i16 = 0,
    line_gap: i16 = 0,
};

var g_api: ?*c.wasmos_driver_api_t = null;
var g_proc_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_font_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_fs_endpoint: u32 = IPC_ENDPOINT_NONE;
var g_req_id: u32 = REQ_BASE;
var g_next_handle_id: u32 = 1;
var g_fonts: [MAX_FONTS]loaded_font_t = [_]loaded_font_t{.{}} ** MAX_FONTS;
var g_handles: [MAX_HANDLES]font_handle_t = [_]font_handle_t{.{}} ** MAX_HANDLES;
var g_raster_scratch_shmem_id: u32 = 0;
var g_raster_scratch_ptr: ?[*]u8 = null;
var g_raster_scratch_cap: usize = 0;
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
    var buf: [96]u8 = undefined;
    var hx: [16]u8 = undefined;
    var n: usize = 0;
    var i: usize = 0;
    while (i < prefix.len and n < buf.len) : (i += 1) {
        buf[n] = prefix[i];
        n += 1;
    }
    const hex_len = sys.hexU32(v, hx[0..]);
    if (hex_len == 0 or n + hex_len + 1 >= buf.len) return;
    i = 0;
    while (i < hex_len and n < buf.len) : (i += 1) {
        buf[n] = hx[i];
        n += 1;
    }
    buf[n] = '\n';
    n += 1;
    _ = api().console_write.?(buf[0..n].ptr, @intCast(n));
}

fn svc_register(name: []const u8, request_id: u32) i32 {
    return sys.svcRegister(api(),
        g_proc_endpoint,
        g_font_endpoint,
        name,
        request_id,
    );
}

fn svc_lookup(name: []const u8, request_id: u32) i32 {
    return sys.svcLookup(api(),
        g_proc_endpoint,
        g_font_endpoint,
        name,
        request_id,
    );
}

fn ipc_call(destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out: *c.nd_ipc_message_t) i32 {
    const cb_store = struct {
        fn onResolve(user: ?*anyopaque, msg_raw: ?*const anyopaque) callconv(.c) void {
            const state: *struct { done: bool, resp: c.nd_ipc_message_t } = @ptrCast(@alignCast(user.?));
            const msg: *const c.nd_ipc_message_t = @ptrCast(@alignCast(msg_raw.?));
            state.done = true;
            state.resp = msg.*;
        }
    }.onResolve;
    var state = struct { done: bool, resp: c.nd_ipc_message_t }{ .done = false, .resp = undefined };
    if (sys.intentSendWithRequestId(&g_ipc_loop, destination, g_font_endpoint, request_id, msg_type, arg0, arg1, arg2, arg3, cb_store, @ptrCast(&state)) != 0) {
        return -1;
    }
    var empty_polls: u32 = 0;
    while (!state.done) {
        const handled = sys.eventLoopPoll(&g_ipc_loop, 8);
        if (handled < 0) return -1;
        if (handled == 0) {
            if (empty_polls >= 1024) return -1;
            empty_polls +%= 1;
            api().sched_yield.?();
        }
    }
    out.* = state.resp;
    return 0;
}

fn fs_borrow_rw() ?[*]u8 {
    const p = api().buffer_borrow.?(
        c.ND_BUFFER_KIND_FS,
        ctxId(),
        c.ND_BUFFER_BORROW_READ | c.ND_BUFFER_BORROW_WRITE,
        PM_FS_BUFFER_SIZE,
    );
    if (p == null) return null;
    return @ptrCast(@alignCast(p.?));
}

fn fs_release() void {
    _ = api().buffer_release.?(c.ND_BUFFER_KIND_FS);
}

fn parse_ttf_metrics(f: *loaded_font_t) bool {
    if (f.ptr == null or f.len < 12) return false;
    const data: []const u8 = f.ptr.?[0..f.len];

    const head_off = sys.findTable(data, .{ 'h', 'e', 'a', 'd' }) orelse return false;
    const hhea_off = sys.findTable(data, .{ 'h', 'h', 'e', 'a' }) orelse return false;

    const upem = sys.beU16(data, head_off + 18) orelse return false;
    const asc = sys.beI16(data, hhea_off + 4) orelse return false;
    const desc = sys.beI16(data, hhea_off + 6) orelse return false;
    const gap = sys.beI16(data, hhea_off + 8) orelse return false;

    if (upem == 0) return false;
    f.units_per_em = upem;
    f.ascent = asc;
    f.descent = desc;
    f.line_gap = gap;
    return true;
}

fn log_path_issue(prefix: []const u8, path: []const u8) void {
    logMsg(prefix);
    logMsg(path);
    logMsg("\n");
}

fn close_fd_best_effort(fd: i32, reply: *c.nd_ipc_message_t) void {
    const req_close = g_req_id;
    g_req_id +%= 1;
    _ = ipc_call(g_fs_endpoint, req_close, c.FS_IPC_CLOSE_REQ, @bitCast(fd), 0, 0, 0, reply);
}

fn read_fd_into_shmem(fd: i32, path: []const u8, dst: [*]u8) usize {
    var reply: c.nd_ipc_message_t = undefined;
    var total: usize = 0;
    while (total < PM_FS_BUFFER_SIZE) {
        const req_read = g_req_id;
        g_req_id +%= 1;
        const remaining = PM_FS_BUFFER_SIZE - total;
        const chunk_req: u32 = @intCast(if (remaining > 4096) 4096 else remaining);
        if (ipc_call(g_fs_endpoint, req_read, c.FS_IPC_READ_REQ, @bitCast(fd), chunk_req, 0, 0, &reply) != 0 or reply.type != c.FS_IPC_RESP) {
            log_path_issue("[font] read call failed: ", path);
            break;
        }
        const got_i32: i32 = @bitCast(reply.arg0);
        if (got_i32 <= 0) break;
        const got: usize = @intCast(got_i32);
        if (got > chunk_req) break;
        if (got > 0) {
            const rc = sys.bufferCopyFrom(api(), c.ND_BUFFER_KIND_FS, ctxId(), c.ND_BUFFER_BORROW_READ | c.ND_BUFFER_BORROW_WRITE, dst + total, @intCast(got), 0);
            if (rc != 0) break;
            total += got;
        }
        if (got < chunk_req) break;
    }
    return total;
}

fn read_file_into_shmem(path: []const u8, out_shmem_id: *u32, out_ptr: *[*]u8, out_len: *usize) i32 {
    if (g_fs_endpoint == IPC_ENDPOINT_NONE) return -1;
    const fs_buf_path = fs_borrow_rw() orelse {
        logMsg("[font] fs buffer borrow failed\n");
        return -1;
    };
    if (path.len == 0 or path.len + 1 >= PM_FS_BUFFER_SIZE) return -1;
    sys.byteCopy(fs_buf_path, path.ptr, path.len);
    fs_buf_path[path.len] = 0;

    var reply: c.nd_ipc_message_t = undefined;
    const req_open = g_req_id;
    g_req_id +%= 1;
    if (ipc_call(g_fs_endpoint, req_open, c.FS_IPC_OPEN_REQ, @intCast(path.len), O_RDONLY, 0, 0, &reply) != 0) {
        log_path_issue("[font] open call failed: ", path);
        return -1;
    }
    if (reply.type != c.FS_IPC_RESP or @as(i32, @bitCast(reply.arg0)) < 0) {
        log_path_issue("[font] open failed: ", path);
        return -1;
    }
    fs_release();
    const fd: i32 = @bitCast(reply.arg0);

    const pages = PM_FS_BUFFER_SIZE / 4096;
    var shmem_id: u32 = 0;
    var mapped_ptr: ?*anyopaque = null;
    if (api().shmem_create.?(pages, 0, &shmem_id, @ptrCast(&mapped_ptr)) != 0 or shmem_id == 0 or mapped_ptr == null) {
        close_fd_best_effort(fd, &reply);
        return -1;
    }
    const dst: [*]u8 = @ptrCast(@alignCast(mapped_ptr.?));
    const total = read_fd_into_shmem(fd, path, dst);

    close_fd_best_effort(fd, &reply);
    if (total == 0) {
        log_path_issue("[font] read empty: ", path);
        return -1;
    }

    out_shmem_id.* = shmem_id;
    out_ptr.* = dst;
    out_len.* = total;
    return 0;
}

fn font_slot_by_id(font_id: u32) ?usize {
    var i: usize = 0;
    while (i < g_fonts.len) : (i += 1) {
        if (g_fonts[i].available and g_fonts[i].font_id == font_id) return i;
    }
    return null;
}

fn handle_slot_by_id(handle_id: u32) ?usize {
    var i: usize = 0;
    while (i < g_handles.len) : (i += 1) {
        if (g_handles[i].in_use and g_handles[i].handle_id == handle_id) return i;
    }
    return null;
}

fn scaled_i16(v: i16, px: u32, upem: u16) i32 {
    const num: i64 = @as(i64, v) * @as(i64, @intCast(px));
    return @intCast(@divTrunc(num, @as(i64, upem)));
}

fn scaled_i32(v: i32, px: u32, upem: u16) i32 {
    const num: i64 = @as(i64, v) * @as(i64, @intCast(px));
    return @intCast(@divTrunc(num, @as(i64, upem)));
}

fn clamp_i32(v: i32, lo: i32, hi: i32) i32 {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

const text_metrics_t = struct {
    width: i32 = 0,
    height: i32 = 0,
    x0: i32 = 0,
    y0: i32 = 0,
    advance_x: i32 = 0,
};

fn compute_text_metrics(f: *const loaded_font_t, px_size: u32, text_ptr: [*]const u8, text_n: usize) text_metrics_t {
    var out: text_metrics_t = .{};
    var pen_x: i32 = 0;
    var min_x: i32 = 0;
    var min_y: i32 = 0;
    var max_x: i32 = 0;
    var max_y: i32 = 0;
    var has_bounds = false;
    var i: usize = 0;
    while (i < text_n) : (i += 1) {
        const cp: u32 = text_ptr[i];
        if (cp == 0) break;

        const scale = c.stbtt_ScaleForPixelHeight(&f.font_info, @floatFromInt(px_size));
        var x0: c_int = 0;
        var y0: c_int = 0;
        var x1: c_int = 0;
        var y1: c_int = 0;
        c.stbtt_GetCodepointBitmapBox(&f.font_info, @bitCast(cp), scale, scale, &x0, &y0, &x1, &y1);
        const w: i32 = x1 - x0;
        const hgt: i32 = y1 - y0;
        if (w > 0 and hgt > 0) {
            const gx0 = pen_x + x0;
            const gy0 = y0;
            const gx1 = pen_x + x1;
            const gy1 = y1;
            if (!has_bounds) {
                min_x = gx0;
                min_y = gy0;
                max_x = gx1;
                max_y = gy1;
                has_bounds = true;
            } else {
                if (gx0 < min_x) min_x = gx0;
                if (gy0 < min_y) min_y = gy0;
                if (gx1 > max_x) max_x = gx1;
                if (gy1 > max_y) max_y = gy1;
            }
        }

        var adv_units: c_int = 0;
        var lsb: c_int = 0;
        c.stbtt_GetCodepointHMetrics(&f.font_info, @bitCast(cp), &adv_units, &lsb);
        var advance_x = scaled_i32(adv_units, px_size, f.units_per_em);
        if (i + 1 < text_n) {
            const next_cp: u32 = text_ptr[i + 1];
            if (next_cp != 0) {
                const kern_units = c.stbtt_GetCodepointKernAdvance(&f.font_info, @bitCast(cp), @bitCast(next_cp));
                advance_x += scaled_i32(kern_units, px_size, f.units_per_em);
            }
        }
        pen_x += advance_x;
    }

    out.width = if (has_bounds) (max_x - min_x) else 0;
    out.height = if (has_bounds) (max_y - min_y) else 0;
    out.x0 = if (has_bounds) min_x else 0;
    out.y0 = if (has_bounds) min_y else 0;
    out.advance_x = pen_x;
    return out;
}

fn reply_with_status(req: *const c.nd_ipc_message_t, status: i32, arg1: u32, arg2: u32, arg3: u32) void {
    if (req.source == IPC_ENDPOINT_NONE or req.request_id == 0) return;
    var resp: c.nd_ipc_message_t = undefined;
    resp.type = if (status == c.FONT_STATUS_OK) c.FONT_IPC_RESP else c.FONT_IPC_ERROR;
    resp.source = g_font_endpoint;
    resp.destination = req.source;
    resp.request_id = req.request_id;
    resp.arg0 = @bitCast(status);
    resp.arg1 = arg1;
    resp.arg2 = arg2;
    resp.arg3 = arg3;
    _ = api().ipc_send.?(ctxId(), req.source, &resp);
}

fn handle_open_font(req: *const c.nd_ipc_message_t) void {
    const font_id = req.arg0;
    const px_size = req.arg1;
    if (px_size == 0 or px_size > 256) {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    }
    if (font_slot_by_id(font_id) == null) {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    }

    var i: usize = 0;
    while (i < g_handles.len) : (i += 1) {
        if (g_handles[i].in_use) continue;
        const hid = g_next_handle_id;
        g_next_handle_id +%= 1;
        if (g_next_handle_id == 0) g_next_handle_id = 1;
        g_handles[i] = .{
            .in_use = true,
            .owner_endpoint = req.source,
            .handle_id = hid,
            .font_id = font_id,
            .px_size = px_size,
        };
        reply_with_status(req, c.FONT_STATUS_OK, hid, 0, 0);
        return;
    }

    reply_with_status(req, c.FONT_STATUS_BUSY, 0, 0, 0);
}

fn handle_get_metrics(req: *const c.nd_ipc_message_t) void {
    const handle_id = req.arg0;
    const hs = handle_slot_by_id(handle_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const h = g_handles[hs];
    if (h.owner_endpoint != req.source) {
        reply_with_status(req, c.FONT_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    const fs = font_slot_by_id(h.font_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const f = g_fonts[fs];
    if (!f.available or f.units_per_em == 0) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    const asc = scaled_i16(f.ascent, h.px_size, f.units_per_em);
    const desc = scaled_i16(f.descent, h.px_size, f.units_per_em);
    const gap = scaled_i16(f.line_gap, h.px_size, f.units_per_em);
    reply_with_status(req, c.FONT_STATUS_OK, @bitCast(asc), @bitCast(desc), @bitCast(gap));
}

fn handle_raster_glyph(req: *const c.nd_ipc_message_t) void {
    const handle_id = req.arg0;
    const codepoint = req.arg1;
    const hs = handle_slot_by_id(handle_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const h = g_handles[hs];
    if (h.owner_endpoint != req.source) {
        reply_with_status(req, c.FONT_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    const fs = font_slot_by_id(h.font_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const f = &g_fonts[fs];
    if (!f.available or !f.font_info_ready) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }

    const scale = c.stbtt_ScaleForPixelHeight(&f.font_info, @floatFromInt(h.px_size));
    c.wasmos_stbtt_alloc_reset();
    var x0: c_int = 0;
    var y0: c_int = 0;
    var x1: c_int = 0;
    var y1: c_int = 0;
    c.stbtt_GetCodepointBitmapBox(&f.font_info, @bitCast(codepoint), scale, scale, &x0, &y0, &x1, &y1);
    const w: i32 = x1 - x0;
    const hgt: i32 = y1 - y0;
    if (w <= 0 or hgt <= 0) {
        reply_with_status(req, c.FONT_STATUS_OK, 0, 0, sys.packS16Pair(x0, y0));
        return;
    }

    const pixel_count: usize = @intCast(w * hgt);
    if (pixel_count > RASTER_SCRATCH_BYTES) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }

    if (g_raster_scratch_ptr == null or g_raster_scratch_shmem_id == 0 or g_raster_scratch_cap < pixel_count) {
        var scratch_id: u32 = 0;
        var scratch_ptr_raw: ?*anyopaque = null;
        const pages = (pixel_count + 4095) / 4096;
        if (api().shmem_create.?(pages, 0, &scratch_id, @ptrCast(&scratch_ptr_raw)) != 0 or scratch_id == 0 or scratch_ptr_raw == null) {
            reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
            return;
        }
        g_raster_scratch_shmem_id = scratch_id;
        g_raster_scratch_ptr = @ptrCast(@alignCast(scratch_ptr_raw.?));
        g_raster_scratch_cap = pages * 4096;
    }

    var owner_context_id: u32 = 0;
    if (api().ipc_endpoint_owner == null or
        api().ipc_endpoint_owner.?(req.source, &owner_context_id) != 0 or
        owner_context_id == 0)
    {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    if (api().shmem_grant == null or
        api().shmem_grant.?(g_raster_scratch_shmem_id, owner_context_id) != 0)
    {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }

    const dst: [*]u8 = g_raster_scratch_ptr.?;
    c.stbtt_MakeCodepointBitmap(&f.font_info, dst, @intCast(w), @intCast(hgt), @intCast(w), scale, scale, @bitCast(codepoint));
    reply_with_status(req, c.FONT_STATUS_OK, g_raster_scratch_shmem_id, sys.packU16Pair(@intCast(w), @intCast(hgt)), sys.packS16Pair(x0, y0));
}

fn handle_measure_text(req: *const c.nd_ipc_message_t) void {
    const handle_id = req.arg0;
    const text_shmem_id = req.arg1;
    const text_len = req.arg2;
    const hs = handle_slot_by_id(handle_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const h = g_handles[hs];
    if (h.owner_endpoint != req.source) {
        reply_with_status(req, c.FONT_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    const fs = font_slot_by_id(h.font_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const f = &g_fonts[fs];
    if (!f.available or !f.font_info_ready or f.units_per_em == 0) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    if (text_shmem_id == 0 or text_len == 0) {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    }

    const text_ptr_raw = api().shmem_map.?(text_shmem_id);
    if (text_ptr_raw == null) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    defer _ = api().shmem_unmap.?(text_shmem_id);
    const text_ptr: [*]const u8 = @ptrCast(@alignCast(text_ptr_raw.?));
    const text_n: usize = @intCast(text_len);

    const metrics = compute_text_metrics(f, h.px_size, text_ptr, text_n);
    if (text_n > 0 and metrics.width == 0 and metrics.height == 0 and metrics.advance_x == 0) {
        logMsg("[dbg-font] measure-zero len/b0/b1/font/px\n");
        logHex32("  len=", @intCast(text_n));
        logHex32("  b0=", text_ptr[0]);
        logHex32("  b1=", if (text_n > 1) text_ptr[1] else 0);
        logHex32("  font_id=", h.font_id);
        logHex32("  px=", h.px_size);
    }
    const w16: u16 = @intCast(clamp_i32(metrics.width, 0, 0xFFFF));
    const h16: u16 = @intCast(clamp_i32(metrics.height, 0, 0xFFFF));
    const x016: i16 = @intCast(clamp_i32(metrics.x0, -32768, 32767));
    const y016: i16 = @intCast(clamp_i32(metrics.y0, -32768, 32767));
    reply_with_status(req, c.FONT_STATUS_OK, sys.packU16Pair(w16, h16), sys.packS16Pair(x016, y016), @bitCast(metrics.advance_x));
}

fn handle_raster_text_into(req: *const c.nd_ipc_message_t) void {
    const handle_id = req.arg0;
    const text_shmem_id = req.arg1;
    const text_len = req.arg2;
    const dst_shmem_id = req.arg3;
    const hs = handle_slot_by_id(handle_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const h = g_handles[hs];
    if (h.owner_endpoint != req.source) {
        reply_with_status(req, c.FONT_STATUS_PERMISSION, 0, 0, 0);
        return;
    }
    const fs = font_slot_by_id(h.font_id) orelse {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    };
    const f = &g_fonts[fs];
    if (!f.available or !f.font_info_ready or f.units_per_em == 0) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    if (text_shmem_id == 0 or text_len == 0 or dst_shmem_id == 0) {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    }

    const text_ptr_raw = api().shmem_map.?(text_shmem_id);
    if (text_ptr_raw == null) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    defer _ = api().shmem_unmap.?(text_shmem_id);
    const text_ptr: [*]const u8 = @ptrCast(@alignCast(text_ptr_raw.?));
    const text_n: usize = @intCast(text_len);

    const metrics = compute_text_metrics(f, h.px_size, text_ptr, text_n);
    const out_w = metrics.width;
    const out_h = metrics.height;
    if (out_w <= 0 or out_h <= 0) {
        const w16: u16 = @intCast(clamp_i32(out_w, 0, 0xFFFF));
        const h16: u16 = @intCast(clamp_i32(out_h, 0, 0xFFFF));
        const x016: i16 = @intCast(clamp_i32(metrics.x0, -32768, 32767));
        const y016: i16 = @intCast(clamp_i32(metrics.y0, -32768, 32767));
        reply_with_status(req, c.FONT_STATUS_OK, sys.packU16Pair(w16, h16), sys.packS16Pair(x016, y016), @bitCast(metrics.advance_x));
        return;
    }

    const dst_ptr_raw = api().shmem_map.?(dst_shmem_id);
    if (dst_ptr_raw == null) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    defer _ = api().shmem_unmap.?(dst_shmem_id);
    const dst: [*]u8 = @ptrCast(@alignCast(dst_ptr_raw.?));
    const out_size_i32 = out_w * out_h;
    if (out_size_i32 <= 0) {
        reply_with_status(req, c.FONT_STATUS_INVALID, 0, 0, 0);
        return;
    }
    const out_size: usize = @intCast(out_size_i32);
    @memset(dst[0..out_size], 0);

    const scale = c.stbtt_ScaleForPixelHeight(&f.font_info, @floatFromInt(h.px_size));
    c.wasmos_stbtt_alloc_reset();
    var pen_x: i32 = 0;
    var i: usize = 0;
    while (i < text_n) : (i += 1) {
        const cp: u32 = text_ptr[i];
        if (cp == 0) break;

        var x0: c_int = 0;
        var y0: c_int = 0;
        var x1: c_int = 0;
        var y1: c_int = 0;
        c.stbtt_GetCodepointBitmapBox(&f.font_info, @bitCast(cp), scale, scale, &x0, &y0, &x1, &y1);
        const w: i32 = x1 - x0;
        const hgt: i32 = y1 - y0;
        if (w > 0 and hgt > 0) {
            const pixel_count_i32 = w * hgt;
            if (pixel_count_i32 <= 0) {
                reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
                return;
            }
            const pixel_count: usize = @intCast(pixel_count_i32);
            if (pixel_count > RASTER_SCRATCH_BYTES) {
                reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
                return;
            }
            var scratch: [RASTER_SCRATCH_BYTES]u8 = undefined;
            c.stbtt_MakeCodepointBitmap(&f.font_info, &scratch[0], @intCast(w), @intCast(hgt), @intCast(w), scale, scale, @bitCast(cp));
            const dst_x = pen_x + x0 - metrics.x0;
            const dst_y = y0 - metrics.y0;
            var gy: i32 = 0;
            while (gy < hgt) : (gy += 1) {
                const py = dst_y + gy;
                if (py < 0 or py >= out_h) continue;
                var gx: i32 = 0;
                while (gx < w) : (gx += 1) {
                    const px = dst_x + gx;
                    if (px < 0 or px >= out_w) continue;
                    const src_idx: usize = @intCast(gy * w + gx);
                    const dst_idx: usize = @intCast(py * out_w + px);
                    const src_a = scratch[src_idx];
                    if (src_a > dst[dst_idx]) dst[dst_idx] = src_a;
                }
            }
        }

        var adv_units: c_int = 0;
        var lsb: c_int = 0;
        c.stbtt_GetCodepointHMetrics(&f.font_info, @bitCast(cp), &adv_units, &lsb);
        var advance_x = scaled_i32(adv_units, h.px_size, f.units_per_em);
        if (i + 1 < text_n) {
            const next_cp: u32 = text_ptr[i + 1];
            if (next_cp != 0) {
                const kern_units = c.stbtt_GetCodepointKernAdvance(&f.font_info, @bitCast(cp), @bitCast(next_cp));
                advance_x += scaled_i32(kern_units, h.px_size, f.units_per_em);
            }
        }
        pen_x += advance_x;
    }

    var nz: usize = 0;
    var max_a: u8 = 0;
    var k: usize = 0;
    while (k < out_size) : (k += 1) {
        const a = dst[k];
        if (a != 0) nz += 1;
        if (a > max_a) max_a = a;
    }
    if (out_size > 0 and nz == 0) {
        logMsg("[dbg-font] raster-zero out_size/font/px/b0/b1\n");
        logHex32("  out_size=", @intCast(out_size));
        logHex32("  font_id=", h.font_id);
        logHex32("  px=", h.px_size);
        logHex32("  b0=", text_ptr[0]);
        logHex32("  b1=", if (text_n > 1) text_ptr[1] else 0);
    }

    const w16: u16 = @intCast(clamp_i32(metrics.width, 0, 0xFFFF));
    const h16: u16 = @intCast(clamp_i32(metrics.height, 0, 0xFFFF));
    const x016: i16 = @intCast(clamp_i32(metrics.x0, -32768, 32767));
    const y016: i16 = @intCast(clamp_i32(metrics.y0, -32768, 32767));
    if (api().shmem_flush.?(dst_shmem_id, @ptrCast(dst), @intCast(out_size)) != 0) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    reply_with_status(req, c.FONT_STATUS_OK, sys.packU16Pair(w16, h16), sys.packS16Pair(x016, y016), @bitCast(metrics.advance_x));
}

fn handle_font_ipc_message(msg: *const c.nd_ipc_message_t) void {
    switch (msg.type) {
        c.FONT_IPC_OPEN_FONT_REQ => handle_open_font(msg),
        c.FONT_IPC_GET_METRICS_REQ => handle_get_metrics(msg),
        c.FONT_IPC_RASTER_GLYPH_REQ => handle_raster_glyph(msg),
        c.FONT_IPC_MEASURE_GLYPH_REQ => handle_measure_text(msg),
        c.FONT_IPC_RASTER_GLYPH_INTO_REQ => handle_raster_text_into(msg),
        else => {
            logHex32("[font] warning: unhandled event type ", msg.type);
            reply_with_status(msg, c.FONT_STATUS_UNSUPPORTED, 0, 0, 0);
        },
    }
}

fn register_ipc_handlers() i32 {
    const cb = struct {
        fn onMessage(user: ?*anyopaque, msg_raw: ?*const anyopaque) callconv(.c) void {
            _ = user;
            if (msg_raw) |m| {
                const msg: *const c.nd_ipc_message_t = @ptrCast(@alignCast(m));
                handle_font_ipc_message(msg);
            }
        }
    }.onMessage;
    if (sys.eventRegister(&g_ipc_loop, c.FONT_IPC_OPEN_FONT_REQ, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.FONT_IPC_GET_METRICS_REQ, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.FONT_IPC_RASTER_GLYPH_REQ, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.FONT_IPC_MEASURE_GLYPH_REQ, cb, null) != 0) return -1;
    if (sys.eventRegister(&g_ipc_loop, c.FONT_IPC_RASTER_GLYPH_INTO_REQ, cb, null) != 0) return -1;
    if (sys.eventSetDefault(&g_ipc_loop, cb, null) != 0) return -1;
    return 0;
}

fn load_builtin_fonts() void {
    const primary_paths = [_][]const u8{
        "/boot/system/fonts/roboto.ttf",
        "/boot/system/fonts/roboto_mono.ttf",
        "/boot/system/fonts/roboto_serif.ttf",
    };
    const ids = [_]u32{ c.FONT_ID_ROBOTO, c.FONT_ID_ROBOTO_MONO, c.FONT_ID_NOTO_SERIF };
    const labels = [_][]const u8{ "roboto", "roboto-mono", "roboto-serif" };

    var i: usize = 0;
    while (i < primary_paths.len and i < g_fonts.len) : (i += 1) {
        logMsg("[font] loading ");
        logMsg(labels[i]);
        logMsg("\n");
        var sid: u32 = 0;
        var ptr: [*]u8 = undefined;
        var len: usize = 0;
        if (read_file_into_shmem(primary_paths[i], &sid, &ptr, &len) != 0)
        {
            logMsg("[font] load failed\n");
            continue;
        }
        g_fonts[i] = .{
            .available = true,
            .font_id = ids[i],
            .shmem_id = sid,
            .ptr = @ptrCast(ptr),
            .len = len,
            .font_info = undefined,
            .font_info_ready = false,
            .units_per_em = 0,
            .ascent = 0,
            .descent = 0,
            .line_gap = 0,
        };
        const offset = c.stbtt_GetFontOffsetForIndex(@ptrCast(ptr), 0);
        if (offset < 0 or c.stbtt_InitFont(&g_fonts[i].font_info, @ptrCast(ptr), offset) == 0) {
            g_fonts[i].available = false;
            logMsg("[font] stb init failed\n");
            continue;
        }
        g_fonts[i].font_info_ready = true;
        if (!parse_ttf_metrics(&g_fonts[i])) {
            g_fonts[i].available = false;
            logMsg("[font] metrics parse failed\n");
            continue;
        }
        logMsg("[font] loaded ok\n");
    }
}

pub export fn initialize(driver_api: *c.wasmos_driver_api_t, module_count: c_int, arg2: c_int, arg3: c_int) c_int {
    _ = arg2;
    _ = arg3;

    g_api = driver_api;
    if (driver_api.abi_magic != c.WASMOS_NATIVE_ABI_MAGIC or driver_api.abi_version != c.WASMOS_NATIVE_ABI_VERSION) {
        return -2;
    }

    g_proc_endpoint = @bitCast(module_count);
    if (g_proc_endpoint == IPC_ENDPOINT_NONE) return -1;

    g_font_endpoint = api().ipc_create_endpoint.?();
    if (g_font_endpoint == IPC_ENDPOINT_NONE) return -1;
    sys.eventLoopInit(&g_ipc_loop, api(), g_font_endpoint, REQ_BASE + 0x2000);
    if (register_ipc_handlers() != 0) return -1;

    if (svc_register("font", 1) != 0) {
        logMsg("[font] register failed\n");
        return -1;
    }

    const fs_ep = sys.svcLookupRetry(api(), g_proc_endpoint, g_font_endpoint, "fs.vfs", 2, 64);
    if (fs_ep >= 0) {
        g_fs_endpoint = @bitCast(fs_ep);
    }

    if (g_fs_endpoint == IPC_ENDPOINT_NONE) {
        logMsg("[font] fs endpoint unavailable\n");
    } else {
        load_builtin_fonts();
        logMsg("[font] service ready\n");
    }

    while (true) {
        const handled = sys.eventLoopPoll(&g_ipc_loop, 32);
        if (handled < 0) return -1;
        if (handled == 0) {
            api().sched_yield.?();
            continue;
        }
    }
}
