const abi = @import("c_abi.zig");
const c = abi.c;

pub const IPC_OK: i32 = 0;
pub const IPC_EMPTY: i32 = 1;
pub const IPC_ENDPOINT_NONE: u32 = 0xFFFF_FFFF;

pub fn packName16(name: []const u8, out: *[4]u32) void {
    out.* = .{ 0, 0, 0, 0 };
    var i: usize = 0;
    while (i < name.len and i < 16) : (i += 1) {
        const slot: usize = i / 4;
        const shift: u5 = @intCast((i % 4) * 8);
        out[slot] |= (@as(u32, name[i]) << shift);
    }
}

pub fn ipcRecvMatching(api: anytype, receiver_endpoint: u32, request_id: u32, out_message: anytype) i32 {
    const ctx_id = api.sched_current_pid.?();
    while (true) {
        const rc = api.ipc_recv.?(ctx_id, receiver_endpoint, out_message);
        if (rc == IPC_EMPTY) {
            api.sched_yield.?();
            continue;
        }
        if (rc != IPC_OK) {
            return -1;
        }
        if (out_message.request_id == request_id) {
            return 0;
        }
    }
}

pub fn ipcCall(comptime Msg: type, api: anytype, source_endpoint: u32, destination: u32, request_id: u32, msg_type: u32, arg0: u32, arg1: u32, arg2: u32, arg3: u32, out_message: *Msg) i32 {
    var req: Msg = undefined;
    req.type = msg_type;
    req.source = source_endpoint;
    req.destination = destination;
    req.request_id = request_id;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.arg3 = arg3;

    const ctx_id = api.sched_current_pid.?();
    if (api.ipc_send.?(ctx_id, destination, &req) != IPC_OK) {
        return -1;
    }
    return ipcRecvMatching(api, source_endpoint, request_id, out_message);
}

pub fn svcRegister(comptime Msg: type, api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    var args: [4]u32 = undefined;
    var msg: Msg = undefined;
    packName16(name, &args);

    msg.type = c.SVC_IPC_REGISTER_REQ;
    msg.source = source_endpoint;
    msg.destination = proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];

    const ctx_id = api.sched_current_pid.?();
    if (api.ipc_send.?(ctx_id, proc_endpoint, &msg) != IPC_OK) {
        return -1;
    }
    if (ipcRecvMatching(api, source_endpoint, request_id, &msg) != 0) {
        return -1;
    }
    if (msg.type != c.SVC_IPC_REGISTER_RESP) {
        return -1;
    }
    return @bitCast(msg.arg0);
}

pub fn svcLookup(comptime Msg: type, api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id: u32) i32 {
    var args: [4]u32 = undefined;
    var msg: Msg = undefined;
    packName16(name, &args);

    msg.type = c.SVC_IPC_LOOKUP_REQ;
    msg.source = source_endpoint;
    msg.destination = proc_endpoint;
    msg.request_id = request_id;
    msg.arg0 = args[0];
    msg.arg1 = args[1];
    msg.arg2 = args[2];
    msg.arg3 = args[3];

    const ctx_id = api.sched_current_pid.?();
    if (api.ipc_send.?(ctx_id, proc_endpoint, &msg) != IPC_OK) {
        return -1;
    }
    if (ipcRecvMatching(api, source_endpoint, request_id, &msg) != 0) {
        return -1;
    }
    if (msg.type != c.SVC_IPC_LOOKUP_RESP or msg.arg0 == IPC_ENDPOINT_NONE) {
        return -1;
    }
    return @bitCast(msg.arg0);
}

pub fn svcLookupRetry(comptime Msg: type, api: anytype, proc_endpoint: u32, source_endpoint: u32, name: []const u8, request_id_base: u32, attempts: u32) i32 {
    var max_attempts = attempts;
    if (max_attempts == 0) {
        max_attempts = 1;
    }
    var i: u32 = 0;
    while (i < max_attempts) : (i += 1) {
        const ep = svcLookup(Msg, api, proc_endpoint, source_endpoint, name, request_id_base + i);
        if (ep >= 0) {
            return ep;
        }
        api.sched_yield.?();
    }
    return -1;
}
