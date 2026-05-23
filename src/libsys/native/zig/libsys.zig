const abi = @import("c_abi.zig");
const c = abi.c;

pub const IPC_OK: i32 = 0;
pub const IPC_EMPTY: i32 = 1;
pub const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;
pub const NativeEventLoop = c.wasmos_sys_native_event_loop_t;

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

pub fn ipcCallBudgeted(api: anytype, recv_endpoint: u32, source_endpoint: u32, destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out_message: anytype, max_empty_polls: u32) i32 {
    var req: c.nd_ipc_message_t = .{
        .type = msg_type,
        .source = source_endpoint,
        .destination = destination,
        .request_id = request_id,
        .arg0 = arg0,
        .arg1 = arg1,
        .arg2 = arg2,
        .arg3 = arg3,
    };
    if (asApi(api).ipc_send.?(asApi(api).sched_current_pid.?(), destination, &req) != IPC_OK) return -1;

    var polls: u32 = 0;
    while (true) {
        const rc = asApi(api).ipc_recv.?(asApi(api).sched_current_pid.?(), recv_endpoint, asMsg(out_message));
        if (rc == IPC_EMPTY) {
            if (polls >= max_empty_polls) return -1;
            polls +%= 1;
            asApi(api).sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) return -1;
        if (asMsg(out_message).request_id == request_id) return 0;
    }
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

pub fn bufferCopyFrom(api: anytype, kind: u32, source_endpoint: u32, borrow_flags: u32, dst: [*]u8, len: i32, offset: i32) i32 {
    return c.wasmos_sys_buffer_copy_from_native(asApi(api), kind, source_endpoint, borrow_flags, dst, len, offset);
}

pub fn bufferWriteTo(api: anytype, kind: u32, source_endpoint: u32, borrow_flags: u32, src: [*]const u8, len: i32, offset: i32) i32 {
    return c.wasmos_sys_buffer_write_to_native(asApi(api), kind, source_endpoint, borrow_flags, src, len, offset);
}

pub fn fsBufferCopyFromEndpoint(api: anytype, source_endpoint: u32, dst: [*]u8, len: i32, offset: i32) i32 {
    return c.wasmos_sys_fs_buffer_copy_from_endpoint_native(asApi(api), source_endpoint, dst, len, offset);
}

pub fn fsBufferWriteToEndpoint(api: anytype, source_endpoint: u32, src: [*]const u8, len: i32, offset: i32) i32 {
    return c.wasmos_sys_fs_buffer_write_to_endpoint_native(asApi(api), source_endpoint, src, len, offset);
}

pub fn fsReadPath(api: anytype, fs_endpoint: u32, reply_endpoint: u32, request_id: u32, path: []const u8, out_text: []u8) i32 {
    if (out_text.len == 0) return -1;
    return c.wasmos_sys_fs_read_path_native(asApi(api), fs_endpoint, reply_endpoint, request_id, path.ptr, @intCast(path.len), out_text.ptr, @intCast(out_text.len));
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

pub fn eventLoopPoll(loop: *NativeEventLoop, budget: u32) i32 {
    return c.wasmos_sys_native_event_loop_poll(loop, budget);
}
