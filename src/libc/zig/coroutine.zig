//! Method-based Zig bindings for the shared WASMOS stackless coroutine core.
//! Task storage and state-machine program counters remain caller-owned.

pub const FutureState = enum(c_int) { pending = 0, ready = 1, failed = 2 };
pub const CoroutineState = enum(c_int) { new = 0, ready = 1, running = 2, waiting = 3, dead = 4 };
pub const GroupKind = enum(c_int) { race = 0, all = 1 };

pub const TaskResult = struct {
    pub const complete: i32 = 0;
    pub const yielded: i32 = 1;
};

pub const AwaitResult = union(enum) {
    ready: usize,
    failed: i32,
    pending,
    invalid,
};

pub const TaskResume = *const fn (?*anyopaque, *usize) callconv(.c) i32;
pub const SuccessCallback = *const fn (?*anyopaque, usize, *usize) callconv(.c) i32;
pub const ErrorCallback = *const fn (?*anyopaque, i32, *usize) callconv(.c) i32;

pub const Runtime = extern struct {
    current: ?*Coroutine = null,
    ready_head: ?*Coroutine = null,
    ready_tail: ?*Coroutine = null,
    continuation_head: ?*Continuation = null,
    continuation_tail: ?*Continuation = null,
    running: bool = false,

    pub fn init(self: *Runtime) void {
        wasmos_wasm_runtime_init(self);
    }
    pub fn run(self: *Runtime) i32 {
        return wasmos_wasm_coroutine_run(self);
    }
    pub fn runBudget(self: *Runtime, budget: usize) i32 {
        return wasmos_wasm_coroutine_run_budget(self, budget);
    }
};

pub const Future = extern struct {
    state: FutureState = .pending,
    status: i32 = 0,
    value: usize = 0,
    runtime: ?*Runtime = null,
    waiters: ?*Coroutine = null,
    continuations: ?*Continuation = null,

    pub fn init(self: *Future, promise: *Promise) void {
        wasmos_future_init(self, promise);
    }
    pub fn poll(self: *const Future) ?union(enum) { ready: usize, failed: i32 } {
        var status: i32 = 0;
        var value: usize = 0;
        if (!wasmos_future_poll(self, &status, &value)) return null;
        return if (status == 0) .{ .ready = value } else .{ .failed = status };
    }
    pub fn awaitValue(self: *Future) AwaitResult {
        var value: usize = 0;
        const status = wasmos_future_await(self, &value);
        if (status == 0) return .{ .ready = value };
        if (status < 0) return .{ .failed = status };
        if (status == 1) return .pending;
        return .invalid;
    }
    pub fn then(self: *Future, runtime: *Runtime, continuation: *Continuation, success: ?SuccessCallback, failure: ?ErrorCallback, user: ?*anyopaque) ?*Future {
        return wasmos_future_then(runtime, self, continuation, success, failure, user);
    }
};

pub const Promise = extern struct {
    future: ?*Future = null,
    pub fn resolve(self: *Promise, value: usize) bool {
        return wasmos_promise_resolve(self, value);
    }
    pub fn reject(self: *Promise, status: i32) bool {
        return status < 0 and wasmos_promise_reject(self, status);
    }
};

pub const Coroutine = extern struct {
    runtime: ?*Runtime = null,
    next: ?*Coroutine = null,
    wait_next: ?*Coroutine = null,
    @"resume": ?TaskResume = null,
    user: ?*anyopaque = null,
    state: CoroutineState = .new,
    result: i32 = 0,
    completion: Future = .{},
    completion_promise: Promise = .{},

    pub fn start(self: *Coroutine, runtime: *Runtime, task_resume: TaskResume, user: ?*anyopaque) ?*Future {
        return wasmos_async_start(runtime, self, task_resume, user);
    }
    pub fn join(self: *Coroutine, result: ?*i32) i32 {
        return wasmos_wasm_coroutine_join(self, result);
    }
};

pub const Continuation = extern struct {
    next: ?*Continuation = null,
    future: ?*Future = null,
    on_success: ?SuccessCallback = null,
    on_error: ?ErrorCallback = null,
    user: ?*anyopaque = null,
    group: ?*FutureGroup = null,
    group_index: usize = 0,
    child: Future = .{},
    child_promise: Promise = .{},
    active: bool = false,
};

pub const FutureGroup = extern struct {
    runtime: ?*Runtime = null,
    future: Future = .{},
    promise: Promise = .{},
    continuations: ?[*]Continuation = null,
    values: ?[*]usize = null,
    count: usize = 0,
    completed: usize = 0,
    kind: GroupKind = .race,
    settled: bool = false,
    active: bool = false,

    pub fn race(self: *FutureGroup, runtime: *Runtime, inputs: []const *Future, continuations: []Continuation) ?*Future {
        if (inputs.len == 0 or inputs.len != continuations.len) return null;
        return wasmos_future_race(runtime, self, @ptrCast(inputs.ptr), inputs.len, continuations.ptr);
    }
    pub fn all(self: *FutureGroup, runtime: *Runtime, inputs: []const *Future, values: []usize, continuations: []Continuation) ?*Future {
        if (inputs.len == 0 or inputs.len != values.len or inputs.len != continuations.len) return null;
        return wasmos_future_all(runtime, self, @ptrCast(inputs.ptr), inputs.len, values.ptr, continuations.ptr);
    }
};

pub const IpcMessage = extern struct {
    type: i32,
    request_id: i32,
    arg0: i32,
    arg1: i32,
    arg2: i32,
    arg3: i32,
    source: i32,
    destination: i32,
};

pub const IpcReplyStatus = *const fn (?*anyopaque, *const IpcMessage) callconv(.c) i32;

pub const EventLoop = extern struct {
    receiver_endpoint: i32 = 0,
    select_id: i32 = 0,
    next_request_id: i32 = 0,
    default_on_message: ?*const anyopaque = null,
    default_user: ?*anyopaque = null,
    intents: [64]u32 = [_]u32{0} ** 64,
    handlers: [64]u32 = [_]u32{0} ** 64,

    pub fn init(self: *EventLoop, receiver_endpoint: i32, request_id_base: i32) void {
        wasmos_sys_event_loop_init(self, receiver_endpoint, request_id_base);
    }
    pub fn poll(self: *EventLoop, budget: i32) i32 {
        return wasmos_sys_event_loop_poll(self, budget);
    }
};

pub const IpcFuture = extern struct {
    future: Future = .{},
    promise: Promise = .{},
    loop: ?*EventLoop = null,
    reply_storage: IpcMessage = undefined,
    reply_status: ?IpcReplyStatus = null,
    user: ?*anyopaque = null,
    request_id: i32 = 0,
    active: u8 = 0,
    padding: [3]u8 = .{ 0, 0, 0 },

    pub fn init(self: *IpcFuture, reply_status: ?IpcReplyStatus, user: ?*anyopaque) void {
        wasmos_sys_wasm_ipc_future_init(self, reply_status, user);
    }
    pub fn send(self: *IpcFuture, loop: *EventLoop, destination: i32, source: i32, msg_type: i32, args: [4]i32, request_id: *i32) ?*Future {
        return wasmos_sys_wasm_ipc_future_send(loop, self, destination, source, msg_type, args[0], args[1], args[2], args[3], request_id);
    }
    pub fn cancel(self: *IpcFuture, status: i32) void {
        wasmos_sys_wasm_ipc_future_cancel(self, status);
    }
    pub fn reply(self: *const IpcFuture) *const IpcMessage {
        return wasmos_sys_wasm_ipc_future_reply(self);
    }
};

pub const FsRequest = extern struct {
    ipc: IpcFuture = .{},
    pub fn init(self: *FsRequest) void {
        wasmos_sys_wasm_fs_request_init(self);
    }
    pub fn send(self: *FsRequest, loop: *EventLoop, fs_endpoint: i32, reply_endpoint: i32, msg_type: i32, args: [4]i32, request_id: *i32) ?*Future {
        return wasmos_sys_wasm_fs_request_send(loop, self, fs_endpoint, reply_endpoint, msg_type, args[0], args[1], args[2], args[3], request_id);
    }
    pub fn reply(self: *const FsRequest) *const IpcMessage {
        return wasmos_sys_wasm_fs_request_reply(self);
    }
};

extern fn wasmos_wasm_runtime_init(*Runtime) void;
extern fn wasmos_async_start(*Runtime, *Coroutine, TaskResume, ?*anyopaque) ?*Future;
extern fn wasmos_wasm_coroutine_run(*Runtime) i32;
extern fn wasmos_wasm_coroutine_run_budget(*Runtime, usize) i32;
extern fn wasmos_wasm_coroutine_join(*Coroutine, ?*i32) i32;
extern fn wasmos_future_init(*Future, *Promise) void;
extern fn wasmos_future_poll(*const Future, ?*i32, ?*usize) bool;
extern fn wasmos_future_await(*Future, ?*usize) i32;
extern fn wasmos_promise_resolve(*Promise, usize) bool;
extern fn wasmos_promise_reject(*Promise, i32) bool;
extern fn wasmos_future_then(*Runtime, *Future, *Continuation, ?SuccessCallback, ?ErrorCallback, ?*anyopaque) ?*Future;
extern fn wasmos_future_race(*Runtime, *FutureGroup, [*]const *Future, usize, [*]Continuation) ?*Future;
extern fn wasmos_future_all(*Runtime, *FutureGroup, [*]const *Future, usize, [*]usize, [*]Continuation) ?*Future;
extern fn wasmos_sys_event_loop_init(*EventLoop, i32, i32) void;
extern fn wasmos_sys_event_loop_poll(*EventLoop, i32) i32;
extern fn wasmos_sys_wasm_ipc_future_init(*IpcFuture, ?IpcReplyStatus, ?*anyopaque) void;
extern fn wasmos_sys_wasm_ipc_future_send(*EventLoop, *IpcFuture, i32, i32, i32, i32, i32, i32, i32, *i32) ?*Future;
extern fn wasmos_sys_wasm_ipc_future_cancel(*IpcFuture, i32) void;
extern fn wasmos_sys_wasm_ipc_future_reply(*const IpcFuture) *const IpcMessage;
extern fn wasmos_sys_wasm_fs_request_init(*FsRequest) void;
extern fn wasmos_sys_wasm_fs_request_send(*EventLoop, *FsRequest, i32, i32, i32, i32, i32, i32, i32, *i32) ?*Future;
extern fn wasmos_sys_wasm_fs_request_reply(*const FsRequest) *const IpcMessage;

// ============================================================================
// Typed asynchronous filesystem operations and the C-owned application wrapper.
//
// These mirror the Go `AsyncFSOperation` / `RunAsyncApp` API so a Zig app can
// express a filesystem workflow as a Promise-style chain of `.then` callbacks
// driven by the shared C coroutine runtime.  Zig passes buffer pointers
// straight through the linked C boundary and uses its own function pointers as
// continuation callbacks; the promise operations come from a fixed leak pool
// because the WASM target is freestanding with no heap.
// ============================================================================

/// A success callback returns this to hand its returned future back to the
/// runtime; the child future then adopts that future's eventual result.
pub const FUTURE_CHAIN_NEXT: i32 = 2;

/// Matches `wasmos_sys_wasm_fs_operation_t`.
pub const FsOperation = extern struct {
    request: FsRequest = .{},
    buffer_id: i32 = 0,
    buffer_borrow: i32 = 0,
    length: i32 = 0,
    has_buffer: u8 = 0,
};

pub const PrepareFn = *const fn (?*anyopaque, i32, i32, i32, i32) callconv(.c) void;

/// Matches `wasmos_sys_wasm_async_config_t`: the C wrapper owns the runtime,
/// root coroutine, event loop, and private reply endpoint for the whole app.
pub const AsyncAppConfig = extern struct {
    runtime: Runtime = .{},
    root: Coroutine = .{},
    event_loop: EventLoop = .{},
    reply_endpoint: i32 = 0,
    @"resume": ?TaskResume = null,
    prepare: ?PrepareFn = null,
    user: ?*anyopaque = null,
};

pub const ChainCallback = *const fn (*AsyncFsOp) ?*Future;
pub const CatchCallback = *const fn (i32) i32;

/// One typed asynchronous filesystem operation plus the owner storage its
/// promise chain needs.  Instances come from a fixed leak pool: the WASM app
/// target is freestanding with no heap, and every op in a chain must stay live
/// until the whole chain settles (the flattened futures adopt one another).
pub const AsyncFsOp = struct {
    operation: FsOperation = .{},
    future: ?*Future = null,
    continuation: Continuation = .{},
    adopt: Continuation = .{},
    reply: IpcMessage = undefined,
    read_ptr: ?[*]u8 = null,
    read_len: i32 = 0,
    path: [256]u8 = undefined,
    chain: ?ChainCallback = null,
    catch_fn: ?CatchCallback = null,

    /// Copy out a settled read payload and return the response status/result.
    pub fn result(self: *AsyncFsOp) i32 {
        const dst: ?*anyopaque = if (self.read_ptr) |p| @ptrCast(p) else null;
        return wasmos_sys_wasm_fs_operation_finish(&self.operation, dst, self.read_len, &self.reply);
    }

    /// JavaScript-Promise `then`: the callback returns the next future and the
    /// returned future adopts its eventual result.
    pub fn then(self: *AsyncFsOp, chain: ChainCallback) ?*Future {
        const future = self.future orelse return null;
        self.chain = chain;
        return wasmos_future_then_flat(wasmos_sys_wasm_async_runtime().?, future, &self.continuation, &self.adopt, asyncFsChainSuccess, asyncFsChainError, @ptrCast(self));
    }

    /// Promise `catch`: convert a rejected operation into a resolved one. The
    /// handler returns zero to resolve or a negative status to keep rejecting.
    pub fn catchReject(self: *AsyncFsOp, handler: CatchCallback) ?*Future {
        const future = self.future orelse return null;
        self.catch_fn = handler;
        return wasmos_future_then(wasmos_sys_wasm_async_runtime().?, future, &self.continuation, null, asyncFsCatchError, @ptrCast(self));
    }
};

fn asyncFsChainSuccess(user: ?*anyopaque, value: usize, out: *usize) callconv(.c) i32 {
    _ = value;
    const op: *AsyncFsOp = @ptrCast(@alignCast(user.?));
    if (op.chain) |chain| {
        const next = chain(op) orelse return -1;
        out.* = @intFromPtr(next);
        return FUTURE_CHAIN_NEXT;
    }
    return -1;
}

fn asyncFsChainError(user: ?*anyopaque, status: i32, out: *usize) callconv(.c) i32 {
    _ = user;
    out.* = 0;
    return status;
}

fn asyncFsCatchError(user: ?*anyopaque, status: i32, out: *usize) callconv(.c) i32 {
    const op: *AsyncFsOp = @ptrCast(@alignCast(user.?));
    out.* = 0;
    return if (op.catch_fn) |handler| handler(status) else status;
}

const ASYNC_FS_OP_MAX = 24;
var fs_op_pool: [ASYNC_FS_OP_MAX]AsyncFsOp = [_]AsyncFsOp{.{}} ** ASYNC_FS_OP_MAX;
var fs_op_next: usize = 0;

fn allocOp() ?*AsyncFsOp {
    if (fs_op_next >= ASYNC_FS_OP_MAX) return null;
    const op = &fs_op_pool[fs_op_next];
    fs_op_next += 1;
    op.* = .{};
    return op;
}

fn stagePath(op: *AsyncFsOp, path: []const u8) bool {
    if (path.len == 0 or path.len + 1 > op.path.len) return false;
    @memcpy(op.path[0..path.len], path);
    op.path[path.len] = 0;
    return true;
}

/// Open `path` with `flags`; returns a promise-chainable operation.
pub fn openAsync(path: []const u8, flags: i32) ?*AsyncFsOp {
    const op = allocOp() orelse return null;
    if (!stagePath(op, path)) return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_open_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), op.path[0..].ptr, flags, &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Read up to `dst.len` bytes for `fd`; `dst` must stay live until the
/// operation settles and `result` copies the payload into it.
pub fn readAsync(fd: i32, dst: []u8) ?*AsyncFsOp {
    if (dst.len == 0) return null;
    const op = allocOp() orelse return null;
    op.read_ptr = dst.ptr;
    op.read_len = @intCast(dst.len);
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_read_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), fd, @ptrCast(dst.ptr), @intCast(dst.len), &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Write `src` to `fd`; `src` is copied synchronously into the transfer buffer.
pub fn writeAsync(fd: i32, src: []const u8) ?*AsyncFsOp {
    if (src.len == 0) return null;
    const op = allocOp() orelse return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_write_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), fd, @ptrCast(src.ptr), @intCast(src.len), &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Close `fd`.
pub fn closeAsync(fd: i32) ?*AsyncFsOp {
    const op = allocOp() orelse return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_close_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), fd, &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Unlink `path`.
pub fn unlinkAsync(path: []const u8) ?*AsyncFsOp {
    const op = allocOp() orelse return null;
    if (!stagePath(op, path)) return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_unlink_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), op.path[0..].ptr, &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Stat `path`; rejects when the path does not exist.
pub fn statAsync(path: []const u8) ?*AsyncFsOp {
    const op = allocOp() orelse return null;
    if (!stagePath(op, path)) return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_stat_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), op.path[0..].ptr, &request_id) orelse return null;
    op.future = future;
    return op;
}

const AppState = struct {
    start: ?*const fn () ?*Future = null,
    completion: ?*Future = null,
    started: bool = false,
};
var app_state: AppState = .{};
var app_config: AsyncAppConfig = .{};

fn appResume(user: ?*anyopaque, out: *usize) callconv(.c) i32 {
    _ = user;
    if (!app_state.started) {
        app_state.started = true;
        app_state.completion = if (app_state.start) |s| s() else null;
    }
    const completion = app_state.completion orelse return -1;
    return switch (completion.awaitValue()) {
        .pending => TaskResult.yielded,
        .ready => |value| ret: {
            out.* = value;
            break :ret TaskResult.complete;
        },
        .failed => |status| status,
        .invalid => -1,
    };
}

/// Enter the C-owned application wrapper. `start` runs once from the root
/// coroutine and returns the terminal future for the app's promise chain; the
/// wrapper owns the runtime, event loop, and private reply endpoint.
pub fn runAsyncApp(start: *const fn () ?*Future) i32 {
    app_state = .{ .start = start, .completion = null, .started = false };
    app_config.@"resume" = appResume;
    app_config.prepare = null;
    app_config.user = null;
    return wasmos_sys_wasm_async_run(&app_config, 0, 0, 0, 0);
}

// Scoped so the `fs_endpoint` host import does not collide with the
// `fs_endpoint` parameters used by the request/ipc-future send methods above.
const host = struct {
    extern "wasmos" fn fs_endpoint() callconv(.c) i32;
};
extern fn wasmos_sys_wasm_fs_operation_init(*FsOperation) void;
extern fn wasmos_sys_wasm_fs_open_async(*EventLoop, *FsOperation, i32, i32, [*]const u8, i32, *i32) ?*Future;
extern fn wasmos_sys_wasm_fs_read_async(*EventLoop, *FsOperation, i32, i32, i32, ?*anyopaque, i32, *i32) ?*Future;
extern fn wasmos_sys_wasm_fs_write_async(*EventLoop, *FsOperation, i32, i32, i32, ?*const anyopaque, i32, *i32) ?*Future;
extern fn wasmos_sys_wasm_fs_close_async(*EventLoop, *FsOperation, i32, i32, i32, *i32) ?*Future;
extern fn wasmos_sys_wasm_fs_unlink_async(*EventLoop, *FsOperation, i32, i32, [*]const u8, *i32) ?*Future;
extern fn wasmos_sys_wasm_fs_stat_async(*EventLoop, *FsOperation, i32, i32, [*]const u8, *i32) ?*Future;
extern fn wasmos_sys_wasm_fs_operation_finish(*FsOperation, ?*anyopaque, i32, *IpcMessage) i32;
extern fn wasmos_future_then_flat(*Runtime, *Future, *Continuation, *Continuation, ?SuccessCallback, ?ErrorCallback, ?*anyopaque) ?*Future;
extern fn wasmos_sys_wasm_async_run(*AsyncAppConfig, i32, i32, i32, i32) i32;
extern fn wasmos_sys_wasm_async_event_loop() ?*EventLoop;
extern fn wasmos_sys_wasm_async_reply_endpoint() i32;
extern fn wasmos_sys_wasm_async_runtime() ?*Runtime;
