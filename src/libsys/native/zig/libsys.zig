const abi = @import("c_abi.zig");
const c = abi.c;

/// `wasmos_driver_api_t.ipc_recv` returned a message in `out_message`.
pub const IPC_OK: i32 = 0;
/// `wasmos_driver_api_t.ipc_recv` found the endpoint queue empty. Not an
/// error: it is the signal to yield or block rather than to give up.
pub const IPC_EMPTY: i32 = 1;
/// "No endpoint" in the unsigned native endpoint space; mirrors
/// WASMOS_SYS_NATIVE_ENDPOINT_NONE.
pub const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;

/// Reactor state for one receive endpoint (`wasmos_sys_native_event_loop_t`).
/// Caller-owned and bound to a driver api table by `eventLoopInit`.
pub const NativeEventLoop = c.wasmos_sys_native_event_loop_t;
/// State of one in-flight entropy request (`wasmos_sys_native_random_request_t`),
/// owned by the caller until its completion callback runs.
pub const NativeRandomRequest = c.wasmos_sys_native_random_request_t;
/// Single-worker cooperative scheduler (`wasmos_native_coroutine_runtime_t`).
pub const NativeCoroutineRuntime = c.wasmos_native_coroutine_runtime_t;
/// One stackful coroutine record (`wasmos_native_coroutine_t`). The record and
/// the stack it was spawned with stay caller-owned until it is dead.
pub const NativeCoroutine = c.wasmos_native_coroutine_t;
/// Single-settlement result cell (`wasmos_future_t`).
pub const Future = c.wasmos_future_t;
/// The settle side of a `Future` (`wasmos_promise_t`).
pub const Promise = c.wasmos_promise_t;
/// Caller-owned registration for one `futureThen` callback pair
/// (`wasmos_future_continuation_t`); it owns the child future returned by
/// `futureThen`, so it must outlive it.
pub const FutureContinuation = c.wasmos_future_continuation_t;
/// Caller-owned state for a `futureRace`/`futureAll` combinator
/// (`wasmos_future_group_t`).
pub const FutureGroup = c.wasmos_future_group_t;
/// Bridge from one non-blocking IPC request to a local future
/// (`wasmos_sys_native_ipc_future_t`).
pub const NativeIpcFuture = c.wasmos_sys_native_ipc_future_t;
/// Predicate deciding whether a reply resolves or rejects an IPC future:
/// zero resolves, anything else rejects.
pub const NativeIpcFutureReplyStatus = c.wasmos_sys_native_ipc_future_reply_status_fn;
/// Native service bootstrap record (`wasmos_sys_native_service_t`).
pub const NativeService = c.wasmos_sys_native_service_t;
/// Body of a native service, run as the root coroutine
/// (`wasmos_sys_native_service_main_fn`).
pub const NativeServiceMain = c.wasmos_sys_native_service_main_fn;
/// The single global an async native service defines
/// (`wasmos_sys_native_async_service_config_t`).
pub const NativeAsyncServiceConfig = c.wasmos_sys_native_async_service_config_t;
/// Coroutine entry point. Declared here rather than aliased from the C header
/// because the call sites pass a non-optional function pointer and `@ptrCast`
/// it to the header's optional form.
pub const NativeCoroutineEntry = *const fn (?*anyopaque) callconv(.c) void;
/// Continuation success callback: return 0 to resolve the child future with
/// `out_value`, or a negative status to reject it.
pub const FutureSuccess = c.wasmos_future_success_fn_t;
/// Continuation error callback, with the same return convention as
/// `FutureSuccess`.
pub const FutureError = c.wasmos_future_error_fn_t;

/// Zero a caller-owned runtime. Pass-through to
/// `wasmos_native_coroutine_runtime_init`.
pub fn coroutineRuntimeInit(runtime: *NativeCoroutineRuntime) void {
    c.wasmos_native_coroutine_runtime_init(runtime);
}

/// Run ready coroutines and continuations until nothing is runnable; returns
/// the number of coroutine resumes, or -1 for reentrant use. Pass-through.
pub fn coroutineRun(runtime: *NativeCoroutineRuntime) i32 {
    return c.wasmos_native_coroutine_run(runtime);
}

/// `coroutineRun` bounded to `budget` coroutine resumes; queued continuations
/// are still drained. Pass-through.
pub fn coroutineRunBudget(runtime: *NativeCoroutineRuntime, budget: usize) i32 {
    return c.wasmos_native_coroutine_run_budget(runtime, budget);
}

/// Prepare `coroutine` to run `entry(arg)` on `stack` and queue it ready;
/// returns 0 on success, -1 for a stack under 1024 bytes or a record that is
/// neither fresh nor dead. `stack` is borrowed and must stay valid and
/// otherwise unused until the coroutine is dead. Passes the slice as the C
/// pointer/length pair.
pub fn coroutineSpawn(runtime: *NativeCoroutineRuntime, coroutine: *NativeCoroutine, stack: []u8, entry: NativeCoroutineEntry, arg: ?*anyopaque) i32 {
    return c.wasmos_native_coroutine_spawn(runtime, coroutine, stack.ptr, stack.len, @ptrCast(entry), arg);
}

/// `coroutineSpawn` returning the coroutine's completion future instead of a
/// status, or null if it could not be started. The future points into the
/// coroutine record.
pub fn asyncStart(runtime: *NativeCoroutineRuntime, coroutine: *NativeCoroutine, stack: []u8, entry: NativeCoroutineEntry, arg: ?*anyopaque) ?*Future {
    return c.wasmos_async_start(runtime, coroutine, stack.ptr, stack.len, @ptrCast(entry), arg);
}

/// Requeue the running coroutine and switch to the scheduler, returning when it
/// is resumed. Traps when there is no running coroutine, so it must not be
/// called from the scheduler stack or a continuation callback. Pass-through.
pub fn coroutineYield() void {
    c.wasmos_native_coroutine_yield();
}

/// Terminate the running coroutine, resolving its completion future with
/// `result` - a negative result still resolves, it does not reject. Traps when
/// there is no running coroutine. Pass-through.
pub fn coroutineExit(result: i32) noreturn {
    c.wasmos_native_coroutine_exit(result);
}

/// Suspend until `coroutine` finishes, then return 0 and write its exit result
/// to `out_result`. Returns -1 without suspending when called outside a running
/// coroutine on one that has not finished. Pass-through.
pub fn coroutineJoin(coroutine: *NativeCoroutine, out_result: ?*i32) i32 {
    return c.wasmos_native_coroutine_join(coroutine, out_result);
}

/// Reset `future` to pending and bind `promise` to it. Only valid on a future
/// nobody is waiting on. Pass-through.
pub fn futureInit(future: *Future, promise: *Promise) void {
    c.wasmos_future_init(future, promise);
}

/// Settle the promise's future with `value`, waking waiters and queueing
/// continuations; returns false if it was already settled. Pass-through.
pub fn promiseResolve(promise: *Promise, value: usize) bool {
    return c.wasmos_promise_resolve(promise, value);
}

/// Settle the promise's future as failed. `status` must be negative: a
/// non-negative one settles nothing and returns false. Pass-through.
pub fn promiseReject(promise: *Promise, status: i32) bool {
    return c.wasmos_promise_reject(promise, status);
}

/// Non-blocking settlement test: false while pending, true with the status and
/// value copied out once settled. Pass-through.
pub fn futurePoll(future: *const Future, out_status: ?*i32, out_value: ?*usize) bool {
    return c.wasmos_future_poll(future, out_status, out_value);
}

/// Suspend the calling coroutine until the future settles, then return its
/// status (0 or negative). Stackful: locals survive the suspension. Returns -1
/// without suspending when called outside a running coroutine. Pass-through.
pub fn futureAwait(future: *Future, out_value: ?*usize) i32 {
    return c.wasmos_future_await(future, out_value);
}

/// Register a scheduled transformation and return its child future, or null on
/// invalid input. The callbacks run from `coroutineRun`, never inline from a
/// resolve. Pass-through.
pub fn futureThen(runtime: *NativeCoroutineRuntime, future: *Future, continuation: *FutureContinuation, on_success: FutureSuccess, on_error: FutureError, user: ?*anyopaque) ?*Future {
    return c.wasmos_future_then(runtime, future, continuation, on_success, on_error, user);
}

/// Settle from the first of `inputs` to settle. Adds the slice-length check the
/// C API cannot express: returns null when `inputs` is empty or its length does
/// not match `continuations`. `group` and `continuations` must stay live until
/// the returned future settles.
pub fn futureRace(runtime: *NativeCoroutineRuntime, group: *FutureGroup, inputs: []const *Future, continuations: []FutureContinuation) ?*Future {
    if (inputs.len == 0 or inputs.len != continuations.len) return null;
    return c.wasmos_future_race(runtime, group, @ptrCast(inputs.ptr), inputs.len, continuations.ptr);
}

/// Resolve with `values` once every input resolves, or reject with the first
/// failure. Adds the same slice-length check as `futureRace`, extended to
/// `values`. `group`, `continuations` and `values` must stay live until the
/// returned future settles.
pub fn futureAll(runtime: *NativeCoroutineRuntime, group: *FutureGroup, inputs: []const *Future, values: []usize, continuations: []FutureContinuation) ?*Future {
    if (inputs.len == 0 or inputs.len != values.len or inputs.len != continuations.len) return null;
    return c.wasmos_future_all(runtime, group, @ptrCast(inputs.ptr), inputs.len, values.ptr, continuations.ptr);
}

/// Zero an IPC-future record and install `reply_status` as its resolve/reject
/// predicate (null accepts every reply). Required before the first send and
/// before every reuse. Pass-through.
pub fn ipcFutureInit(operation: *NativeIpcFuture, reply_status: NativeIpcFutureReplyStatus, user: ?*anyopaque) void {
    c.wasmos_sys_native_ipc_future_init(operation, reply_status, user);
}

/// Issue the request as a loop intent and return the record's future, or null
/// when the record is already in flight or already settled. A failed send
/// returns the future ALREADY REJECTED, so a non-null result does not mean the
/// request is on the wire. Pass-through.
pub fn ipcFutureSend(loop: *NativeEventLoop, operation: *NativeIpcFuture, destination_endpoint: u32, source_endpoint: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out_request_id: ?*u32) ?*Future {
    return c.wasmos_sys_native_ipc_future_send(loop, operation, destination_endpoint, source_endpoint, msg_type, arg0, arg1, arg2, arg3, out_request_id);
}

/// Stop tracking the reply and reject the future with `status` (a non-negative
/// status is normalised to -1). The transport request is not recalled: a late
/// reply falls through to the loop's handlers. Pass-through.
pub fn ipcFutureCancel(operation: *NativeIpcFuture, status: i32) void {
    c.wasmos_sys_native_ipc_future_cancel(operation, status);
}

/// Zero a service record and record `root_stack` as the root coroutine's stack.
/// The slice is borrowed for the whole run. Passes it as the C pointer/length
/// pair.
pub fn serviceInit(service: *NativeService, root_stack: []u8) void {
    c.wasmos_sys_native_service_init(service, root_stack.ptr, root_stack.len);
}

/// Start `main` as the root coroutine and pump it until it exits, returning
/// main's result or -1 on a bootstrap failure. Blocks for the service's whole
/// lifetime. `api` is any pointer to the driver api table. Pass-through.
pub fn serviceRun(service: *NativeService, api: anytype, main: NativeServiceMain, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_service_run(service, asApi(api), main, user);
}
/// Recursive mutex whose locking logic lives in the kernel. Layout-compatible
/// with `wasmos_sys_mutex_t`, so the two may alias the same memory; a
/// zero-initialised value is unlocked.
pub const Mutex = extern struct {
    owner_tid: u32,
    recursion_depth: u32,

    /// Reset to unlocked. Only valid before first use or when nobody holds it:
    /// it forgets a held lock rather than releasing it.
    pub fn init(self: *Mutex) void {
        c.wasmos_sys_mutex_init(@ptrCast(self));
    }

    /// Acquire without waiting: 0 when held by this thread (including a
    /// recursive re-entry), 1 when another thread holds it, -1 on error.
    pub fn tryLock(self: *Mutex, api: anytype) i32 {
        return c.wasmos_sys_mutex_try_lock(asApi(api), @ptrCast(self));
    }

    /// Acquire, yielding between attempts until it succeeds; returns 0 once
    /// held or -1 on error, never 1. Contention is a yield-spin, not a sleep.
    pub fn lock(self: *Mutex, api: anytype) i32 {
        return c.wasmos_sys_mutex_lock(asApi(api), @ptrCast(self));
    }

    /// Release one level of ownership; the mutex frees only when the recursion
    /// depth reaches zero. Returns 0, or <0 for a non-owner.
    pub fn unlock(self: *Mutex, api: anytype) i32 {
        return c.wasmos_sys_mutex_unlock(asApi(api), @ptrCast(self));
    }
};

fn asApi(api_ptr: anytype) *c.wasmos_driver_api_t {
    return @ptrCast(@alignCast(api_ptr));
}

fn asMsg(msg_ptr: anytype) *c.nd_ipc_message_t {
    return @ptrCast(@alignCast(msg_ptr));
}

/// Pack the first 16 bytes of `name` into four IPC args, zero-filling the rest;
/// a longer name is truncated and a 16-byte name carries no terminator. Passes
/// the slice as the C pointer/length pair.
pub fn packName16(name: []const u8, out: *[4]u32) void {
    c.wasmos_sys_ipc_pack_name16_native(name.ptr, @intCast(name.len), out);
}

/// Inverse of `packName16`: writes at most `out.len - 1` bytes plus a NUL,
/// stopping at the first zero byte. Adds a guard for an empty `out`.
pub fn unpackName16(arg0: u32, arg1: u32, arg2: u32, arg3: u32, out: []u8) void {
    if (out.len == 0) return;
    c.wasmos_sys_ipc_unpack_name16_native(arg0, arg1, arg2, arg3, out.ptr, @intCast(out.len));
}

/// The calling thread's id. Not a libsys call: it dispatches straight through
/// the api table, so it PANICS when the table has no `thread_current_tid`.
pub fn currentTid(api: anytype) u32 {
    return asApi(api).thread_current_tid.?();
}

/// Park a terminal-state service: receive on `receiver_endpoint` forever,
/// discarding everything and yielding when the queue is empty. Does not return
/// unless the api table is unusable. Pass-through.
pub fn ipcRecvLoop(api: anytype, receiver_endpoint: u32) void {
    c.wasmos_sys_ipc_recv_loop_native(asApi(api), receiver_endpoint);
}

/// Receive until a message carrying `request_id` arrives; returns 0 with it in
/// `out_message`, or -1 on a receive error. Everything else on the endpoint is
/// consumed and DISCARDED, so this belongs on a private reply endpoint. Blocks
/// by yield-spinning, forever if the reply never comes. Pass-through.
pub fn ipcRecvMatching(api: anytype, receiver_endpoint: u32, request_id: u32, out_message: anytype) i32 {
    return c.wasmos_sys_ipc_recv_matching_native(asApi(api), receiver_endpoint, request_id, asMsg(out_message));
}

/// Send, retrying up to `retries` times (0 means one attempt) with a yield
/// between tries. Returns 0 on success, -3 once the retries are used up on a
/// full destination queue, and any other send status unchanged. Does not wait
/// for a reply. Pass-through.
pub fn ipcSendRetry(api: anytype, destination_endpoint: u32, source_endpoint: u32, msg_type: u32, request_id: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, retries: u32) i32 {
    return c.wasmos_sys_ipc_send_retry_native(asApi(api), destination_endpoint, source_endpoint, msg_type, request_id, arg0, arg1, arg2, arg3, retries);
}

/// Send and block on `source_endpoint` for the matching reply; returns 0 with
/// the reply in `out_message`, -1 on error. Inherits `ipcRecvMatching`'s
/// discard-and-yield-spin behaviour. Pass-through.
pub fn ipcCall(api: anytype, source_endpoint: u32, destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out_message: anytype) i32 {
    return c.wasmos_sys_ipc_call_native(asApi(api), source_endpoint, destination, request_id, msg_type, arg0, arg1, arg2, arg3, asMsg(out_message));
}

/// Register the calling context under `name` (at most 16 bytes) and block for
/// the reply. Returns the assigned service handle, or -1 on failure. Passes the
/// slice as the C pointer/length pair.
pub fn svcRegister(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    return c.wasmos_sys_svc_register_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id);
}

/// Resolve `name` to a service endpoint, blocking for the reply. Returns the
/// endpoint id (>= 0), or -1 when the lookup fails or the name is unknown.
/// Passes the slice as the C pointer/length pair.
pub fn svcLookup(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    return c.wasmos_sys_svc_lookup_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id);
}

/// `svcLookup` retried up to `attempts` times (0 means one attempt), yielding
/// between tries so a service that has not registered yet gets a chance to run.
/// Each attempt consumes one id from `request_id_base` upwards. Returns the
/// endpoint id or -1. Pass-through.
pub fn svcLookupRetry(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id_base: u32, attempts: u32) i32 {
    return c.wasmos_sys_svc_lookup_retry_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id_base, attempts);
}

/// `svcLookupRetry` with the failure folded into the type: null instead of a
/// negative status, otherwise the endpoint id as a u32.
pub fn svcLookupEndpointRetry(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id_base: u32, attempts: u32) ?u32 {
    const ep = svcLookupRetry(api, proc_endpoint, source_endpoint, name, request_id_base, attempts);
    if (ep < 0) return null;
    return @bitCast(ep);
}

/// Start a non-blocking entropy request filling `out`. Returns 0 once the first
/// chunk is sent, after which the outcome arrives only through `on_complete`; a
/// negative packed hrng status means nothing was started and `on_complete` will
/// NOT run. `request` and `out` stay caller-owned until it does, and completion
/// is driven by `eventLoopPoll`. Passes the slice as the C pointer/length pair.
pub fn randomBytesAsync(loop: *NativeEventLoop, hrng_endpoint: u32, out: []u8, request: *NativeRandomRequest, on_complete: *const fn (?*anyopaque, i32) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_random_bytes_async(loop, hrng_endpoint, out.ptr, @intCast(out.len), request, @ptrCast(on_complete), user);
}

/// `randomBytesAsync` for a single u32, drawn straight into `out_value` in
/// whatever byte order the entropy source produced. Pass-through.
pub fn randomIntAsync(loop: *NativeEventLoop, hrng_endpoint: u32, out_value: *u32, request: *NativeRandomRequest, on_complete: *const fn (?*anyopaque, i32) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_random_int_async(loop, hrng_endpoint, out_value, request, @ptrCast(on_complete), user);
}

/// `randomBytesAsync` for an f32 uniformly distributed in [0, 1). `out_value`
/// is written only on success, when the request completes. Pass-through.
pub fn randomFloatAsync(loop: *NativeEventLoop, hrng_endpoint: u32, out_value: *f32, request: *NativeRandomRequest, on_complete: *const fn (?*anyopaque, i32) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_random_float_async(loop, hrng_endpoint, out_value, request, @ptrCast(on_complete), user);
}

/// Forward copy of `len` bytes; the ranges must not overlap and neither pointer
/// is bounds-checked. Takes raw many-item pointers, so the caller carries the
/// length itself. Pass-through.
pub fn byteCopy(dst: [*]u8, src: [*]const u8, len: usize) void {
    c.wasmos_sys_byte_copy_native(dst, src, @intCast(len));
}

/// Bounds-checked big-endian load at `off`, or null when the field would run
/// past the end of `data`. Turns the C -1 status into an optional.
pub fn beU16(data: []const u8, off: usize) ?u16 {
    var v: u16 = 0;
    if (c.wasmos_sys_be_u16_native(data.ptr, @intCast(data.len), @intCast(off), &v) != 0) return null;
    return v;
}

/// Signed counterpart of `beU16`, with the same bounds behaviour.
pub fn beI16(data: []const u8, off: usize) ?i16 {
    var v: i16 = 0;
    if (c.wasmos_sys_be_i16_native(data.ptr, @intCast(data.len), @intCast(off), &v) != 0) return null;
    return v;
}

/// 32-bit counterpart of `beU16`, with the same bounds behaviour.
pub fn beU32(data: []const u8, off: usize) ?u32 {
    var v: u32 = 0;
    if (c.wasmos_sys_be_u32_native(data.ptr, @intCast(data.len), @intCast(off), &v) != 0) return null;
    return v;
}

/// Locate an OpenType/TrueType table by its 4-character tag and return its file
/// offset, or null when the tag is absent or the directory is malformed. The
/// offset is not validated against `data.len`. Turns the C -1 status into an
/// optional.
pub fn findTable(data: []const u8, tag: [4]u8) ?usize {
    var off: u32 = 0;
    if (c.wasmos_sys_find_table_native(data.ptr, @intCast(data.len), &tag, &off) != 0) return null;
    return @intCast(off);
}

/// Pack two values into the halves of one IPC arg: `a` in bits 0-15, `b` in
/// bits 16-31. Pass-through.
pub fn packU16Pair(a: u32, b: u32) u32 {
    return c.wasmos_sys_pack_u16_pair_native(a, b);
}

/// Signed form of `packU16Pair`: each operand is truncated to i16 first, so
/// unpacking must sign-extend. Pass-through.
pub fn packS16Pair(a: i32, b: i32) u32 {
    return c.wasmos_sys_pack_s16_pair_native(a, b);
}

/// Write `value` as "0x" plus 8 lowercase hex digits and a NUL, returning the
/// 10 characters written. Needs `out.len >= 11`; a shorter buffer writes
/// nothing and returns 0. Adds a guard for an empty `out`.
pub fn hexU32(value: u32, out: []u8) usize {
    if (out.len == 0) return 0;
    return @intCast(c.wasmos_sys_hex_u32_native(value, out.ptr, @intCast(out.len)));
}

/// Bind a caller-owned loop to `api` and `receiver_endpoint` and clear its
/// tables. `request_id_base` seeds the counter `intentSend` draws from, so
/// concurrent loops need disjoint ranges. Pass-through.
pub fn eventLoopInit(loop: *NativeEventLoop, api: anytype, receiver_endpoint: u32, request_id_base: u32) void {
    c.wasmos_sys_native_event_loop_init(loop, asApi(api), receiver_endpoint, request_id_base);
}

/// Register (or replace) the handler for `msg_type`; returns 0, or -1 when the
/// handler table is full of other types. The callback receives the message as
/// an opaque pointer to cast to the driver message type. Pass-through.
pub fn eventRegister(loop: *NativeEventLoop, msg_type: u32, on_message: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_event_register(loop, msg_type, @ptrCast(on_message), user);
}

/// Install the fallback handler for messages matching no intent and no
/// registered type; returns 0, or -1 on a null callback. Replaces any previous
/// default. Pass-through.
pub fn eventSetDefault(loop: *NativeEventLoop, on_message: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_event_set_default(loop, @ptrCast(on_message), user);
}

/// Send a request and register `on_resolve` for the reply carrying the
/// allocated id, reported through `out_request_id`. Non-blocking: the reply is
/// delivered from `eventLoopPoll`. Returns 0, -1 for a full intent table or an
/// unusable api table, or the transport status when the send fails.
/// Pass-through.
pub fn intentSend(loop: *NativeEventLoop, destination_endpoint: u32, source_endpoint: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, on_resolve: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque, out_request_id: ?*u32) i32 {
    return c.wasmos_sys_native_intent_send(loop, destination_endpoint, source_endpoint, msg_type, arg0, arg1, arg2, arg3, @ptrCast(on_resolve), user, out_request_id);
}

/// `intentSend` with a caller-chosen `request_id`, which must be non-zero and
/// not already pending on this loop. The loop's own counter is untouched, so
/// mixing both forms means keeping the ranges disjoint. Pass-through.
pub fn intentSendWithRequestId(loop: *NativeEventLoop, destination_endpoint: u32, source_endpoint: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, on_resolve: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_intent_send_with_request_id(loop, destination_endpoint, source_endpoint, request_id, msg_type, arg0, arg1, arg2, arg3, @ptrCast(on_resolve), user);
}

/// Drop local tracking of a pending request so its callback can no longer run.
/// The request is not recalled: a late reply falls through to the loop's type
/// or default handler. Pass-through.
pub fn intentCancel(loop: *NativeEventLoop, request_id: u32) void {
    c.wasmos_sys_native_intent_cancel(loop, request_id);
}

/// Dispatch at most `budget` queued messages (0 means 1) and return how many
/// ran, or -1 on an unusable api table or a receive error. NEVER blocks: the
/// native loop owns no select-set, so a service that must sleep at idle does so
/// in its service idle hook. Pass-through.
pub fn eventLoopPoll(loop: *NativeEventLoop, budget: u32) i32 {
    return c.wasmos_sys_native_event_loop_poll(loop, budget);
}
