const std = @import("std");
const root = @import("root");

const FS_IPC_OPEN_REQ: i32 = 0x400;
const FS_IPC_READ_REQ: i32 = 0x401;
const FS_IPC_WRITE_REQ: i32 = 0x406;
const FS_IPC_CLOSE_REQ: i32 = 0x402;
const FS_IPC_STAT_REQ: i32 = 0x403;
const FS_IPC_SEEK_REQ: i32 = 0x405;
const FS_IPC_RESP: i32 = 0x480;

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;
const IPC_FIELD_ARG1: i32 = 3;

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
extern "wasmos" fn ipc_recv(endpoint: i32) callconv(.c) i32;
extern "wasmos" fn ipc_last_field(field: i32) callconv(.c) i32;
extern "wasmos" fn fs_endpoint() callconv(.c) i32;
extern "wasmos" fn fs_buffer_size() callconv(.c) i32;
extern "wasmos" fn fs_buffer_write(ptr: i32, len: i32, offset: i32) callconv(.c) i32;
extern "wasmos" fn fs_buffer_copy(ptr: i32, len: i32, offset: i32) callconv(.c) i32;

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
var g_startup_args = [4]i32{ 0, 0, 0, 0 };

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
    return @intCast(root.main());
}

fn rawWrite(bytes: []const u8) Error!void {
    if (bytes.len == 0) {
        return;
    }
    if (console_write(@intCast(@intFromPtr(bytes.ptr)), @intCast(bytes.len)) != 0) {
        return Error.HostCallFailed;
    }
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
    if (ipc_recv(reply_endpoint) < 0) {
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
            const max_buffer = fs_buffer_size();
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
                if (fs_buffer_copy(@intCast(@intFromPtr(buffer.ptr + done)), chunk_read, 0) != 0) {
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
            const max_buffer = fs_buffer_size();
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
                if (fs_buffer_write(@intCast(@intFromPtr(buffer.ptr + done)), @intCast(chunk_len), 0) != 0) {
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

    fn openWithFlags(path: []const u8, flags: i32) Error!File {
        var path_buf: [256]u8 = undefined;
        const max_buffer = fs_buffer_size();

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

        if (fs_buffer_write(@intCast(@intFromPtr(&path_buf[0])), @intCast(path.len + 1), 0) != 0) {
            return Error.HostCallFailed;
        }

        const response = try fsRequest(FS_IPC_OPEN_REQ, @intCast(path.len), flags, 0, 0);
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
        var path_buf: [256]u8 = undefined;
        const max_buffer = fs_buffer_size();

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

        if (fs_buffer_write(@intCast(@intFromPtr(&path_buf[0])), @intCast(path.len + 1), 0) != 0) {
            return Error.HostCallFailed;
        }

        const response = try fsRequest(FS_IPC_STAT_REQ, @intCast(path.len), 0, 0, 0);
        if (response.arg0 < 0) {
            return Error.BadResponse;
        }
        return Stat{
            .size = @intCast(response.arg0),
            .mode = @as(u32, @intCast(response.arg1)) & (S_IFREG | S_IFDIR),
        };
    }
};
