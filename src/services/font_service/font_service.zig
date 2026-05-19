const c = @cImport({
    @cInclude("font_service_imports.h");
});

const IPC_OK: i32 = 0;
const IPC_EMPTY: i32 = 1;
const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;
const REQ_BASE: u32 = 0xA000;
const PM_FS_BUFFER_SIZE: usize = 256 * 1024;
const MAX_FONTS: usize = 3;
const MAX_HANDLES: usize = 16;

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
    msg.source = g_font_endpoint;
    msg.destination = g_proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];
    if (api().ipc_send.?(ctxId(), g_proc_endpoint, &msg) != IPC_OK) return -1;

    while (true) {
        const rc = api().ipc_recv.?(ctxId(), g_font_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) return -1;
        if (msg.request_id == request_id) break;
    }
    if (msg.type != c.SVC_IPC_REGISTER_RESP) return -1;
    return @bitCast(msg.arg0);
}

fn svc_lookup(name: []const u8, request_id: u32) i32 {
    var args: [4]u32 = undefined;
    var msg: c.nd_ipc_message_t = undefined;
    packName16(name, &args);

    msg.type = c.SVC_IPC_LOOKUP_REQ;
    msg.source = g_font_endpoint;
    msg.destination = g_proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];
    if (api().ipc_send.?(ctxId(), g_proc_endpoint, &msg) != IPC_OK) return -1;

    while (true) {
        const rc = api().ipc_recv.?(ctxId(), g_font_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) return -1;
        if (msg.request_id == request_id) break;
    }
    if (msg.type != c.SVC_IPC_LOOKUP_RESP or msg.arg0 == IPC_ENDPOINT_NONE) return -1;
    return @bitCast(msg.arg0);
}

fn ipc_call(destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out: *c.nd_ipc_message_t) i32 {
    var req: c.nd_ipc_message_t = undefined;
    req.type = msg_type;
    req.source = g_font_endpoint;
    req.destination = destination;
    req.request_id = request_id;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.arg3 = arg3;
    if (api().ipc_send.?(ctxId(), destination, &req) != IPC_OK) return -1;

    while (true) {
        const rc = api().ipc_recv.?(ctxId(), g_font_endpoint, out);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) return -1;
        if (out.request_id == request_id) return 0;
    }
}

fn fs_borrow_rw() ?[*]u8 {
    const p = api().buffer_borrow.?(c.ND_BUFFER_KIND_FS, ctxId(), c.ND_BUFFER_BORROW_READ | c.ND_BUFFER_BORROW_WRITE, PM_FS_BUFFER_SIZE);
    if (p == null) return null;
    return @ptrCast(@alignCast(p.?));
}

fn fs_release() void {
    _ = api().buffer_release.?(c.ND_BUFFER_KIND_FS);
}

fn byte_copy(dst: [*]u8, src: [*]const u8, len: usize) void {
    var i: usize = 0;
    while (i < len) : (i += 1) {
        dst[i] = src[i];
    }
}

fn be_u16(data: []const u8, off: usize) ?u16 {
    if (off + 2 > data.len) return null;
    return (@as(u16, data[off]) << 8) | @as(u16, data[off + 1]);
}

fn be_i16(data: []const u8, off: usize) ?i16 {
    return @bitCast(be_u16(data, off) orelse return null);
}

fn be_u32(data: []const u8, off: usize) ?u32 {
    if (off + 4 > data.len) return null;
    return (@as(u32, data[off]) << 24) |
        (@as(u32, data[off + 1]) << 16) |
        (@as(u32, data[off + 2]) << 8) |
        @as(u32, data[off + 3]);
}

fn find_table(data: []const u8, tag: [4]u8) ?usize {
    const num_tables = be_u16(data, 4) orelse return null;
    var i: usize = 0;
    while (i < num_tables) : (i += 1) {
        const rec = 12 + i * 16;
        if (rec + 16 > data.len) return null;
        if (data[rec] == tag[0] and data[rec + 1] == tag[1] and data[rec + 2] == tag[2] and data[rec + 3] == tag[3]) {
            const offset = be_u32(data, rec + 8) orelse return null;
            return @intCast(offset);
        }
    }
    return null;
}

fn parse_ttf_metrics(f: *loaded_font_t) bool {
    if (f.ptr == null or f.len < 12) return false;
    const data: []const u8 = f.ptr.?[0..f.len];

    const head_off = find_table(data, .{ 'h', 'e', 'a', 'd' }) orelse return false;
    const hhea_off = find_table(data, .{ 'h', 'h', 'e', 'a' }) orelse return false;

    const upem = be_u16(data, head_off + 18) orelse return false;
    const asc = be_i16(data, hhea_off + 4) orelse return false;
    const desc = be_i16(data, hhea_off + 6) orelse return false;
    const gap = be_i16(data, hhea_off + 8) orelse return false;

    if (upem == 0) return false;
    f.units_per_em = upem;
    f.ascent = asc;
    f.descent = desc;
    f.line_gap = gap;
    return true;
}

fn read_file_into_shmem(path: []const u8, out_shmem_id: *u32, out_ptr: *[*]u8, out_len: *usize) i32 {
    if (g_fs_endpoint == IPC_ENDPOINT_NONE) return -1;
    const fs_buf = fs_borrow_rw() orelse return -1;
    defer fs_release();

    if (path.len == 0 or path.len >= PM_FS_BUFFER_SIZE) return -1;
    byte_copy(fs_buf, path.ptr, path.len);

    var reply: c.nd_ipc_message_t = undefined;
    const req = g_req_id;
    g_req_id +%= 1;
    if (ipc_call(g_fs_endpoint, req, c.FS_IPC_READ_PATH_REQ, @intCast(path.len), 0, 0, 0, &reply) != 0) return -1;
    if (reply.type != c.FS_IPC_RESP or @as(i32, @bitCast(reply.arg0)) <= 0) return -1;

    const len: usize = @intCast(reply.arg0);
    if (len > PM_FS_BUFFER_SIZE) return -1;

    const pages = (len + 4095) / 4096;
    var shmem_id: u32 = 0;
    var mapped_ptr: ?*anyopaque = null;
    if (api().shmem_create.?(pages, 0, &shmem_id, @ptrCast(&mapped_ptr)) != 0 or shmem_id == 0 or mapped_ptr == null) {
        return -1;
    }
    const dst: [*]u8 = @ptrCast(@alignCast(mapped_ptr.?));
    byte_copy(dst, fs_buf, len);

    out_shmem_id.* = shmem_id;
    out_ptr.* = dst;
    out_len.* = len;
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

fn pack_u16_pair(a: u32, b: u32) u32 {
    const a16: u16 = @intCast(a & 0xFFFF);
    const b16: u16 = @intCast(b & 0xFFFF);
    return @as(u32, a16) | (@as(u32, b16) << 16);
}

fn pack_s16_pair(a: i32, b: i32) u32 {
    const a16: u16 = @bitCast(@as(i16, @truncate(a)));
    const b16: u16 = @bitCast(@as(i16, @truncate(b)));
    return @as(u32, a16) | (@as(u32, b16) << 16);
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
        reply_with_status(req, c.FONT_STATUS_OK, 0, 0, pack_s16_pair(x0, y0));
        return;
    }

    const pixel_count: usize = @intCast(w * hgt);
    const pages = (pixel_count + 4095) / 4096;
    var shmem_id: u32 = 0;
    var mapped_ptr: ?*anyopaque = null;
    if (api().shmem_create.?(pages, 0, &shmem_id, @ptrCast(&mapped_ptr)) != 0 or shmem_id == 0 or mapped_ptr == null) {
        reply_with_status(req, c.FONT_STATUS_IO, 0, 0, 0);
        return;
    }
    const dst: [*]u8 = @ptrCast(@alignCast(mapped_ptr.?));
    c.stbtt_MakeCodepointBitmap(&f.font_info, dst, @intCast(w), @intCast(hgt), @intCast(w), scale, scale, @bitCast(codepoint));
    reply_with_status(req, c.FONT_STATUS_OK, shmem_id, pack_u16_pair(@intCast(w), @intCast(hgt)), pack_s16_pair(x0, y0));
}

fn load_builtin_fonts() void {
    const paths = [_][]const u8{
        "/boot/system/fonts/roboto.ttf",
        "/boot/system/fonts/roboto_mono.ttf",
        "/boot/system/fonts/roboto_serif.ttf",
    };
    const ids = [_]u32{ c.FONT_ID_ROBOTO, c.FONT_ID_ROBOTO_MONO, c.FONT_ID_NOTO_SERIF };
    const labels = [_][]const u8{ "roboto", "roboto-mono", "roboto-serif" };

    var i: usize = 0;
    while (i < paths.len and i < g_fonts.len) : (i += 1) {
        logMsg("[font] loading ");
        logMsg(labels[i]);
        logMsg("\n");
        var sid: u32 = 0;
        var ptr: [*]u8 = undefined;
        var len: usize = 0;
        if (read_file_into_shmem(paths[i], &sid, &ptr, &len) != 0) {
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

    if (svc_register("font", 1) != 0) {
        logMsg("[font] register failed\n");
        return -1;
    }

    const fs_ep = svc_lookup("fs.vfs", 2);
    if (fs_ep >= 0) {
        g_fs_endpoint = @bitCast(fs_ep);
    } else {
        const fs_ep_legacy = svc_lookup("fs", 3);
        if (fs_ep_legacy >= 0) g_fs_endpoint = @bitCast(fs_ep_legacy);
    }

    if (g_fs_endpoint == IPC_ENDPOINT_NONE) {
        logMsg("[font] fs endpoint unavailable\n");
    } else {
        load_builtin_fonts();
        logMsg("[font] service ready\n");
    }

    while (true) {
        var msg: c.nd_ipc_message_t = undefined;
        const rc = api().ipc_recv.?(ctxId(), g_font_endpoint, &msg);
        if (rc == IPC_EMPTY) {
            api().sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) return -1;

        switch (msg.type) {
            c.FONT_IPC_OPEN_FONT_REQ => handle_open_font(&msg),
            c.FONT_IPC_GET_METRICS_REQ => handle_get_metrics(&msg),
            c.FONT_IPC_RASTER_GLYPH_REQ => handle_raster_glyph(&msg),
            else => reply_with_status(&msg, c.FONT_STATUS_UNSUPPORTED, 0, 0, 0),
        }
    }
}
