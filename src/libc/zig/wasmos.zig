const std = @import("std");
const root = @import("root");

/// Stackless coroutines, futures/promises, the IPC event loop and the typed
/// asynchronous filesystem operations. Re-exported so a guest imports only this
/// module; see coroutine.zig for the contracts.
pub const coroutine = @import("coroutine.zig");

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

// `whence` values for fs.File.seek: the new offset is taken from the start of
// the file, from the current offset, or from the file size. The FS backend
// refuses a resulting offset outside [0, size], so SEEK_END with a positive
// offset fails rather than extending the file.
pub const SEEK_SET: i32 = 0;
pub const SEEK_CUR: i32 = 1;
pub const SEEK_END: i32 = 2;
// File-type bits of fs.Stat.mode; the FS reply carries no permission bits, and
// stat masks everything except these two.
pub const S_IFREG: u32 = 0x8000;
pub const S_IFDIR: u32 = 0x4000;
// Open flags, POSIX-valued. Bit 0 is the access mode (O_RDONLY or O_WRONLY);
// the rest are modifiers that the FS backend accepts only together with
// O_WRONLY. There is no read/write mode: a flag word carrying any other bit is
// rejected with a bad-args status.
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
// Object/owner/borrow xfer ABI (owner-push): read/write name the object by
// buffer_id; acquire owns; borrow grants a named endpoint's context rights.
extern "wasmos" fn xfer_buffer_write(buffer_id: i32, ptr: i32, len: i32, offset: i32) callconv(.c) i32;
extern "wasmos" fn xfer_buffer_read(buffer_id: i32, ptr: i32, len: i32, offset: i32) callconv(.c) i32;
extern "wasmos" fn xfer_buffer_acquire(minimum_size: i32) callconv(.c) i32;
extern "wasmos" fn xfer_buffer_borrow(grantee_endpoint: i32, buffer_id: i32, flags: i32) callconv(.c) i32;
extern "wasmos" fn xfer_buffer_release(buffer_id: i32) callconv(.c) i32;
extern "wasmos" fn spawn_info_buffer() callconv(.c) i32;
const XFER_GRANT_RW: i32 = 0x3;

// Startup contract (mirrors wasmos_spawn_info_t in wasmos_spawn_info.h).
const SPAWN_INFO_MAGIC: u32 = 0x57535049; // 'WSPI'
const SpawnInfo = extern struct {
    magic: u32 = 0,
    version: u32 = 0,
    header_size: u32 = 0,
    proc_endpoint: u32 = 0,
    tty: u32 = 0,
    module_count: u32 = 0,
    module_index: u32 = 0,
    args_off: u32 = 0,
    args_len: u32 = 0,
};
var g_spawn_info: SpawnInfo = .{};

fn loadSpawnInfo() void {
    g_spawn_info = .{};
    const bid = spawn_info_buffer();
    if (bid <= 0) return;
    if (xfer_buffer_read(
        bid,
        @intCast(@intFromPtr(&g_spawn_info)),
        @intCast(@sizeOf(SpawnInfo)),
        0,
    ) != 0 or g_spawn_info.magic != SPAWN_INFO_MAGIC) {
        g_spawn_info = .{};
    }
}
extern "wasmos" fn thread_gettid() callconv(.c) i32;
extern "wasmos" fn thread_yield() callconv(.c) i32;
extern "wasmos" fn mutex_try_lock(ptr: i32) callconv(.c) i32;
extern "wasmos" fn mutex_unlock(ptr: i32) callconv(.c) i32;

/// Failure modes of this module. The FS protocol's own packed WASMOS_ERR_FS_*
/// status is not surfaced: a request the backend rejects comes back as an
/// FS_IPC_ERROR message, which every entry point here reports as BadResponse.
pub const Error = error{
    /// A reply arrived but did not match the request: wrong message type (an
    /// error reply included), wrong request id, or a negative status field.
    BadResponse,
    /// The value does not fit the destination: a formatted line longer than
    /// stdlib.print's 256-byte buffer, or a path longer than the transfer
    /// buffer the FS manager reads it from.
    BufferTooSmall,
    /// A host call refused the operation (console write, transfer-buffer
    /// read/write/borrow, or IPC send/receive).
    HostCallFailed,
    /// A caller-supplied argument is unusable: an empty path, or a readline
    /// buffer with no room for a byte plus the NUL terminator.
    InvalidArgument,
    /// The path plus its NUL exceeds the 256-byte staging buffer.
    NameTooLong,
    /// A prerequisite of the call does not exist yet: no FS service has
    /// registered, no endpoint could be created, or no transfer buffer could be
    /// acquired.
    NotAvailable,
    /// Declared for parity with the other language ports; no function in this
    /// module returns it.
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
    // Argv is the args blob in the spawn-info buffer (loaded by loadSpawnInfo).
    if (g_spawn_info.magic != SPAWN_INFO_MAGIC or g_spawn_info.args_len == 0) return;
    const bid = spawn_info_buffer();
    if (bid <= 0) return;
    var n: usize = g_spawn_info.args_len;
    if (n > CLI_ARGS_BUF_LEN - 1) n = CLI_ARGS_BUF_LEN - 1;
    if (xfer_buffer_read(
        bid,
        @intCast(@intFromPtr(&g_cli_args_raw[0])),
        @intCast(n),
        @intCast(g_spawn_info.args_off),
    ) != 0) return;
    g_cli_args_raw[n] = 0;

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
///
/// The slices point into a module-static 128-byte copy of the spawn-info args
/// blob, valid for the life of the process. Parsing is a plain split on spaces
/// and tabs with no quoting; at most 16 arguments and 127 bytes survive, and the
/// rest are dropped. Empty when wasmos_main did not run or the process was
/// spawned without arguments.
pub fn cliArgs() []const []const u8 {
    return g_cli_arg_slices[0..g_cli_argc];
}

/// Recursive mutex whose state lives in guest memory and whose arbitration is
/// done by the kernel, layout-compatible with wasmos_mutex_t.
///
/// The kernel link tables export no `wasmos.mutex_try_lock` / `mutex_unlock`
/// (FIXME(user-mutex-import) in src/libc/include/wasmos/api.h), so a module that
/// actually calls these fails to instantiate on an unresolved import.
pub const Mutex = extern struct {
    /// Thread id of the current owner, 0 when unlocked. Written by the kernel.
    owner_tid: u32,
    /// Number of unmatched lock acquisitions by the owner.
    recursion_depth: u32,

    /// Reset to the unlocked state. Zeroing a mutex another thread holds loses
    /// that ownership, so init only a mutex nobody has locked yet.
    pub fn init(self: *Mutex) void {
        self.owner_tid = 0;
        self.recursion_depth = 0;
    }

    /// Thread id of the calling thread, as the kernel records it in owner_tid.
    pub fn currentTid() i32 {
        return thread_gettid();
    }

    /// Attempts one acquisition: 0 when the mutex is now held by this thread
    /// (raising recursion_depth if it already was), 1 when another thread owns
    /// it, negative on error. Never blocks.
    pub fn tryLock(self: *Mutex) i32 {
        return mutex_try_lock(@intCast(@intFromPtr(self)));
    }

    /// Acquires the mutex, yielding the thread between attempts while another
    /// owner holds it. Returns 0 once held, or the negative code that ended the
    /// retry loop. This is a yield-spin, not a sleep.
    pub fn lock(self: *Mutex) i32 {
        while (true) {
            const rc = self.tryLock();
            if (rc != 1) {
                return rc;
            }
            _ = thread_yield();
        }
    }

    /// Drops one acquisition, releasing the mutex when recursion_depth reaches
    /// zero. Returns 0 on success, negative when the caller is not the owner.
    pub fn unlock(self: *Mutex) i32 {
        return mutex_unlock(@intCast(@intFromPtr(self)));
    }
};

/// Values the process manager handed this process at spawn time, read out of the
/// spawn-info buffer by wasmos_main. A module entered through anything other
/// than wasmos_main sees zeros throughout.
pub const startup = struct {
    /// Legacy accessor: index 0 == proc.endpoint (from spawn-info); 1..3 == 0.
    pub fn arg(index: usize) i32 {
        if (index >= g_startup_args.len) {
            return 0;
        }
        return g_startup_args[index];
    }
    /// IPC endpoint of the process manager, for spawn/exit protocol requests.
    /// 0 when no spawn info was loaded.
    pub fn procEndpoint() i32 {
        return @bitCast(g_spawn_info.proc_endpoint);
    }
    /// Id of the controlling TTY the process manager allocated, 0 when none was
    /// allocated or no spawn info was loaded.
    pub fn tty() i32 {
        return @bitCast(g_spawn_info.tty);
    }
    /// Number of boot modules in this process's boot list.
    pub fn moduleCount() u32 {
        return g_spawn_info.module_count;
    }
    /// This module's index in that boot list, 0 when not applicable.
    pub fn moduleIndex() u32 {
        return g_spawn_info.module_index;
    }
};

/// Entry point the process manager calls instead of `_start`.
///
/// Loads the spawn-info header and argv blob, then calls the guest's `main`
/// (resolved from the root module) and reports its return value to the process
/// manager via proc_exit. Takes no arguments: every startup value comes from the
/// spawn-info buffer. proc_exit does not return, so the trailing return is
/// unreachable in a live process.
pub export fn wasmos_main() callconv(.c) i32 {
    loadSpawnInfo();
    g_startup_args[0] = @bitCast(g_spawn_info.proc_endpoint);
    g_startup_args[1] = 0;
    g_startup_args[2] = 0;
    g_startup_args[3] = 0;
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

/// Console output and line input. Everything here writes to the process's
/// console (the kernel log / its terminal), not to a file descriptor.
pub const stdlib = struct {
    /// Writes `bytes` verbatim; no newline is added and no NUL is required.
    /// An empty slice is a no-op success.
    pub fn write(bytes: []const u8) Error!void {
        try rawWrite(bytes);
    }

    /// Identical to `write`: the trailing newline C's puts adds is not added
    /// here.
    pub fn puts(bytes: []const u8) Error!void {
        try rawWrite(bytes);
    }

    /// Formats into a 256-byte stack buffer and writes the result. A line that
    /// does not fit is refused with Error.BufferTooSmall rather than truncated.
    /// `fmt` is a comptime std.fmt format string, so this pulls in std.fmt,
    /// which emits bulk-memory opcodes. Both runtimes execute those, so this is
    /// usable; `strconv` remains for callers that want formatting without
    /// pulling std.fmt's code size into the module.
    pub fn print(comptime fmt: []const u8, args: anytype) Error!void {
        var buffer: [256]u8 = undefined;
        const line = std.fmt.bufPrint(&buffer, fmt, args) catch return Error.BufferTooSmall;
        try rawWrite(line);
    }

    /// Alias of `print`, for guests written against the C API's name.
    pub fn printf(comptime fmt: []const u8, args: anytype) Error!void {
        try print(fmt, args);
    }

    /// As `print`, with a newline appended to the format at comptime. The
    /// newline counts against `print`'s 256-byte line budget.
    pub fn println(comptime fmt: []const u8, args: anytype) Error!void {
        try print(fmt ++ "\n", args);
    }

    /// Reads console bytes into `buffer` up to and including the first newline,
    /// NUL-terminates them, and returns the byte count excluding the NUL.
    ///
    /// The newline, when one arrived, is part of the returned count. Reads stop
    /// early once the console has no byte ready, so a short return is normal and
    /// does not mean end of input; it does not park until a full line exists.
    /// `buffer` shorter than two bytes is InvalidArgument, a full buffer ends
    /// the read with the terminator in the last byte, and a host-call failure
    /// clears buffer[0] before returning HostCallFailed.
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

/// Synchronous message passing. `call` and `recv` park the process in the kernel
/// until a message arrives; a component that must keep serving its own endpoint
/// while a request is outstanding uses coroutine.EventLoop instead.
pub const ipc = struct {
    /// A received message, copied out of the caller's last-received slot. The
    /// four argument words are protocol-defined; `source` is the endpoint to
    /// address a reply to, `destination` the endpoint it arrived on.
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

    /// Send a request to server and block until the FIRST message arrives on the
    /// per-context managed reply endpoint; it is returned as the reply without
    /// checking its request id or source. Only one request may be outstanding on
    /// that endpoint at a time, or a stale reply is returned for a later call.
    /// The C helper (wasmos_ipc_call) matches instead, and eventloop-driven
    /// components demultiplex properly.
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
    /// Parks the process indefinitely: there is no timeout and no way to
    /// interrupt the wait. Every message queued on `endpoint` is returned,
    /// replies and requests alike, so a server that also issues requests must
    /// demultiplex on request_id itself. HostCallFailed on an invalid endpoint
    /// or a receive error.
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
    /// request_id must be echoed from the request, or the caller cannot match
    /// the reply. Returns once the message is queued; it does not wait for the
    /// peer to receive it, and HostCallFailed means the send was refused (a
    /// full or unknown destination endpoint).
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

/// Synchronous filesystem access over the FS manager's IPC protocol. Every call
/// stages its payload through an owned transfer buffer, sends one request, and
/// parks until the reply arrives.
pub const fs = struct {
    /// Result of `stat`. `size` is the file length in bytes; `mode` carries only
    /// the S_IFREG / S_IFDIR type bits.
    pub const Stat = struct {
        size: u32,
        mode: u32,
    };

    /// An open file. `fd` is the FS manager's client-side descriptor; it is
    /// valid until `close`, and closing twice fails at the manager rather than
    /// here.
    pub const File = struct {
        fd: i32,

        /// Reads up to `buffer.len` bytes at the file's current offset,
        /// returning how many were stored.
        ///
        /// Loops over transfer-buffer-sized chunks, so a short reply ends the
        /// read: the return is 0 at end of file and may be less than requested
        /// without being an error. An empty `buffer` is a 0-byte success that
        /// issues no request. The buffer acquired for the transfer is released
        /// on every path, which also revokes the FS manager's borrow.
        pub fn read(self: File, buffer: []u8) Error!usize {
            if (buffer.len == 0) {
                return 0;
            }
            const max_buffer = xfer_buffer_size();
            if (max_buffer <= 0) {
                return Error.NotAvailable;
            }

            // Own one buffer and grant the FS manager once; reuse both across the
            // whole call (a per-chunk re-grant would fail ALREADY_BORROWED). defer
            // release cascade-revokes the grant on every path.
            const bid = xfer_buffer_acquire(max_buffer);
            if (bid < 0) {
                return Error.NotAvailable;
            }
            defer _ = xfer_buffer_release(bid);
            const b1 = xfer_buffer_borrow(fs_endpoint(), bid, XFER_GRANT_RW);
            if (b1 < 0) {
                return Error.HostCallFailed;
            }

            var done: usize = 0;
            while (done < buffer.len) {
                const remaining = buffer.len - done;
                const chunk_len: usize = if (remaining > @as(usize, @intCast(max_buffer)))
                    @intCast(max_buffer)
                else
                    remaining;
                const response = try fsRequest(FS_IPC_READ_REQ, self.fd, @intCast(chunk_len), bid, b1);
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
                if (xfer_buffer_read(bid, @intCast(@intFromPtr(buffer.ptr + done)), chunk_read, 0) != 0) {
                    return Error.HostCallFailed;
                }
                done += @intCast(chunk_read);
                if (chunk_read < chunk_len) {
                    break;
                }
            }
            return done;
        }

        /// Releases the descriptor at the FS manager. A refusal surfaces as
        /// BadResponse; the status word of a successful response is ignored.
        pub fn close(self: File) Error!void {
            _ = try fsRequest(FS_IPC_CLOSE_REQ, self.fd, 0, 0, 0);
        }

        /// Writes `buffer` at the file's current offset and returns how many
        /// bytes the FS manager accepted.
        ///
        /// Chunked like `read`: a chunk the manager only partially accepts ends
        /// the loop, so a short return is a real short write and not an error.
        /// An empty `buffer` is a 0-byte success that issues no request.
        pub fn write(self: File, buffer: []const u8) Error!usize {
            if (buffer.len == 0) {
                return 0;
            }
            const max_buffer = xfer_buffer_size();
            if (max_buffer <= 0) {
                return Error.NotAvailable;
            }

            const bid = xfer_buffer_acquire(max_buffer);
            if (bid < 0) {
                return Error.NotAvailable;
            }
            defer _ = xfer_buffer_release(bid);
            const b1 = xfer_buffer_borrow(fs_endpoint(), bid, XFER_GRANT_RW);
            if (b1 < 0) {
                return Error.HostCallFailed;
            }

            var done: usize = 0;
            while (done < buffer.len) {
                const remaining = buffer.len - done;
                const chunk_len: usize = if (remaining > @as(usize, @intCast(max_buffer)))
                    @intCast(max_buffer)
                else
                    remaining;
                if (xfer_buffer_write(bid, @intCast(@intFromPtr(buffer.ptr + done)), @intCast(chunk_len), 0) != 0) {
                    return Error.HostCallFailed;
                }
                const response = try fsRequest(FS_IPC_WRITE_REQ, self.fd, @intCast(chunk_len), bid, b1);
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

        /// Moves the file offset to `offset` bytes from the SEEK_SET / SEEK_CUR
        /// / SEEK_END origin and returns the new absolute offset. A target
        /// outside [0, size] is refused by the backend and surfaces as
        /// BadResponse; seeking past the end does not extend the file.
        pub fn seek(self: File, offset: i32, whence: i32) Error!i32 {
            const response = try fsRequest(FS_IPC_SEEK_REQ, self.fd, offset, whence, 0);
            if (response.arg0 < 0) {
                return Error.BadResponse;
            }
            return response.arg0;
        }
    };

    /// Submits one filesystem protocol request without blocking. Synchronous
    /// File APIs remain unchanged. request and any transfer buffer named by
    /// args[2]/args[3] stay caller-owned until the future settles.
    pub fn requestAsync(loop: *coroutine.EventLoop, request: *coroutine.FsRequest, reply_endpoint: i32, msg_type: i32, args: [4]i32, request_id: *i32) Error!?*coroutine.Future {
        const endpoint = fs_endpoint();
        if (endpoint < 0 or reply_endpoint < 0) return Error.NotAvailable;
        request.init();
        return request.send(loop, endpoint, reply_endpoint, msg_type, args, request_id);
    }

    // Owner-push staging: own a buffer holding the NUL-terminated path, grant the
    // FS manager R|W over it, and return the handles + path length (excluding
    // NUL). The caller passes path_len (arg0), bid (arg2) and b1 (arg3) to
    // fsRequest and releases bid afterward (cascade-revokes b1).
    const StagedPath = struct { bid: i32, b1: i32, path_len: usize };

    fn stagePath(path: []const u8) Error!StagedPath {
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

        const bid = xfer_buffer_acquire(@intCast(path.len + 1));
        if (bid < 0) {
            return Error.NotAvailable;
        }
        if (xfer_buffer_write(bid, @intCast(@intFromPtr(&path_buf[0])), @intCast(path.len + 1), 0) != 0) {
            _ = xfer_buffer_release(bid);
            return Error.HostCallFailed;
        }
        const b1 = xfer_buffer_borrow(fs_endpoint(), bid, XFER_GRANT_RW);
        if (b1 < 0) {
            _ = xfer_buffer_release(bid);
            return Error.HostCallFailed;
        }
        return StagedPath{ .bid = bid, .b1 = b1, .path_len = path.len };
    }

    fn openWithFlags(path: []const u8, flags: i32) Error!File {
        const s = try stagePath(path);
        defer _ = xfer_buffer_release(s.bid);
        const response = try fsRequest(FS_IPC_OPEN_REQ, @intCast(s.path_len), flags, s.bid, s.b1);
        if (response.arg0 < 0) {
            return Error.BadResponse;
        }
        return File{ .fd = response.arg0 };
    }

    /// Opens an existing file for reading. `path` is borrowed for the call and
    /// must be non-empty and shorter than 256 bytes including its NUL. A
    /// missing file is reported as BadResponse.
    pub fn openRead(path: []const u8) Error!File {
        return openWithFlags(path, O_RDONLY);
    }

    /// Opens an existing file for writing at offset 0 without truncating it.
    /// Does not create the file.
    pub fn openWrite(path: []const u8) Error!File {
        return openWithFlags(path, O_WRONLY);
    }

    /// Creates the file if needed and truncates it to zero length.
    pub fn create(path: []const u8) Error!File {
        return openWithFlags(path, O_WRONLY | O_CREAT | O_TRUNC);
    }

    /// Creates the file if needed and positions writes at the end.
    pub fn openAppend(path: []const u8) Error!File {
        return openWithFlags(path, O_WRONLY | O_CREAT | O_APPEND);
    }

    /// Returns the size and file-type bits of `path` without opening it.
    /// BadResponse when the path does not exist.
    pub fn stat(path: []const u8) Error!Stat {
        const s = try stagePath(path);
        defer _ = xfer_buffer_release(s.bid);
        const response = try fsRequest(FS_IPC_STAT_REQ, @intCast(s.path_len), 0, s.bid, s.b1);
        if (response.arg0 < 0) {
            return Error.BadResponse;
        }
        return Stat{
            .size = @intCast(response.arg0),
            .mode = @as(u32, @intCast(response.arg1)) & (S_IFREG | S_IFDIR),
        };
    }

    /// Removes a file. A refusal by the backend (missing file, directory,
    /// read-only mount) arrives as an FS error message and surfaces as
    /// BadResponse; the status word of a successful response is not inspected.
    pub fn unlink(path: []const u8) Error!void {
        const s = try stagePath(path);
        defer _ = xfer_buffer_release(s.bid);
        _ = try fsRequest(FS_IPC_UNLINK_REQ, @intCast(s.path_len), 0, s.bid, s.b1);
    }

    /// Creates a directory. Reports failure the same way as `unlink`.
    pub fn mkdir(path: []const u8) Error!void {
        const s = try stagePath(path);
        defer _ = xfer_buffer_release(s.bid);
        _ = try fsRequest(FS_IPC_MKDIR_REQ, @intCast(s.path_len), 0, s.bid, s.b1);
    }

    /// Removes a directory. Reports failure the same way as `unlink`.
    pub fn rmdir(path: []const u8) Error!void {
        const s = try stagePath(path);
        defer _ = xfer_buffer_release(s.bid);
        _ = try fsRequest(FS_IPC_RMDIR_REQ, @intCast(s.path_len), 0, s.bid, s.b1);
    }

    /// Lists the current directory into `buffer` as a NUL-terminated text blob
    /// and returns its length excluding the terminator.
    ///
    /// The listing arrives as a stream of FS_IPC_STREAM messages carrying four
    /// bytes each, and zero bytes inside a message are skipped rather than
    /// stored. A `buffer` too small to hold the listing is filled, terminated,
    /// and returned truncated -- the remaining stream messages keep arriving on
    /// the reply endpoint and are not drained.
    pub fn readDir(buffer: []u8) Error!usize {
        return fsRequestStream(FS_IPC_READDIR_REQ, 0, 0, 0, 0, buffer);
    }
};

// ---------------------------------------------------------------------------
// fmt — numeric formatting helpers that avoid std.fmt.
//
// std.fmt.bufPrint emits memory.fill / memory.copy (the WASM bulk-memory
// extension). Both wasm3 and WARP execute those, so std.fmt is usable -- these
// helpers are kept because they cost far less code size, not because std.fmt is
// unavailable.
//
// They produce the same output for the values a typical WASMOS app would
// display (integers, finite floats, error sentinel) using only basic
// arithmetic — no allocator.
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
            if (buf.len > 0) {
                buf[0] = '0';
                return buf[0..1];
            }
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
        if (val < 0.0) {
            buf[pos] = '-';
            pos += 1;
            val = -val;
        }
        if (@trunc(val) == val and val < 1e15) {
            const s = fmtIntF64(val, buf[pos..]);
            return buf[0 .. pos + s.len];
        }
        const int_f = @trunc(val);
        const si = fmtIntF64(int_f, buf[pos..]);
        pos += si.len;
        if (pos < buf.len) {
            buf[pos] = '.';
            pos += 1;
        }
        var frac = val - int_f;
        var last_nz = pos;
        var d: usize = 0;
        while (d < 8 and pos < buf.len) : (d += 1) {
            frac *= 10.0;
            const t = @trunc(frac);
            const dv = f64Digit(t); // f64Digit avoids trunc_sat
            buf[pos] = '0' + dv;
            pos += 1;
            frac = frac - t; // manual subtraction avoids @mod / fmod
            if (dv != 0) last_nz = pos;
        }
        return buf[0..last_nz];
    }

    /// Parse a decimal string (with optional leading '-' and one '.') to f64.
    /// inline: prevents a WASM function type with (result f64) from being emitted.
    ///
    /// Parsing stops at the first character it does not recognise and there is
    /// no failure channel: a leading '+', an exponent, or leading whitespace
    /// all end the parse, and an unparsable string yields 0. Digits are
    /// accumulated with repeated multiply-add, so long inputs lose precision.
    pub inline fn parseF64(s: []const u8) f64 {
        var neg = false;
        var i: usize = 0;
        if (i < s.len and s[i] == '-') {
            neg = true;
            i += 1;
        }
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
    /// Truncates silently to buf.len and returns the prefix actually written;
    /// no NUL terminator is added.
    pub fn litBuf(buf: []u8, lit: []const u8) []const u8 {
        const n = if (lit.len < buf.len) lit.len else buf.len;
        var i: usize = 0;
        while (i < n) : (i += 1) buf[i] = lit[i];
        return buf[0..n];
    }
};
