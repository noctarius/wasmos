//! Unit tests for the table of backends a block client has lent its buffers to
//! (src/drivers/block/partition/backends.zig).
const std = @import("std");
const be = @import("backends");

test "a backend is lent to once however many devices it serves" {
    var t = be.Table(4){};
    try std.testing.expectEqual(be.Note.lend, t.note(30));
    try std.testing.expectEqual(be.Note.known, t.note(30));
    try std.testing.expectEqual(be.Note.lend, t.note(31));
    try std.testing.expectEqual(be.Note.known, t.note(30));
}

test "a full table refuses a new backend and keeps the ones it holds" {
    var t = be.Table(2){};
    _ = t.note(30);
    _ = t.note(31);
    try std.testing.expectEqual(be.Note.full, t.note(32));
    try std.testing.expectEqual(be.Note.known, t.note(31));
}

// Regression: 2026-11-21-partmgr-loses-a-restarted-disk -- the partition
// manager and the volume manager recorded every backend endpoint they had ever
// lent to and never dropped one. A supervised disk driver comes back on a new
// endpoint, so each restart took a slot, and once the table was full the next
// instance was never lent the request buffer and every transfer to it failed.
test "a backend that ended frees its slot for the instance that replaces it" {
    var t = be.Table(2){};
    _ = t.note(30);
    _ = t.note(31);
    var ep: i32 = 31;
    var restart: i32 = 0;
    while (restart < 8) : (restart += 1) {
        t.forget(ep);
        ep += 10;
        try std.testing.expectEqual(be.Note.lend, t.note(ep));
    }
    try std.testing.expectEqual(be.Note.known, t.note(30));
    try std.testing.expectEqual(@as(usize, 2), t.count);
}

test "forgetting an endpoint twice, or one never lent to, changes nothing" {
    var t = be.Table(4){};
    _ = t.note(30);
    _ = t.note(31);
    t.forget(30);
    t.forget(30);
    t.forget(99);
    try std.testing.expectEqual(@as(usize, 1), t.count);
    try std.testing.expectEqual(be.Note.known, t.note(31));
    try std.testing.expectEqual(be.Note.lend, t.note(30));
}
