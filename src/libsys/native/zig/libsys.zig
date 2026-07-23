const abi = @import("c_abi.zig");
const c = abi.c;

pub const IPC_OK: i32 = 0;
pub const IPC_EMPTY: i32 = 1;
pub const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;
pub const NativeEventLoop = c.wasmos_sys_native_event_loop_t;
pub const NativeRandomRequest = c.wasmos_sys_native_random_request_t;
pub const NativeCoroutineRuntime = c.wasmos_native_coroutine_runtime_t;
pub const NativeCoroutine = c.wasmos_native_coroutine_t;
pub const Future = c.wasmos_future_t;
pub const Promise = c.wasmos_promise_t;
pub const FutureContinuation = c.wasmos_future_continuation_t;
pub const FutureGroup = c.wasmos_future_group_t;
pub const NativeIpcFuture = c.wasmos_sys_native_ipc_future_t;
pub const NativeIpcFutureReplyStatus = c.wasmos_sys_native_ipc_future_reply_status_fn;
pub const NativeCoroutineEntry = *const fn (?*anyopaque) callconv(.c) void;
pub const FutureSuccess = c.wasmos_future_success_fn_t;
pub const FutureError = c.wasmos_future_error_fn_t;

pub fn coroutineRuntimeInit(runtime: *NativeCoroutineRuntime) void {
    c.wasmos_native_coroutine_runtime_init(runtime);
}

pub fn coroutineRun(runtime: *NativeCoroutineRuntime) i32 {
    return c.wasmos_native_coroutine_run(runtime);
}

pub fn coroutineSpawn(runtime: *NativeCoroutineRuntime, coroutine: *NativeCoroutine, stack: []u8, entry: NativeCoroutineEntry, arg: ?*anyopaque) i32 {
    return c.wasmos_native_coroutine_spawn(runtime, coroutine, stack.ptr, stack.len, @ptrCast(entry), arg);
}

pub fn asyncStart(runtime: *NativeCoroutineRuntime, coroutine: *NativeCoroutine, stack: []u8, entry: NativeCoroutineEntry, arg: ?*anyopaque) ?*Future {
    return c.wasmos_async_start(runtime, coroutine, stack.ptr, stack.len, @ptrCast(entry), arg);
}

pub fn coroutineYield() void {
    c.wasmos_native_coroutine_yield();
}

pub fn coroutineExit(result: i32) noreturn {
    c.wasmos_native_coroutine_exit(result);
}

pub fn coroutineJoin(coroutine: *NativeCoroutine, out_result: ?*i32) i32 {
    return c.wasmos_native_coroutine_join(coroutine, out_result);
}

pub fn futureInit(future: *Future, promise: *Promise) void {
    c.wasmos_future_init(future, promise);
}

pub fn promiseResolve(promise: *Promise, value: usize) bool {
    return c.wasmos_promise_resolve(promise, value);
}

pub fn promiseReject(promise: *Promise, status: i32) bool {
    return c.wasmos_promise_reject(promise, status);
}

pub fn futurePoll(future: *const Future, out_status: ?*i32, out_value: ?*usize) bool {
    return c.wasmos_future_poll(future, out_status, out_value);
}

pub fn futureAwait(future: *Future, out_value: ?*usize) i32 {
    return c.wasmos_future_await(future, out_value);
}

pub fn futureThen(runtime: *NativeCoroutineRuntime, future: *Future, continuation: *FutureContinuation, on_success: FutureSuccess, on_error: FutureError, user: ?*anyopaque) ?*Future {
    return c.wasmos_future_then(runtime, future, continuation, on_success, on_error, user);
}

pub fn futureRace(runtime: *NativeCoroutineRuntime, group: *FutureGroup, inputs: []const *Future, continuations: []FutureContinuation) ?*Future {
    if (inputs.len == 0 or inputs.len != continuations.len) return null;
    return c.wasmos_future_race(runtime, group, @ptrCast(inputs.ptr), inputs.len, continuations.ptr);
}

pub fn futureAll(runtime: *NativeCoroutineRuntime, group: *FutureGroup, inputs: []const *Future, values: []usize, continuations: []FutureContinuation) ?*Future {
    if (inputs.len == 0 or inputs.len != values.len or inputs.len != continuations.len) return null;
    return c.wasmos_future_all(runtime, group, @ptrCast(inputs.ptr), inputs.len, values.ptr, continuations.ptr);
}

pub fn ipcFutureInit(operation: *NativeIpcFuture, reply_status: NativeIpcFutureReplyStatus, user: ?*anyopaque) void {
    c.wasmos_sys_native_ipc_future_init(operation, reply_status, user);
}

pub fn ipcFutureSend(loop: *NativeEventLoop, operation: *NativeIpcFuture, destination_endpoint: u32, source_endpoint: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out_request_id: ?*u32) ?*Future {
    return c.wasmos_sys_native_ipc_future_send(loop, operation, destination_endpoint, source_endpoint, msg_type, arg0, arg1, arg2, arg3, out_request_id);
}

pub fn ipcFutureCancel(operation: *NativeIpcFuture, status: i32) void {
    c.wasmos_sys_native_ipc_future_cancel(operation, status);
}
pub const Mutex = extern struct {
    owner_tid: u32,
    recursion_depth: u32,

    pub fn init(self: *Mutex) void {
        c.wasmos_sys_mutex_init(@ptrCast(self));
    }

    pub fn tryLock(self: *Mutex, api: anytype) i32 {
        return c.wasmos_sys_mutex_try_lock(asApi(api), @ptrCast(self));
    }

    pub fn lock(self: *Mutex, api: anytype) i32 {
        return c.wasmos_sys_mutex_lock(asApi(api), @ptrCast(self));
    }

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

pub fn packName16(name: []const u8, out: *[4]u32) void {
    c.wasmos_sys_ipc_pack_name16_native(name.ptr, @intCast(name.len), out);
}

pub fn unpackName16(arg0: u32, arg1: u32, arg2: u32, arg3: u32, out: []u8) void {
    if (out.len == 0) return;
    c.wasmos_sys_ipc_unpack_name16_native(arg0, arg1, arg2, arg3, out.ptr, @intCast(out.len));
}

pub fn currentTid(api: anytype) u32 {
    return asApi(api).thread_current_tid.?();
}

pub fn ipcRecvLoop(api: anytype, receiver_endpoint: u32) void {
    c.wasmos_sys_ipc_recv_loop_native(asApi(api), receiver_endpoint);
}

pub fn ipcRecvMatching(api: anytype, receiver_endpoint: u32, request_id: u32, out_message: anytype) i32 {
    return c.wasmos_sys_ipc_recv_matching_native(asApi(api), receiver_endpoint, request_id, asMsg(out_message));
}

pub fn ipcSendRetry(api: anytype, destination_endpoint: u32, source_endpoint: u32, msg_type: u32, request_id: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, retries: u32) i32 {
    return c.wasmos_sys_ipc_send_retry_native(asApi(api), destination_endpoint, source_endpoint, msg_type, request_id, arg0, arg1, arg2, arg3, retries);
}

pub fn ipcCall(api: anytype, source_endpoint: u32, destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out_message: anytype) i32 {
    return c.wasmos_sys_ipc_call_native(asApi(api), source_endpoint, destination, request_id, msg_type, arg0, arg1, arg2, arg3, asMsg(out_message));
}

pub fn svcRegister(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    return c.wasmos_sys_svc_register_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id);
}

pub fn svcLookup(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    return c.wasmos_sys_svc_lookup_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id);
}

pub fn svcLookupRetry(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id_base: u32, attempts: u32) i32 {
    return c.wasmos_sys_svc_lookup_retry_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id_base, attempts);
}

pub fn svcLookupEndpointRetry(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id_base: u32, attempts: u32) ?u32 {
    const ep = svcLookupRetry(api, proc_endpoint, source_endpoint, name, request_id_base, attempts);
    if (ep < 0) return null;
    return @bitCast(ep);
}

pub fn randomBytesAsync(loop: *NativeEventLoop, hrng_endpoint: u32, out: []u8, request: *NativeRandomRequest, on_complete: *const fn (?*anyopaque, i32) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_random_bytes_async(loop, hrng_endpoint, out.ptr, @intCast(out.len), request, @ptrCast(on_complete), user);
}

pub fn randomIntAsync(loop: *NativeEventLoop, hrng_endpoint: u32, out_value: *u32, request: *NativeRandomRequest, on_complete: *const fn (?*anyopaque, i32) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_random_int_async(loop, hrng_endpoint, out_value, request, @ptrCast(on_complete), user);
}

pub fn randomFloatAsync(loop: *NativeEventLoop, hrng_endpoint: u32, out_value: *f32, request: *NativeRandomRequest, on_complete: *const fn (?*anyopaque, i32) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_random_float_async(loop, hrng_endpoint, out_value, request, @ptrCast(on_complete), user);
}

pub fn byteCopy(dst: [*]u8, src: [*]const u8, len: usize) void {
    c.wasmos_sys_byte_copy_native(dst, src, @intCast(len));
}

pub fn beU16(data: []const u8, off: usize) ?u16 {
    var v: u16 = 0;
    if (c.wasmos_sys_be_u16_native(data.ptr, @intCast(data.len), @intCast(off), &v) != 0) return null;
    return v;
}

pub fn beI16(data: []const u8, off: usize) ?i16 {
    var v: i16 = 0;
    if (c.wasmos_sys_be_i16_native(data.ptr, @intCast(data.len), @intCast(off), &v) != 0) return null;
    return v;
}

pub fn beU32(data: []const u8, off: usize) ?u32 {
    var v: u32 = 0;
    if (c.wasmos_sys_be_u32_native(data.ptr, @intCast(data.len), @intCast(off), &v) != 0) return null;
    return v;
}

pub fn findTable(data: []const u8, tag: [4]u8) ?usize {
    var off: u32 = 0;
    if (c.wasmos_sys_find_table_native(data.ptr, @intCast(data.len), &tag, &off) != 0) return null;
    return @intCast(off);
}

pub fn packU16Pair(a: u32, b: u32) u32 {
    return c.wasmos_sys_pack_u16_pair_native(a, b);
}

pub fn packS16Pair(a: i32, b: i32) u32 {
    return c.wasmos_sys_pack_s16_pair_native(a, b);
}

pub fn hexU32(value: u32, out: []u8) usize {
    if (out.len == 0) return 0;
    return @intCast(c.wasmos_sys_hex_u32_native(value, out.ptr, @intCast(out.len)));
}

pub fn eventLoopInit(loop: *NativeEventLoop, api: anytype, receiver_endpoint: u32, request_id_base: u32) void {
    c.wasmos_sys_native_event_loop_init(loop, asApi(api), receiver_endpoint, request_id_base);
}

pub fn eventRegister(loop: *NativeEventLoop, msg_type: u32, on_message: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_event_register(loop, msg_type, @ptrCast(on_message), user);
}

pub fn eventSetDefault(loop: *NativeEventLoop, on_message: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_event_set_default(loop, @ptrCast(on_message), user);
}

pub fn intentSend(loop: *NativeEventLoop, destination_endpoint: u32, source_endpoint: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, on_resolve: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque, out_request_id: ?*u32) i32 {
    return c.wasmos_sys_native_intent_send(loop, destination_endpoint, source_endpoint, msg_type, arg0, arg1, arg2, arg3, @ptrCast(on_resolve), user, out_request_id);
}

pub fn intentSendWithRequestId(loop: *NativeEventLoop, destination_endpoint: u32, source_endpoint: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, on_resolve: *const fn (?*anyopaque, ?*const anyopaque) callconv(.c) void, user: ?*anyopaque) i32 {
    return c.wasmos_sys_native_intent_send_with_request_id(loop, destination_endpoint, source_endpoint, request_id, msg_type, arg0, arg1, arg2, arg3, @ptrCast(on_resolve), user);
}

pub fn intentCancel(loop: *NativeEventLoop, request_id: u32) void {
    c.wasmos_sys_native_intent_cancel(loop, request_id);
}

pub fn eventLoopPoll(loop: *NativeEventLoop, budget: u32) i32 {
    return c.wasmos_sys_native_event_loop_poll(loop, budget);
}
