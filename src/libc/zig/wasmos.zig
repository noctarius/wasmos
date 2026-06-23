const std = @import("std");
const root = @import("root");

const FS_IPC_OPEN_REQ: i32 = 0x400;
const FS_IPC_READ_REQ: i32 = 0x401;
const FS_IPC_WRITE_REQ: i32 = 0x406;
const FS_IPC_CLOSE_REQ: i32 = 0x402;
const FS_IPC_STAT_REQ: i32 = 0x403;
const FS_IPC_SEEK_REQ: i32 = 0x405;
const FS_IPC_UNLINK_REQ: i32 = 0x407;
const FS_IPC_MKDIR_REQ: i32 = 0x408;
const FS_IPC_RMDIR_REQ: i32 = 0x409;
const FS_IPC_READDIR_REQ: i32 = 0x410;
const FS_IPC_RESP: i32 = 0x480;
const FS_IPC_STREAM: i32 = 0x481;

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;
const IPC_FIELD_ARG1: i32 = 3;
const IPC_FIELD_SOURCE: i32 = 4;
const IPC_FIELD_DESTINATION: i32 = 5;
const IPC_FIELD_ARG2: i32 = 6;
const IPC_FIELD_ARG3: i32 = 7;

pub const SEEK_SET: i32 = 0;
pub const SEEK_CUR: i32 = 1;
pub const SEEK_END: i32 = 2;
pub const S_IFREG: u32 = 0x8000;
pub const S_IFDIR: u32 = 0x4000;
pub const O_RDONLY: i32 = 0;
pub const O_WRONLY: i32 = 1;
pub const O_APPEND: i32 = 0x0008;
pub const O_CREAT: i32 = 0x0040;
pub const O_TRUNC: i32 = 0x0200;

extern "wasmos" fn console_write(ptr: i32, len: i32) callconv(.c) i32;
extern "wasmos" fn console_read(ptr: i32, len: i32) callconv(.c) i32;
extern "wasmos" fn proc_exit(status: i32) callconv(.c) i32;
extern "wasmos" fn ipc_create_endpoint() callconv(.c) i32;
extern "wasmos" fn ipc_send(
    destination_endpoint: i32,
    source_endpoint: i32,
    msg_type: i32,
    request_id: i32,
    arg0: i32,
    arg1: i32,
    arg2: i32,
    arg3: i32,
) callconv(.c) i32;
extern "wasmos" fn ipc_select_one(endpoint: i32) callconv(.c) i32;
extern "wasmos" fn ipc_last_field(field: i32) callconv(.c) i32;
extern "wasmos" fn fs_endpoint() callconv(.c) i32;
extern "wasmos" fn xfer_buffer_size() callconv(.c) i32;
extern "wasmos" fn xfer_buffer_write(ptr: i32, len: i32, offset: i32) callconv(.c) i32;
extern "wasmos" fn xfer_buffer_read(ptr: i32, len: i32, offset: i32) callconv(.c) i32;
extern "wasmos" fn thread_gettid() callconv(.c) i32;
extern "wasmos" fn thread_yield() callconv(.c) i32;
extern "wasmos" fn mutex_try_lock(ptr: i32) callconv(.c) i32;
extern "wasmos" fn mutex_unlock(ptr: i32) callconv(.c) i32;

pub const Error = error{
    BadResponse,
    BufferTooSmall,
    HostCallFailed,
    InvalidArgument,
    NameTooLong,
    NotAvailable,
    Unsupported,
};

var g_fs_reply_endpoint: i32 = -1;
var g_fs_request_id: i32 = 1;
var g_ipc_reply_endpoint: i32 = -1;
var g_ipc_request_id: i32 = 1;
var g_startup_args = [4]i32{ 0, 0, 0, 0 };

const CLI_ARGS_BUF_LEN = 128;
const CLI_ARGS_MAX = 16;
var g_cli_args_raw: [CLI_ARGS_BUF_LEN]u8 = [_]u8{0} ** CLI_ARGS_BUF_LEN;
var g_cli_arg_slices: [CLI_ARGS_MAX][]const u8 = undefined;
var g_cli_argc: usize = 0;

fn parseCliArgs() void {
    g_cli_argc = 0;
    if (xfer_buffer_read(
        @intCast(@intFromPtr(&g_cli_args_raw[0])),
        CLI_ARGS_BUF_LEN - 1,
        0,
    ) != 0) return;
    g_cli_args_raw[CLI_ARGS_BUF_LEN - 1] = 0;

    var pos: usize = 0;
    while (pos < CLI_ARGS_BUF_LEN - 1 and g_cli_args_raw[pos] != 0 and g_cli_argc < CLI_ARGS_MAX) {
        while (pos < CLI_ARGS_BUF_LEN - 1 and
            (g_cli_args_raw[pos] == ' ' or g_cli_args_raw[pos] == '\t')) : (pos += 1)
        {}
        if (pos >= CLI_ARGS_BUF_LEN - 1 or g_cli_args_raw[pos] == 0) break;
        const start = pos;
        while (pos < CLI_ARGS_BUF_LEN - 1 and
            g_cli_args_raw[pos] != 0 and
            g_cli_args_raw[pos] != ' ' and
            g_cli_args_raw[pos] != '\t') : (pos += 1)
        {}
        if (pos > start) {
            g_cli_arg_slices[g_cli_argc] = g_cli_args_raw[start..pos];
            g_cli_argc += 1;
        }
    }
}

/// Returns the CLI argument strings passed to this process at spawn time.
pub fn cliArgs() []const []const u8 {
    return g_cli_arg_slices[0..g_cli_argc];
}

pub const Mutex = extern struct {
    owner_tid: u32,
    recursion_depth: u32,

    pub fn init(self: *Mutex) void {
        self.owner_tid = 0;
        self.recursion_depth = 0;
    }

    pub fn currentTid() i32 {
        return thread_gettid();
    }

    pub fn tryLock(self: *Mutex) i32 {
        return mutex_try_lock(@intCast(@intFromPtr(self)));
    }

    pub fn lock(self: *Mutex) i32 {
        while (true) {
            const rc = self.tryLock();
            if (rc != 1) {
                return rc;
            }
            _ = thread_yield();
        }
    }

    pub fn unlock(self: *Mutex) i32 {
        return mutex_unlock(@intCast(@intFromPtr(self)));
    }
};

pub const startup = struct {
    pub fn arg(index: usize) i32 {
        if (index >= g_startup_args.len) {
            return 0;
        }
        return g_startup_args[index];
    }
};

pub export fn wasmos_main(arg0: i32, arg1: i32, arg2: i32, arg3: i32) callconv(.c) i32 {
    g_startup_args[0] = arg0;
    g_startup_args[1] = arg1;
    g_startup_args[2] = arg2;
    g_startup_args[3] = arg3;
    parseCliArgs();
    const rc: i32 = @intCast(root.main());
    _ = proc_exit(rc);
    return rc;
}

fn rawWrite(bytes: []const u8) Error!void {
    if (bytes.len == 0) {
        return;
    }
    if (console_write(@intCast(@intFromPtr(bytes.ptr)), @intCast(bytes.len)) != 0) {
        return Error.HostCallFailed;
    }
}

fn ensureIpcReplyEndpoint() Error!i32 {
    if (g_ipc_reply_endpoint >= 0) {
        return g_ipc_reply_endpoint;
    }
    const endpoint = ipc_create_endpoint();
    if (endpoint < 0) {
        return Error.NotAvailable;
    }
    g_ipc_reply_endpoint = endpoint;
    return endpoint;
}

fn nextIpcRequestId() i32 {
    const id = g_ipc_request_id;
    g_ipc_request_id += 1;
    if (g_ipc_request_id < 1) {
        g_ipc_request_id = 1;
    }
    return id;
}

fn ensureFsReplyEndpoint() Error!i32 {
    if (g_fs_reply_endpoint >= 0) {
        return g_fs_reply_endpoint;
    }

    const endpoint = ipc_create_endpoint();
    if (endpoint < 0) {
        return Error.NotAvailable;
    }
    g_fs_reply_endpoint = endpoint;
    return endpoint;
}

fn nextFsRequestId() i32 {
    const request_id = g_fs_request_id;
    g_fs_request_id += 1;
    if (g_fs_request_id < 1) {
        g_fs_request_id = 1;
    }
    return request_id;
}

fn fsRequest(msg_type: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32) Error!struct { arg0: i32, arg1: i32 } {
    const endpoint = fs_endpoint();
    if (endpoint < 0) {
        return Error.NotAvailable;
    }

    const reply_endpoint = try ensureFsReplyEndpoint();
    const request_id = nextFsRequestId();

    if (ipc_send(endpoint, reply_endpoint, msg_type, request_id, arg0, arg1, arg2, arg3) != 0) {
        return Error.HostCallFailed;
    }
    if (ipc_select_one(reply_endpoint) < 0) {
        return Error.HostCallFailed;
    }
    if (ipc_last_field(IPC_FIELD_REQUEST_ID) != request_id or ipc_last_field(IPC_FIELD_TYPE) != FS_IPC_RESP) {
        return Error.BadResponse;
    }
    return .{
        .arg0 = ipc_last_field(IPC_FIELD_ARG0),
        .arg1 = ipc_last_field(IPC_FIELD_ARG1),
    };
}

fn fsRequestStream(msg_type: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32, out: []u8) Error!usize {
    const endpoint = fs_endpoint();
    if (endpoint < 0 or out.len == 0) {
        return Error.NotAvailable;
    }

    const reply_endpoint = try ensureFsReplyEndpoint();
    const request_id = nextFsRequestId();
    if (ipc_send(endpoint, reply_endpoint, msg_type, request_id, arg0, arg1, arg2, arg3) != 0) {
        return Error.HostCallFailed;
    }

    var out_len: usize = 0;
    while (true) {
        if (ipc_select_one(reply_endpoint) < 0) {
            return Error.HostCallFailed;
        }
        if (ipc_last_field(IPC_FIELD_REQUEST_ID) != request_id) {
            continue;
        }

        const response_type = ipc_last_field(IPC_FIELD_TYPE);
        if (response_type == FS_IPC_STREAM) {
            const args = [4]i32{
                ipc_last_field(IPC_FIELD_ARG0),
                ipc_last_field(IPC_FIELD_ARG1),
                ipc_last_field(IPC_FIELD_ARG2),
                ipc_last_field(IPC_FIELD_ARG3),
            };
            for (args) |a| {
                const c: u8 = @intCast(a & 0xFF);
                if (c == 0) continue;
                if (out_len + 1 >= out.len) {
                    out[out.len - 1] = 0;
                    return out_len;
                }
                out[out_len] = c;
                out_len += 1;
            }
            continue;
        }

        if (response_type != FS_IPC_RESP or ipc_last_field(IPC_FIELD_ARG0) != 0) {
            return Error.BadResponse;
        }
        if (out_len < out.len) {
            out[out_len] = 0;
        }
        return out_len;
    }
}

pub const stdlib = struct {
    pub fn write(bytes: []const u8) Error!void {
        try rawWrite(bytes);
    }

    pub fn puts(bytes: []const u8) Error!void {
        try rawWrite(bytes);
    }

    pub fn print(comptime fmt: []const u8, args: anytype) Error!void {
        var buffer: [256]u8 = undefined;
        const line = std.fmt.bufPrint(&buffer, fmt, args) catch return Error.BufferTooSmall;
        try rawWrite(line);
    }

    pub fn printf(comptime fmt: []const u8, args: anytype) Error!void {
        try print(fmt, args);
    }

    pub fn println(comptime fmt: []const u8, args: anytype) Error!void {
        try print(fmt ++ "\n", args);
    }

    pub fn readline(buffer: []u8) Error!usize {
        if (buffer.len <= 1) {
            return Error.InvalidArgument;
        }
        var pos: usize = 0;
        while (pos + 1 < buffer.len) {
            const got = console_read(@intCast(@intFromPtr(buffer.ptr + pos)), 1);
            if (got < 0) {
                buffer[0] = 0;
                return Error.HostCallFailed;
            }
            if (got == 0) {
                break;
            }
            pos += 1;
            if (buffer[pos - 1] == '\n') {
                break;
            }
        }
        buffer[pos] = 0;
        return pos;
    }
};

pub const ipc = struct {
    pub const Reply = struct {
        type: i32,
        request_id: i32,
        source: i32,
        destination: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    };

    /// Create a new message endpoint (for servers setting up their receive endpoint).
    pub fn createEndpoint() Error!i32 {
        const ep = ipc_create_endpoint();
        if (ep < 0) return Error.NotAvailable;
        return ep;
    }

    /// Send a request to server and block until a reply arrives.
    /// The reply endpoint is per-context and managed internally — callers never
    /// share it, so only this context's reply ever lands there.
    pub fn call(server: i32, msg_type: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32) Error!Reply {
        const reply_endpoint = try ensureIpcReplyEndpoint();
        const request_id = nextIpcRequestId();
        if (ipc_send(server, reply_endpoint, msg_type, request_id, arg0, arg1, arg2, arg3) != 0) {
            return Error.HostCallFailed;
        }
        if (ipc_select_one(reply_endpoint) < 0) {
            return Error.HostCallFailed;
        }
        return Reply{
            .type = ipc_last_field(IPC_FIELD_TYPE),
            .request_id = ipc_last_field(IPC_FIELD_REQUEST_ID),
            .source = ipc_last_field(IPC_FIELD_SOURCE),
            .destination = ipc_last_field(IPC_FIELD_DESTINATION),
            .arg0 = ipc_last_field(IPC_FIELD_ARG0),
            .arg1 = ipc_last_field(IPC_FIELD_ARG1),
            .arg2 = ipc_last_field(IPC_FIELD_ARG2),
            .arg3 = ipc_last_field(IPC_FIELD_ARG3),
        };
    }

    /// Block until a message arrives on endpoint (for servers).
    pub fn recv(endpoint: i32) Error!Reply {
        if (ipc_select_one(endpoint) < 0) {
            return Error.HostCallFailed;
        }
        return Reply{
            .type = ipc_last_field(IPC_FIELD_TYPE),
            .request_id = ipc_last_field(IPC_FIELD_REQUEST_ID),
            .source = ipc_last_field(IPC_FIELD_SOURCE),
            .destination = ipc_last_field(IPC_FIELD_DESTINATION),
            .arg0 = ipc_last_field(IPC_FIELD_ARG0),
            .arg1 = ipc_last_field(IPC_FIELD_ARG1),
            .arg2 = ipc_last_field(IPC_FIELD_ARG2),
            .arg3 = ipc_last_field(IPC_FIELD_ARG3),
        };
    }

    /// Send a reply from a server back to the caller's private reply endpoint.
    /// source should be the server's own service endpoint.
    /// destination should be req.source from the incoming request.
    pub fn reply(
        destination: i32,
        source: i32,
        msg_type: i32,
        request_id: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    ) Error!void {
        if (ipc_send(destination, source, msg_type, request_id, arg0, arg1, arg2, arg3) != 0) {
            return Error.HostCallFailed;
        }
    }
};

pub const fs = struct {
    pub const Stat = struct {
        size: u32,
        mode: u32,
    };

    pub const File = struct {
        fd: i32,

        pub fn read(self: File, buffer: []u8) Error!usize {
            if (buffer.len == 0) {
                return 0;
            }
            const max_buffer = xfer_buffer_size();
            if (max_buffer <= 0) {
                return Error.NotAvailable;
            }

            var done: usize = 0;
            while (done < buffer.len) {
                const remaining = buffer.len - done;
                const chunk_len: usize = if (remaining > @as(usize, @intCast(max_buffer)))
                    @intCast(max_buffer)
                else
                    remaining;
                const response = try fsRequest(FS_IPC_READ_REQ, self.fd, @intCast(chunk_len), 0, 0);
                const chunk_read = response.arg0;
                if (chunk_read < 0) {
                    return Error.BadResponse;
                }
                if (chunk_read == 0) {
                    break;
                }
                if (chunk_read > max_buffer or @as(usize, @intCast(chunk_read)) > chunk_len) {
                    return Error.BadResponse;
                }
                if (xfer_buffer_read(@intCast(@intFromPtr(buffer.ptr + done)), chunk_read, 0) != 0) {
                    return Error.HostCallFailed;
                }
                done += @intCast(chunk_read);
                if (chunk_read < chunk_len) {
                    break;
                }
            }
            return done;
        }

        pub fn close(self: File) Error!void {
            _ = try fsRequest(FS_IPC_CLOSE_REQ, self.fd, 0, 0, 0);
        }

        pub fn write(self: File, buffer: []const u8) Error!usize {
            if (buffer.len == 0) {
                return 0;
            }
            const max_buffer = xfer_buffer_size();
            if (max_buffer <= 0) {
                return Error.NotAvailable;
            }

            var done: usize = 0;
            while (done < buffer.len) {
                const remaining = buffer.len - done;
                const chunk_len: usize = if (remaining > @as(usize, @intCast(max_buffer)))
                    @intCast(max_buffer)
                else
                    remaining;
                if (xfer_buffer_write(@intCast(@intFromPtr(buffer.ptr + done)), @intCast(chunk_len), 0) != 0) {
                    return Error.HostCallFailed;
                }
                const response = try fsRequest(FS_IPC_WRITE_REQ, self.fd, @intCast(chunk_len), 0, 0);
                if (response.arg0 < 0 or @as(usize, @intCast(response.arg0)) > chunk_len) {
                    return Error.BadResponse;
                }
                done += @intCast(response.arg0);
                if (response.arg0 == 0 or @as(usize, @intCast(response.arg0)) != chunk_len) {
                    break;
                }
            }
            return done;
        }

        pub fn seek(self: File, offset: i32, whence: i32) Error!i32 {
            const response = try fsRequest(FS_IPC_SEEK_REQ, self.fd, offset, whence, 0);
            if (response.arg0 < 0) {
                return Error.BadResponse;
            }
            return response.arg0;
        }
    };

    fn stagePath(path: []const u8) Error!usize {
        var path_buf: [256]u8 = undefined;
        const max_buffer = xfer_buffer_size();

        if (path.len == 0) {
            return Error.InvalidArgument;
        }
        if (max_buffer <= 0) {
            return Error.NotAvailable;
        }
        if (path.len + 1 > path_buf.len) {
            return Error.NameTooLong;
        }
        if (path.len + 1 > @as(usize, @intCast(max_buffer))) {
            return Error.BufferTooSmall;
        }

        @memcpy(path_buf[0..path.len], path);
        path_buf[path.len] = 0;

        if (xfer_buffer_write(@intCast(@intFromPtr(&path_buf[0])), @intCast(path.len + 1), 0) != 0) {
            return Error.HostCallFailed;
        }
        return path.len;
    }

    fn openWithFlags(path: []const u8, flags: i32) Error!File {
        const path_len = try stagePath(path);
        const response = try fsRequest(FS_IPC_OPEN_REQ, @intCast(path_len), flags, 0, 0);
        if (response.arg0 < 0) {
            return Error.BadResponse;
        }
        return File{ .fd = response.arg0 };
    }

    pub fn openRead(path: []const u8) Error!File {
        return openWithFlags(path, O_RDONLY);
    }

    pub fn openWrite(path: []const u8) Error!File {
        return openWithFlags(path, O_WRONLY);
    }

    pub fn create(path: []const u8) Error!File {
        return openWithFlags(path, O_WRONLY | O_CREAT | O_TRUNC);
    }

    pub fn openAppend(path: []const u8) Error!File {
        return openWithFlags(path, O_WRONLY | O_CREAT | O_APPEND);
    }

    pub fn stat(path: []const u8) Error!Stat {
        const path_len = try stagePath(path);
        const response = try fsRequest(FS_IPC_STAT_REQ, @intCast(path_len), 0, 0, 0);
        if (response.arg0 < 0) {
            return Error.BadResponse;
        }
        return Stat{
            .size = @intCast(response.arg0),
            .mode = @as(u32, @intCast(response.arg1)) & (S_IFREG | S_IFDIR),
        };
    }

    pub fn unlink(path: []const u8) Error!void {
        const path_len = try stagePath(path);
        _ = try fsRequest(FS_IPC_UNLINK_REQ, @intCast(path_len), 0, 0, 0);
    }

    pub fn mkdir(path: []const u8) Error!void {
        const path_len = try stagePath(path);
        _ = try fsRequest(FS_IPC_MKDIR_REQ, @intCast(path_len), 0, 0, 0);
    }

    pub fn rmdir(path: []const u8) Error!void {
        const path_len = try stagePath(path);
        _ = try fsRequest(FS_IPC_RMDIR_REQ, @intCast(path_len), 0, 0, 0);
    }

    pub fn readDir(buffer: []u8) Error!usize {
        return fsRequestStream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, buffer);
    }
};

// ---------------------------------------------------------------------------
// fmt — numeric formatting helpers that avoid std.fmt.
//
// std.fmt.bufPrint internally uses memory.fill / memory.copy (WASM bulk-memory
// extension) even when -mcpu=generic-bulk_memory is passed, because the Zig
// standard library does not honour that flag for its own internals.  WARP's
// single-pass JIT only supports WASM MVP and rejects those opcodes.
//
// These helpers produce the same output for the values a typical WASMOS app
// would display (integers, finite floats, error sentinel) using only basic
// arithmetic — no WASM extensions, no allocator.
// ---------------------------------------------------------------------------
pub const strconv = struct {
    /// Convert a f64 value in [0.0, 9.0] to a u8 digit using only f64
    /// comparisons.  @intFromFloat generates i32.trunc_sat_f64_u which is
    /// NOT implemented in WARP's JIT (FeatureNotSupportedException).  The
    /// comparison chain uses only f64.lt (WASM MVP) and no trunc_sat.
    inline fn f64Digit(v: f64) u8 {
        if (v < 1.0) return 0;
        if (v < 2.0) return 1;
        if (v < 3.0) return 2;
        if (v < 4.0) return 3;
        if (v < 5.0) return 4;
        if (v < 6.0) return 5;
        if (v < 7.0) return 6;
        if (v < 8.0) return 7;
        if (v < 9.0) return 8;
        return 9;
    }

    /// Write the digits of a non-negative finite f64 integer value into buf.
    /// inline: prevents a separate WASM function type with f64 parameter from
    /// being emitted — WARP's JIT rejects function types that contain f64.
    inline fn fmtIntF64(val: f64, buf: []u8) []const u8 {
        if (val == 0.0) {
            if (buf.len > 0) { buf[0] = '0'; return buf[0..1]; }
            return buf[0..0];
        }
        var tmp: [20]u8 = undefined;
        var len: usize = 0;
        var rem = val;
        // Use manual modulo (rem - trunc(rem/10)*10) instead of @mod which
        // generates a call to a software fmod() runtime helper that produces
        // WASM function types containing f64 — rejected by WARP's JIT.
        while (rem >= 1.0 and len < 20) {
            const q = @trunc(rem / 10.0);
            tmp[len] = '0' + f64Digit(rem - q * 10.0);
            len += 1;
            rem = q;
        }
        var out: usize = 0;
        var j = len;
        while (j > 0 and out < buf.len) : (out += 1) {
            j -= 1;
            buf[out] = tmp[j];
        }
        return buf[0..out];
    }

    /// Format a finite f64 into buf with up to 8 fractional digits,
    /// trailing zeros stripped.  Returns "Error" for NaN / Inf.
    /// inline: see fmtIntF64 — prevents f64 from appearing in WASM type section.
    pub inline fn f64Buf(v: f64, buf: []u8) []const u8 {
        if (v != v or v > 1e308 or v < -1e308) return litBuf(buf, "Error");
        if (buf.len == 0) return buf[0..0];
        var pos: usize = 0;
        var val = v;
        if (val < 0.0) { buf[pos] = '-'; pos += 1; val = -val; }
        if (@trunc(val) == val and val < 1e15) {
            const s = fmtIntF64(val, buf[pos..]);
            return buf[0 .. pos + s.len];
        }
        const int_f = @trunc(val);
        const si = fmtIntF64(int_f, buf[pos..]);
        pos += si.len;
        if (pos < buf.len) { buf[pos] = '.'; pos += 1; }
        var frac = val - int_f;
        var last_nz = pos;
        var d: usize = 0;
        while (d < 8 and pos < buf.len) : (d += 1) {
            frac *= 10.0;
            const t = @trunc(frac);
            const dv = f64Digit(t);  // f64Digit avoids trunc_sat
            buf[pos] = '0' + dv;
            pos += 1;
            frac = frac - t;  // manual subtraction avoids @mod / fmod
            if (dv != 0) last_nz = pos;
        }
        return buf[0..last_nz];
    }

    /// Parse a decimal string (with optional leading '-' and one '.') to f64.
    /// inline: prevents a WASM function type with (result f64) from being emitted.
    pub inline fn parseF64(s: []const u8) f64 {
        var neg = false;
        var i: usize = 0;
        if (i < s.len and s[i] == '-') { neg = true; i += 1; }
        var int_part: f64 = 0;
        while (i < s.len and s[i] >= '0' and s[i] <= '9') : (i += 1)
            int_part = int_part * 10 + @as(f64, @floatFromInt(s[i] - '0'));
        var frac: f64 = 0;
        if (i < s.len and s[i] == '.') {
            i += 1;
            var place: f64 = 0.1;
            while (i < s.len and s[i] >= '0' and s[i] <= '9') : (i += 1) {
                frac += @as(f64, @floatFromInt(s[i] - '0')) * place;
                place *= 0.1;
            }
        }
        const result = int_part + frac;
        return if (neg) -result else result;
    }

    /// Copy a string literal into buf without using std.fmt.
    pub fn litBuf(buf: []u8, lit: []const u8) []const u8 {
        const n = if (lit.len < buf.len) lit.len else buf.len;
        var i: usize = 0;
        while (i < n) : (i += 1) buf[i] = lit[i];
        return buf[0..n];
    }
};
