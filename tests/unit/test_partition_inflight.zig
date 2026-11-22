//! Unit tests for the partition manager's in-flight transfer table
//! (src/drivers/partition_manager/inflight.zig).
//!
//! The partition manager forwards a client's transfer to the disk beneath the
//! partition by staging a rewritten request descriptor in a buffer it owns and
//! lends to the disk. The disk reads that descriptor when it reaches the
//! message, which may be after the partition manager has forwarded more
//! transfers. These cases stage descriptors into a byte array laid out exactly
//! as the lent buffer is, and read one back the way the disk would.
const std = @import("std");
const inf = @import("inflight");

/// Stands in for sizeof(wasmos_block_request_t); the table depends only on it
/// being the stride.
const DESC: u32 = 48;

fn stage(buf: []u8, e: *const inf.Inflight, fill: u8) void {
    @memset(buf[e.req_offset .. e.req_offset + DESC], fill);
}

fn allEqual(bytes: []const u8, value: u8) bool {
    for (bytes) |b| {
        if (b != value) return false;
    }
    return true;
}

// Regression: 2026-11-16-partmgr-shared-request-slot -- every forwarded
// transfer staged its descriptor at offset 0 of one buffer, so a second
// transfer forwarded before the disk had read the first rewrote it: the first
// request was served with the second's disk, LBA and destination borrow. With
// /boot and /user both partitions, /boot's FAT read /user's sectors into its
// one-sector cache under its root-directory LBA, every later lookup hit that
// poisoned entry, and the boot panicked init_no_sysinit after 512 NOT_FOUNDs.
test "staging a second transfer leaves the first one's descriptor intact" {
    var table = inf.Table.init(DESC);
    var buf = [_]u8{0} ** inf.Table.bufferBytes(DESC);

    const a = table.claim() orelse return error.NoEntry;
    stage(&buf, a, 0xAA);
    a.down_request_id = 1;
    a.in_use = true;

    const b = table.claim() orelse return error.NoEntry;
    try std.testing.expect(a != b);
    stage(&buf, b, 0xBB);
    b.down_request_id = 2;
    b.in_use = true;

    // The disk now reaches the FIRST message and reads its descriptor.
    try std.testing.expect(allEqual(buf[a.req_offset .. a.req_offset + DESC], 0xAA));
    try std.testing.expect(allEqual(buf[b.req_offset .. b.req_offset + DESC], 0xBB));
}

test "every live transfer owns a disjoint slot inside the buffer" {
    var table = inf.Table.init(DESC);
    var buf = [_]u8{0} ** inf.Table.bufferBytes(DESC);

    var live: [inf.MAX]*inf.Inflight = undefined;
    for (&live, 0..) |*slot, i| {
        const e = table.claim() orelse return error.NoEntry;
        try std.testing.expect(e.req_offset + DESC <= buf.len);
        stage(&buf, e, @intCast(i + 1));
        e.down_request_id = @intCast(i + 1);
        e.in_use = true;
        slot.* = e;
    }
    try std.testing.expect(table.claim() == null);

    for (live, 0..) |e, i| {
        try std.testing.expect(allEqual(buf[e.req_offset .. e.req_offset + DESC], @intCast(i + 1)));
    }
}

test "an answered transfer's entry is reused with the same slot" {
    var table = inf.Table.init(DESC);

    var live: [inf.MAX]*inf.Inflight = undefined;
    for (&live, 0..) |*slot, i| {
        const e = table.claim() orelse return error.NoEntry;
        e.down_request_id = @intCast(100 + i);
        e.in_use = true;
        slot.* = e;
    }

    const answered = table.findByDownId(103) orelse return error.NotFound;
    try std.testing.expect(answered == live[3]);
    const offset = answered.req_offset;
    answered.in_use = false;

    const next = table.claim() orelse return error.NoEntry;
    try std.testing.expect(next == answered);
    try std.testing.expectEqual(offset, next.req_offset);
    try std.testing.expect(table.findByDownId(103) == null);
}

// Regression: 2026-11-22-block-transfer-lost-across-a-driver-restart -- a
// transfer forwarded to a disk driver that ended was failed with
// WASMOS_ERR_BLOCK_DEV_GONE although its successor served the same disk a
// moment later. A held entry keeps its descriptor slot, is found again per
// disk, and is never matched by a reply from the driver that ended.
test "a held transfer keeps its slot and answers no late reply" {
    var table = inf.Table.init(DESC);
    var buf = [_]u8{0} ** inf.Table.bufferBytes(DESC);

    const e = table.claim() orelse return error.NoEntry;
    stage(&buf, e, 0xCC);
    e.down_request_id = 41;
    e.down_borrow_id = 9;
    e.server = 30;
    e.disk = 1;
    e.in_use = true;
    const offset = e.req_offset;

    try std.testing.expect(!table.anyHeld());
    inf.Table.hold(e);
    try std.testing.expect(table.anyHeld());
    try std.testing.expect(table.findByDownId(41) == null);
    try std.testing.expect(table.findByDownId(0) == null);
    try std.testing.expectEqual(@as(i32, -1), e.down_borrow_id);
    try std.testing.expectEqual(offset, e.req_offset);
    try std.testing.expect(allEqual(buf[e.req_offset .. e.req_offset + DESC], 0xCC));

    try std.testing.expect(table.nextHeld(0) == null);
    try std.testing.expect(table.nextHeld(1) == e);

    // Resent: on the wire again under a new id, and found by it.
    e.held = false;
    e.down_request_id = 42;
    try std.testing.expect(table.findByDownId(42) == e);
    try std.testing.expect(table.nextHeld(1) == null);
    try std.testing.expect(!table.anyHeld());
}

test "held transfers are found per disk until each is resent or freed" {
    var table = inf.Table.init(DESC);

    var live: [3]*inf.Inflight = undefined;
    for (&live, 0..) |*slot, i| {
        const e = table.claim() orelse return error.NoEntry;
        e.down_request_id = @intCast(10 + i);
        e.disk = if (i == 1) 2 else 3;
        e.in_use = true;
        slot.* = e;
    }
    for (live) |e| inf.Table.hold(e);

    var resent: usize = 0;
    while (table.nextHeld(3)) |e| {
        e.held = false;
        e.down_request_id = @intCast(100 + resent);
        resent += 1;
    }
    try std.testing.expectEqual(@as(usize, 2), resent);
    try std.testing.expect(table.nextHeld(2) == live[1]);
    live[1].in_use = false;
    try std.testing.expect(table.nextHeld(2) == null);
    try std.testing.expect(!table.anyHeld());
}
