//! Method-based Zig bindings for the shared WASMOS stackless coroutine core.
//! Task storage and state-machine program counters remain caller-owned.

/// Lifecycle of a future: pending until a promise settles it, then ready (its
/// status is 0 and its value meaningful) or failed (its status is negative).
/// A settled future never changes state again.
pub const FutureState = enum(c_int) { pending = 0, ready = 1, failed = 2 };
/// Lifecycle of a coroutine: new before `start`, ready while queued, running
/// inside its resume function, waiting while parked on a future, dead once its
/// resume function returned anything other than TaskResult.yielded.
pub const CoroutineState = enum(c_int) { new = 0, ready = 1, running = 2, waiting = 3, dead = 4 };
/// Settlement rule of a FutureGroup: `race` settles on the first input to
/// settle either way, `all` on the first failure or on every input succeeding.
pub const GroupKind = enum(c_int) { race = 0, all = 1 };

/// Return values of a TaskResume function. `complete` publishes the value
/// written to the out-parameter as the coroutine's result; `yielded` asks to be
/// resumed again, either from the ready queue or when the future the task
/// parked on settles. Any other value is taken as a failure status and rejects
/// the coroutine's completion future, so it must be negative.
pub const TaskResult = struct {
    pub const complete: i32 = 0;
    pub const yielded: i32 = 1;
};

/// Outcome of Future.awaitValue. `pending` means the calling task was parked and
/// must return TaskResult.yielded at once without touching its out-parameter;
/// `invalid` is the runtime refusing the await (no running coroutine, or a
/// future owned by another runtime) and never parks.
pub const AwaitResult = union(enum) {
    ready: usize,
    failed: i32,
    pending,
    invalid,
};

/// A stackless task body: called with the `user` pointer given to
/// Coroutine.start and an out-parameter for its completion value, returning a
/// TaskResult. It must record its own progress in caller-owned state, because
/// no call stack survives across a yield.
pub const TaskResume = *const fn (?*anyopaque, *usize) callconv(.c) i32;
/// Continuation callback for a resolved future: receives the future's value and
/// writes the child future's value. Returns 0 to resolve the child, a negative
/// status to reject it, or FUTURE_CHAIN_NEXT (from a `then_flat` registration
/// only) with the next future's address in the out-parameter.
pub const SuccessCallback = *const fn (?*anyopaque, usize, *usize) callconv(.c) i32;
/// Continuation callback for a rejected future: receives the negative status and
/// returns the status to propagate, or 0 to convert the rejection into a
/// resolved child carrying the out-parameter's value.
pub const ErrorCallback = *const fn (?*anyopaque, i32, *usize) callconv(.c) i32;

/// The cooperative scheduler: a queue of runnable coroutines and a queue of
/// continuations whose source future has settled. Caller-owned storage,
/// layout-compatible with wasmos_wasm_runtime_t; a guest normally has one.
/// Nothing runs until `run` or `runBudget` is called.
pub const Runtime = extern struct {
    current: ?*Coroutine = null,
    ready_head: ?*Coroutine = null,
    ready_tail: ?*Coroutine = null,
    continuation_head: ?*Continuation = null,
    continuation_tail: ?*Continuation = null,
    /// Set while inside run/runBudget; re-entry is refused rather than nested.
    running: bool = false,

    /// Zeroes the runtime. Any coroutine still queued on it is forgotten, so
    /// init only a runtime that is not being driven.
    pub fn init(self: *Runtime) void {
        wasmos_wasm_runtime_init(self);
    }
    /// Resumes ready coroutines and dispatches settled continuations until both
    /// queues are empty, returning the number of coroutine resumptions, or -1
    /// when called re-entrantly from inside a task or continuation. Coroutines
    /// waiting on futures nothing settles are simply left parked.
    pub fn run(self: *Runtime) i32 {
        return wasmos_wasm_coroutine_run(self);
    }
    /// As `run`, but resumes at most `budget` coroutines; queued continuations
    /// are still drained to exhaustion afterwards. A budget of 0 dispatches
    /// continuations only.
    pub fn runBudget(self: *Runtime, budget: usize) i32 {
        return wasmos_wasm_coroutine_run_budget(self, budget);
    }
};

/// The observing half of a one-shot result. Settled exactly once by the Promise
/// bound to it; until then it holds the coroutines parked on it and the
/// continuations registered against it. Caller-owned and reusable, but only
/// through `init`, which returns it to pending and drops those lists.
pub const Future = extern struct {
    state: FutureState = .pending,
    /// 0 once resolved, the rejecting negative status once failed.
    status: i32 = 0,
    /// Resolution value; protocol-defined, often a pointer cast to usize.
    value: usize = 0,
    /// Runtime that owns the future, adopted from the first await or `then`.
    /// Mixing a future between runtimes is refused.
    runtime: ?*Runtime = null,
    waiters: ?*Coroutine = null,
    continuations: ?*Continuation = null,

    /// Resets to pending and makes `promise` the only handle that can settle it.
    /// Any coroutine still parked on the previous incarnation is dropped and
    /// never woken.
    pub fn init(self: *Future, promise: *Promise) void {
        wasmos_future_init(self, promise);
    }
    /// Non-blocking observation: null while pending, otherwise the value or the
    /// rejecting status. Does not park and does not consume the result -- a
    /// settled future keeps answering.
    pub fn poll(self: *const Future) ?union(enum) { ready: usize, failed: i32 } {
        var status: i32 = 0;
        var value: usize = 0;
        if (!wasmos_future_poll(self, &status, &value)) return null;
        return if (status == 0) .{ .ready = value } else .{ .failed = status };
    }
    /// Awaits from inside a running task. A settled future answers immediately;
    /// a pending one parks the calling coroutine and returns `.pending`, on
    /// which the task MUST return TaskResult.yielded straight away -- it will be
    /// resumed from the top when the future settles, so its own state machine
    /// has to re-enter at the same point. `.invalid` means nothing was parked:
    /// there is no coroutine running, or the future belongs to another runtime.
    pub fn awaitValue(self: *Future) AwaitResult {
        var value: usize = 0;
        const status = wasmos_future_await(self, &value);
        if (status == 0) return .{ .ready = value };
        if (status < 0) return .{ .failed = status };
        if (status == 1) return .pending;
        return .invalid;
    }
    /// Registers `continuation` on this future and returns the child future that
    /// settles with the callback's result, or null when the continuation is
    /// already in use or the future belongs to another runtime.
    ///
    /// The callback never runs inline: it is queued on the runtime -- at once if
    /// this future has already settled -- and dispatched from run/runBudget. It
    /// runs at most once, and `continuation` and `user` must stay live until it
    /// does. With no matching callback the child inherits the source's outcome.
    pub fn then(self: *Future, runtime: *Runtime, continuation: *Continuation, success: ?SuccessCallback, failure: ?ErrorCallback, user: ?*anyopaque) ?*Future {
        return wasmos_future_then(runtime, self, continuation, success, failure, user);
    }
};

/// The settling half of a future, bound by Future.init. Whoever holds it decides
/// the outcome; it settles at most once.
pub const Promise = extern struct {
    future: ?*Future = null,
    /// Settles the future as ready with `value`, waking every parked waiter and
    /// queueing every registered continuation. False when there is no bound
    /// future or it has already settled.
    pub fn resolve(self: *Promise, value: usize) bool {
        return wasmos_promise_resolve(self, value);
    }
    /// Settles the future as failed with `status`, which must be negative --
    /// a zero or positive status is refused and returns false, as does an
    /// already-settled future.
    pub fn reject(self: *Promise, status: i32) bool {
        return status < 0 and wasmos_promise_reject(self, status);
    }
};

/// One stackless task and its completion future. Caller-owned; `start` overwrites
/// the whole record, so it may be reused once the previous run is dead.
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

    /// Schedules the task on `runtime` and returns its completion future, which
    /// resolves with the task's out-value or rejects with its failure status.
    /// Null when the coroutine is neither new nor dead, so a live task cannot be
    /// restarted. Nothing runs until the runtime is driven; `user` is borrowed
    /// and must outlive the task.
    pub fn start(self: *Coroutine, runtime: *Runtime, task_resume: TaskResume, user: ?*anyopaque) ?*Future {
        return wasmos_async_start(runtime, self, task_resume, user);
    }
    /// Awaits this coroutine's completion from inside another task: 0 with the
    /// result written out, a negative failure status, or 1 (AwaitResult
    /// `.pending`) after parking the caller, which must then yield. Not a
    /// blocking join -- calling it outside a running coroutine returns -1.
    pub fn join(self: *Coroutine, result: ?*i32) i32 {
        return wasmos_wasm_coroutine_join(self, result);
    }
};

/// Caller-owned registration record for one `then` callback, holding the child
/// future the callback settles. One record serves one live registration: it must
/// stay live until the callback runs, and cannot be reused while `active`.
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

/// Caller-owned storage combining several futures into one. The group, its
/// continuation array and (for `all`) its value array must stay live until the
/// group future settles -- at that point the runtime unlinks the continuations
/// still registered on unsettled inputs, so they need not outlive the slowest
/// source.
pub const FutureGroup = extern struct {
    runtime: ?*Runtime = null,
    future: Future = .{},
    promise: Promise = .{},
    continuations: ?[*]Continuation = null,
    values: ?[*]usize = null,
    count: usize = 0,
    /// Source callbacks that have run. A settled race leaves this at 1: the
    /// other sources were released, not merely outvoted.
    completed: usize = 0,
    kind: GroupKind = .race,
    /// True once the group future has been resolved or rejected.
    settled: bool = false,
    /// True between a successful start and settlement; a source callback that
    /// fires afterwards is ignored.
    active: bool = false,

    /// Settles with the first input to settle, resolving with its value or
    /// rejecting with its status. The losers are abandoned, not cancelled: their
    /// own work continues and their results are discarded.
    ///
    /// `inputs` and `continuations` must be non-empty and equal in length, every
    /// continuation unused, and every input either unowned or owned by
    /// `runtime`; otherwise null. The input pointers are read during this call
    /// only, but the futures themselves and both arrays must stay live until the
    /// returned future settles.
    pub fn race(self: *FutureGroup, runtime: *Runtime, inputs: []const *Future, continuations: []Continuation) ?*Future {
        if (inputs.len == 0 or inputs.len != continuations.len) return null;
        return wasmos_future_race(runtime, self, @ptrCast(inputs.ptr), inputs.len, continuations.ptr);
    }
    /// Settles once every input has resolved, filling `values` in input order
    /// and resolving with `values.ptr` cast to usize; the first input to reject
    /// rejects the group with that status and abandons the rest.
    ///
    /// All three slices must be non-empty and equal in length, with the same
    /// continuation and runtime conditions as `race`; otherwise null. `values`
    /// is written from source callbacks, so it must stay live until the returned
    /// future settles and is only fully populated on success.
    pub fn all(self: *FutureGroup, runtime: *Runtime, inputs: []const *Future, values: []usize, continuations: []Continuation) ?*Future {
        if (inputs.len == 0 or inputs.len != values.len or inputs.len != continuations.len) return null;
        return wasmos_future_all(runtime, self, @ptrCast(inputs.ptr), inputs.len, values.ptr, continuations.ptr);
    }
};

/// One IPC message in the layout the C event loop uses (wasmos_ipc_message_t).
/// Field order differs from ipc.Reply in wasmos.zig: source and destination come
/// last here.
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

/// Protocol validator for a reply: returns 0 to resolve the IpcFuture, or a
/// negative status to reject it. Runs from the event loop while dispatching the
/// reply, before the future settles.
pub const IpcReplyStatus = *const fn (?*anyopaque, *const IpcMessage) callconv(.c) i32;

/// The IPC demultiplexer: one receive endpoint, a table of in-flight requests
/// keyed by request id, and a table of per-message-type handlers. Caller-owned
/// and layout-compatible with wasmos_sys_event_loop_t; the intents/handlers word
/// arrays are opaque storage for the C records.
pub const EventLoop = extern struct {
    receiver_endpoint: i32 = 0,
    select_id: i32 = 0,
    next_request_id: i32 = 0,
    default_on_message: ?*const anyopaque = null,
    default_user: ?*anyopaque = null,
    intents: [64]u32 = [_]u32{0} ** 64,
    handlers: [64]u32 = [_]u32{0} ** 64,

    /// Binds the loop to `receiver_endpoint` and seeds its request ids at
    /// `request_id_base`, clearing both tables. Also creates a select set over
    /// the endpoint so `poll` can park; without one, poll degrades to a
    /// non-blocking drain. Bases must not collide between loops in one process,
    /// since the request id is what routes a reply to its intent.
    pub fn init(self: *EventLoop, receiver_endpoint: i32, request_id_base: i32) void {
        wasmos_sys_event_loop_init(self, receiver_endpoint, request_id_base);
    }
    /// Dispatches up to `budget` messages (0 means 1) and returns how many were
    /// handled. A message matching an in-flight request id resolves that intent,
    /// otherwise a type handler runs, otherwise the default handler; anything
    /// unclaimed is dropped. With nothing queued the first iteration parks on
    /// the select set instead of spinning.
    pub fn poll(self: *EventLoop, budget: i32) i32 {
        return wasmos_sys_event_loop_poll(self, budget);
    }
};

/// One in-flight request exposed as a future. Caller-owned and reusable: `init`
/// re-arms it, `send` refuses while the previous round trip is live, and the
/// reply is copied into `reply_storage` before the future settles.
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

    /// Zeroes the record and re-arms its future. `reply_status` decides how the
    /// reply settles the future (null accepts any reply); `user` is borrowed and
    /// passed back to it.
    pub fn init(self: *IpcFuture, reply_status: ?IpcReplyStatus, user: ?*anyopaque) void {
        wasmos_sys_wasm_ipc_future_init(self, reply_status, user);
    }
    /// Sends the request through `loop` and returns the future that settles when
    /// the reply is dispatched, writing the allocated id to `request_id`.
    ///
    /// Null when the record is already in flight or was not re-armed. A send
    /// that fails still returns the future, already rejected, so a caller that
    /// chained onto it observes the failure the same way as a rejecting reply.
    /// The future resolves with the address of the stored reply.
    pub fn send(self: *IpcFuture, loop: *EventLoop, destination: i32, source: i32, msg_type: i32, args: [4]i32, request_id: *i32) ?*Future {
        return wasmos_sys_wasm_ipc_future_send(loop, self, destination, source, msg_type, args[0], args[1], args[2], args[3], request_id);
    }
    /// Stops tracking an in-flight request and rejects its future with `status`
    /// (a non-negative status is replaced by -1). Only local tracking stops: a
    /// late reply from the peer is then dispatched as an ordinary message.
    pub fn cancel(self: *IpcFuture, status: i32) void {
        wasmos_sys_wasm_ipc_future_cancel(self, status);
    }
    /// The stored reply. Meaningful only after the future settles -- it starts
    /// zeroed -- and overwritten by the next `send` on this record.
    pub fn reply(self: *const IpcFuture) *const IpcMessage {
        return wasmos_sys_wasm_ipc_future_reply(self);
    }
};

/// An IpcFuture pre-armed for the filesystem protocol: its future resolves only
/// on an FS_IPC_RESP reply and rejects anything else, error replies included.
pub const FsRequest = extern struct {
    ipc: IpcFuture = .{},
    /// Re-arms the request with the FS reply validator installed.
    pub fn init(self: *FsRequest) void {
        wasmos_sys_wasm_fs_request_init(self);
    }
    /// Sends one FS protocol message; see IpcFuture.send. Null when either
    /// endpoint is negative. Any transfer buffer named by `args` stays
    /// caller-owned until the future settles.
    pub fn send(self: *FsRequest, loop: *EventLoop, fs_endpoint: i32, reply_endpoint: i32, msg_type: i32, args: [4]i32, request_id: *i32) ?*Future {
        return wasmos_sys_wasm_fs_request_send(loop, self, fs_endpoint, reply_endpoint, msg_type, args[0], args[1], args[2], args[3], request_id);
    }
    /// The stored FS reply; see IpcFuture.reply. arg0 carries the FS status or
    /// byte count.
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

/// Matches `wasmos_sys_wasm_fs_operation_t`: one FS request plus the transfer
/// buffer the C helpers acquire, borrow to the FS manager, and release again in
/// `AsyncFsOp.result`.
pub const FsOperation = extern struct {
    request: FsRequest = .{},
    buffer_id: i32 = 0,
    buffer_borrow: i32 = 0,
    length: i32 = 0,
    has_buffer: u8 = 0,
};

/// Optional hook the async wrapper calls with the four entry-arg registers after
/// the reply endpoint and event loop exist but before the root task starts.
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

/// Body of an `AsyncFsOp.then` step: receives the settled operation (call
/// `result` on it to take the payload) and returns the next future, or null to
/// reject the chain with -1.
pub const ChainCallback = *const fn (*AsyncFsOp) ?*Future;
/// Body of an `AsyncFsOp.catchReject` handler: receives the rejecting negative
/// status and returns 0 to resolve the chain, or a negative status to keep it
/// rejected.
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
    ///
    /// Returns the reply's arg0 -- an fd for open, a byte count for read/write,
    /// a size for stat -- or -1 when the payload could not be copied out. Call
    /// it only after the operation's future has settled: the stored reply starts
    /// zeroed, so an early call reports 0 rather than a real status. Releases
    /// the operation's transfer buffer, idempotently.
    pub fn result(self: *AsyncFsOp) i32 {
        const dst: ?*anyopaque = if (self.read_ptr) |p| @ptrCast(p) else null;
        return wasmos_sys_wasm_fs_operation_finish(&self.operation, dst, self.read_len, &self.reply);
    }

    /// JavaScript-Promise `then`: the callback returns the next future and the
    /// returned future adopts its eventual result.
    ///
    /// The callback runs from the runtime's continuation queue after this
    /// operation settles, never inline. A rejected operation skips it and
    /// forwards the status. Null when the operation never started; the op holds
    /// one continuation record, so `then` and `catchReject` share it and only
    /// one registration can be live at a time.
    pub fn then(self: *AsyncFsOp, chain: ChainCallback) ?*Future {
        const future = self.future orelse return null;
        self.chain = chain;
        return wasmos_future_then_flat(wasmos_sys_wasm_async_runtime().?, future, &self.continuation, &self.adopt, asyncFsChainSuccess, asyncFsChainError, @ptrCast(self));
    }

    /// Promise `catch`: convert a rejected operation into a resolved one. The
    /// handler returns zero to resolve or a negative status to keep rejecting.
    ///
    /// A resolved operation passes through with its value untouched, so the
    /// handler runs only on rejection. Registers on the same continuation record
    /// as `then`; null when the operation never started.
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

// The six *Async starters below all draw an AsyncFsOp from the fixed 24-entry
// leak pool (null once it is exhausted, since ops are never returned to it) and
// submit one FS request through the wrapper's event loop and reply endpoint.
// They are only usable from inside runAsyncApp: outside it the wrapper has no
// active config and the unwrap of its event loop traps. None of them blocks; the
// result is taken with AsyncFsOp.result once the returned operation settles.
/// Open `path` with `flags`; returns a promise-chainable operation. `path` is
/// copied into the operation and must be non-empty and under 256 bytes.
pub fn openAsync(path: []const u8, flags: i32) ?*AsyncFsOp {
    const op = allocOp() orelse return null;
    if (!stagePath(op, path)) return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_open_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), op.path[0..].ptr, flags, &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Read up to `dst.len` bytes for `fd`; `dst` must stay live until the
/// operation settles and `result` copies the payload into it. One request, so
/// the byte count `result` returns may be short of `dst.len`. Null for an empty
/// slice.
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

/// Write `src` to `fd`; `src` is copied synchronously into the transfer buffer,
/// so it need only be live for this call. Not chunked: one buffer is acquired
/// for the whole payload, so a `src` beyond the transfer-buffer limit fails the
/// acquire and returns null, and a short write is reported through `result`.
pub fn writeAsync(fd: i32, src: []const u8) ?*AsyncFsOp {
    if (src.len == 0) return null;
    const op = allocOp() orelse return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_write_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), fd, @ptrCast(src.ptr), @intCast(src.len), &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Close `fd`. Uses no transfer buffer; `result` returns the reply's status.
pub fn closeAsync(fd: i32) ?*AsyncFsOp {
    const op = allocOp() orelse return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_close_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), fd, &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Unlink `path`. A refusal by the backend arrives as an FS error message, not
/// an FS_IPC_RESP, so the operation rejects with -1 and the packed
/// WASMOS_ERR_FS_* code it carried is not surfaced.
pub fn unlinkAsync(path: []const u8) ?*AsyncFsOp {
    const op = allocOp() orelse return null;
    if (!stagePath(op, path)) return null;
    var request_id: i32 = 0;
    const future = wasmos_sys_wasm_fs_unlink_async(wasmos_sys_wasm_async_event_loop().?, &op.operation, host.fs_endpoint(), wasmos_sys_wasm_async_reply_endpoint(), op.path[0..].ptr, &request_id) orelse return null;
    op.future = future;
    return op;
}

/// Stat `path`; rejects when the path does not exist. On success `result`
/// returns the file size and the reply's arg1 carries the mode bits.
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
///
/// Blocks until that future settles: the wrapper alternates one coroutine step
/// with one event-loop poll, parking on the reply endpoint whenever the root
/// task is waiting, so it never spins. Returns the chain's resolved value
/// truncated to i32; every failure collapses to -1, including a rejected chain,
/// a null return from `start`, and a reply endpoint or root coroutine that could
/// not be created. The wrapper state is module-static, so one call is live at a
/// time.
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
