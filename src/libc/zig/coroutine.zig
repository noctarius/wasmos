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

    pub fn init(self: *Runtime) void { wasmos_wasm_runtime_init(self); }
    pub fn run(self: *Runtime) i32 { return wasmos_wasm_coroutine_run(self); }
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

    pub fn init(self: *Future, promise: *Promise) void { wasmos_future_init(self, promise); }
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
    pub fn then(self: *Future, runtime: *Runtime, continuation: *Continuation,
                success: ?SuccessCallback, failure: ?ErrorCallback, user: ?*anyopaque) ?*Future {
        return wasmos_future_then(runtime, self, continuation, success, failure, user);
    }
};

pub const Promise = extern struct {
    future: ?*Future = null,
    pub fn resolve(self: *Promise, value: usize) bool { return wasmos_promise_resolve(self, value); }
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
    pub fn join(self: *Coroutine, result: ?*i32) i32 { return wasmos_wasm_coroutine_join(self, result); }
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
    values: ?[*]usize = null,
    count: usize = 0,
    completed: usize = 0,
    kind: GroupKind = .race,
    settled: bool = false,
    active: bool = false,

    pub fn race(self: *FutureGroup, runtime: *Runtime, inputs: []const *Future,
                continuations: []Continuation) ?*Future {
        if (inputs.len == 0 or inputs.len != continuations.len) return null;
        return wasmos_future_race(runtime, self, @ptrCast(inputs.ptr), inputs.len, continuations.ptr);
    }
    pub fn all(self: *FutureGroup, runtime: *Runtime, inputs: []const *Future, values: []usize,
               continuations: []Continuation) ?*Future {
        if (inputs.len == 0 or inputs.len != values.len or inputs.len != continuations.len) return null;
        return wasmos_future_all(runtime, self, @ptrCast(inputs.ptr), inputs.len, values.ptr, continuations.ptr);
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
