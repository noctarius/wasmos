//! `ipc.call` in the Zig binding (src/libc/zig/wasmos.zig), run on the build
//! host against a fake kernel mailbox exported here in place of the four IPC
//! host calls it reaches.
//!
//! Regression: 2026-11-26-lang-ipc-call-match -- `call` returned the first
//! message that arrived on the context's managed reply endpoint without
//! checking its request id or source, so a stale reply, or a message with the
//! right id from another endpoint, was handed back as the answer to the
//! request just sent. The C helper (`wasmos_ipc_call`) accepts only a message
//! carrying the request's id AND sent from the endpoint the request went to.
//!
//! The fake server on SERVER answers every request on the caller's reply
//! endpoint with `arg0 + 1`. `arg1` selects what it queues AHEAD of that
//! answer: nothing, a stale reply (another request id), or an impostor (this
//! request id, another source). Every case also checks that `call` consumed the
//! queue, so a message it skipped is not left for the next call.
const std = @import("std");
const wasmos = @import("wasmos");

const SERVER: i32 = 5;
const IMPOSTOR: i32 = 6;
const REPLY_ENDPOINT: i32 = 90;
const SCENARIO_CLEAN: i32 = 0;
const SCENARIO_STALE_FIRST: i32 = 1;
const SCENARIO_IMPOSTOR_FIRST: i32 = 2;

/// Field indices of `ipc_last_field` (abi/hostcalls.yaml).
const F_TYPE = 0;
const F_REQUEST_ID = 1;
const F_ARG0 = 2;
const F_SOURCE = 4;
const F_DESTINATION = 5;

const Message = [8]i32;

var queue: [8]Message = undefined;
var queue_head: usize = 0;
var queue_tail: usize = 0;
var last: Message = [_]i32{0} ** 8;

fn push(msg_type: i32, request_id: i32, source: i32, arg0: i32) void {
    var m: Message = [_]i32{0} ** 8;
    m[F_TYPE] = msg_type;
    m[F_REQUEST_ID] = request_id;
    m[F_SOURCE] = source;
    m[F_DESTINATION] = REPLY_ENDPOINT;
    m[F_ARG0] = arg0;
    queue[queue_tail % queue.len] = m;
    queue_tail += 1;
}

fn reset() void {
    queue_head = 0;
    queue_tail = 0;
    last = [_]i32{0} ** 8;
}

export fn ipc_create_endpoint() callconv(.c) i32 {
    return REPLY_ENDPOINT;
}

export fn ipc_send(
    destination: i32,
    source: i32,
    msg_type: i32,
    request_id: i32,
    arg0: i32,
    arg1: i32,
    arg2: i32,
    arg3: i32,
) callconv(.c) i32 {
    _ = arg2;
    _ = arg3;
    std.debug.assert(destination == SERVER);
    std.debug.assert(source == REPLY_ENDPOINT);
    switch (arg1) {
        SCENARIO_STALE_FIRST => push(msg_type, request_id + 1000, SERVER, -1),
        SCENARIO_IMPOSTOR_FIRST => push(msg_type, request_id, IMPOSTOR, -1),
        else => {},
    }
    push(msg_type, request_id, SERVER, arg0 + 1);
    return 0;
}

/// Pops the next message into the "last received" slot; an empty queue is a
/// receive error rather than a block, so a `call` that keeps waiting fails the
/// case instead of hanging it.
export fn ipc_select_one(endpoint: i32) callconv(.c) i32 {
    std.debug.assert(endpoint == REPLY_ENDPOINT);
    if (queue_head == queue_tail) return -1;
    last = queue[queue_head % queue.len];
    queue_head += 1;
    return 0;
}

export fn ipc_last_field(field: i32) callconv(.c) i32 {
    return last[@intCast(field)];
}

fn call(arg0: i32, scenario: i32) !wasmos.ipc.Reply {
    return wasmos.ipc.call(SERVER, 0x10, arg0, scenario, 0, 0);
}

test "a clean reply is returned" {
    reset();
    const reply = try call(41, SCENARIO_CLEAN);
    try std.testing.expectEqual(@as(i32, 42), reply.arg0);
    try std.testing.expectEqual(SERVER, reply.source);
    try std.testing.expectEqual(queue_tail, queue_head);
}

test "a stale reply queued ahead is skipped" {
    reset();
    const reply = try call(10, SCENARIO_STALE_FIRST);
    try std.testing.expectEqual(@as(i32, 11), reply.arg0);
    try std.testing.expectEqual(queue_tail, queue_head);
}

test "a reply from another endpoint is skipped" {
    reset();
    const reply = try call(20, SCENARIO_IMPOSTOR_FIRST);
    try std.testing.expectEqual(@as(i32, 21), reply.arg0);
    try std.testing.expectEqual(SERVER, reply.source);
    try std.testing.expectEqual(queue_tail, queue_head);
}

test "consecutive calls each get their own reply" {
    reset();
    const first = try call(1, SCENARIO_STALE_FIRST);
    const second = try call(2, SCENARIO_IMPOSTOR_FIRST);
    try std.testing.expectEqual(@as(i32, 2), first.arg0);
    try std.testing.expectEqual(@as(i32, 3), second.arg0);
    try std.testing.expect(first.request_id != second.request_id);
    try std.testing.expectEqual(queue_tail, queue_head);
}
