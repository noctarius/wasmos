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
//!
//! Regression: 2026-11-26-lang-ipc-call-hangup -- `call` never registered
//! `ipc_peer_await` before sending and never recognised WASMOS_IPC_HANGUP, so a
//! server that died before answering left the caller blocked forever: the
//! kernel owes a hangup only to an endpoint the server had sent to, or to one
//! that registered an await (docs/architecture/09-process-and-ipc.md, "Peer
//! Hangup"). The fake's `ipc_peer_await` records the registration and refuses
//! DEAD, an endpoint with no owner, with WASMOS_NOENT; HANGUP_INSTEAD has the
//! server queue a hangup naming itself in place of an answer.
const std = @import("std");
const wasmos = @import("wasmos");

const SERVER: i32 = 5;
const IMPOSTOR: i32 = 6;
const REPLY_ENDPOINT: i32 = 90;
const SCENARIO_CLEAN: i32 = 0;
const SCENARIO_STALE_FIRST: i32 = 1;
const SCENARIO_IMPOSTOR_FIRST: i32 = 2;
const SCENARIO_HANGUP_INSTEAD: i32 = 3;
const DEAD: i32 = 7;
const WASMOS_NOENT: i32 = -4;
const WASMOS_IPC_HANGUP: i32 = 0xFF03;

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
/// The endpoint the last ipc_peer_await on the reply endpoint named.
var awaited: i32 = -1;
var sends: u32 = 0;
var sends_unawaited: u32 = 0;

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
    awaited = -1;
    sends = 0;
    sends_unawaited = 0;
}

export fn ipc_peer_await(waiter: i32, endpoint: i32) callconv(.c) i32 {
    std.debug.assert(waiter == REPLY_ENDPOINT);
    if (endpoint == DEAD) return WASMOS_NOENT;
    awaited = endpoint;
    return 0;
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
    std.debug.assert(source == REPLY_ENDPOINT);
    sends += 1;
    if (awaited != destination) sends_unawaited += 1;
    if (destination != SERVER) return 0;
    if (arg1 == SCENARIO_HANGUP_INSTEAD) {
        push(WASMOS_IPC_HANGUP, 0, -1, SERVER);
        return 0;
    }
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

test "the server is awaited before the request goes out" {
    reset();
    _ = try call(1, SCENARIO_CLEAN);
    try std.testing.expectEqual(@as(u32, 1), sends);
    try std.testing.expectEqual(@as(u32, 0), sends_unawaited);
}

test "a hangup naming the server ends the call" {
    reset();
    try std.testing.expectError(error.PeerGone, call(1, SCENARIO_HANGUP_INSTEAD));
    try std.testing.expectEqual(queue_tail, queue_head);
}

test "a server with no owner is refused before sending" {
    reset();
    try std.testing.expectError(error.PeerGone, wasmos.ipc.call(DEAD, 0x10, 1, SCENARIO_CLEAN, 0, 0));
    try std.testing.expectEqual(@as(u32, 0), sends);
}
