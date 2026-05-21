const abi = @import("c_abi.zig");
const c = abi.c;

pub const IPC_OK: i32 = 0;
pub const IPC_EMPTY: i32 = 1;
pub const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;

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

pub fn svcRegister(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    return c.wasmos_sys_svc_register_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id);
}

pub fn svcLookup(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    return c.wasmos_sys_svc_lookup_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id);
}

pub fn svcLookupRetry(api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id_base: u32, attempts: u32) i32 {
    return c.wasmos_sys_svc_lookup_retry_native(asApi(api), proc_endpoint, source_endpoint, name.ptr, @intCast(name.len), request_id_base, attempts);
}

pub fn bufferCopyFrom(api: anytype, kind: u32, source_endpoint: u32, borrow_flags: u32, dst: [*]u8, len: i32, offset: i32) i32 {
    return c.wasmos_sys_buffer_copy_from_native(asApi(api), kind, source_endpoint, borrow_flags, dst, len, offset);
}

pub fn bufferWriteTo(api: anytype, kind: u32, source_endpoint: u32, borrow_flags: u32, src: [*]const u8, len: i32, offset: i32) i32 {
    return c.wasmos_sys_buffer_write_to_native(asApi(api), kind, source_endpoint, borrow_flags, src, len, offset);
}

pub fn fsReadPath(api: anytype, fs_endpoint: u32, reply_endpoint: u32, request_id: u32, path: []const u8, out_text: []u8) i32 {
    if (out_text.len == 0) return -1;
    return c.wasmos_sys_fs_read_path_native(asApi(api), fs_endpoint, reply_endpoint, request_id, path.ptr, @intCast(path.len), out_text.ptr, @intCast(out_text.len));
}
