//! Unit tests for the rescan diff
//! (src/drivers/partition_manager/reconcile.zig).
//!
//! Every case here is a partition table being rewritten under live devices,
//! which is the situation `fdisk` creates and the one that is impractical to
//! reach any other way: producing it on a real disk means writing a table,
//! rescanning, and then reading back which devices the driver chose to leave
//! alone -- a distinction that is invisible from outside precisely because
//! leaving a device alone looks like doing nothing.
//!
//! The distinction being asserted throughout is KEEP versus UPDATE. Both end
//! with a device for that slot still published, so a test that only counted
//! devices would pass against a driver that re-registered every partition on
//! every rescan -- which would tear the class registration out from under any
//! filesystem mounted on one.
const std = @import("std");
const rc = @import("reconcile");

const W = rc.Window;

// A disk big enough that nothing below is rejected for geometry; the cases that
// test geometry state their own.
const DISK: u64 = 1000;

// --- fits --------------------------------------------------------------------

test "an extent inside the disk fits" {
    try std.testing.expect(rc.fits(.{ .slot = 0, .lba_start = 0, .lba_count = DISK }, DISK));
    try std.testing.expect(rc.fits(.{ .slot = 1, .lba_start = 999, .lba_count = 1 }, DISK));
}

test "an empty extent describes nothing" {
    try std.testing.expect(!rc.fits(.{ .slot = 0, .lba_start = 10, .lba_count = 0 }, DISK));
}

test "an extent starting at or past the end does not fit" {
    try std.testing.expect(!rc.fits(.{ .slot = 0, .lba_start = DISK, .lba_count = 1 }, DISK));
    try std.testing.expect(!rc.fits(.{ .slot = 0, .lba_start = DISK + 1, .lba_count = 1 }, DISK));
}

test "an extent running past the end does not fit" {
    try std.testing.expect(!rc.fits(.{ .slot = 0, .lba_start = 990, .lba_count = 11 }, DISK));
}

test "a length that would overflow a naive end calculation does not fit" {
    // start + count wraps in 64 bits. Computed as count <= sector_count - start
    // rather than start + count <= sector_count for exactly this entry, which a
    // table anyone can write can hold.
    const huge = W{ .slot = 0, .lba_start = 512, .lba_count = 0xFFFF_FFFF_FFFF_FF00 };
    try std.testing.expect(!rc.fits(huge, DISK));
}

// --- classify ----------------------------------------------------------------

test "an entry for a slot with no device is published" {
    const have = [_]W{.{ .slot = 1, .lba_start = 100, .lba_count = 100 }};
    const verdict = rc.classify(have[0..], .{ .slot = 2, .lba_start = 300, .lba_count = 100 });
    try std.testing.expect(verdict == .publish);
}

test "an unchanged entry is kept, not republished" {
    const have = [_]W{
        .{ .slot = 1, .lba_start = 100, .lba_count = 100 },
        .{ .slot = 2, .lba_start = 300, .lba_count = 100 },
    };
    const verdict = rc.classify(have[0..], .{ .slot = 2, .lba_start = 300, .lba_count = 100 });
    try std.testing.expectEqual(@as(usize, 1), verdict.keep);
}

test "a slot whose start moved is an update" {
    const have = [_]W{.{ .slot = 1, .lba_start = 100, .lba_count = 100 }};
    const verdict = rc.classify(have[0..], .{ .slot = 1, .lba_start = 200, .lba_count = 100 });
    try std.testing.expectEqual(@as(usize, 0), verdict.update);
}

test "a slot that only grew is an update" {
    // The identity is the slot, so a resize is the same device describing more
    // of the disk -- not a different one. A diff that matched on the extent
    // would retire this and publish a replacement, and the replacement would
    // carry a different canonical id for the same partition.
    const have = [_]W{.{ .slot = 1, .lba_start = 100, .lba_count = 100 }};
    const verdict = rc.classify(have[0..], .{ .slot = 1, .lba_start = 100, .lba_count = 400 });
    try std.testing.expectEqual(@as(usize, 0), verdict.update);
}

test "two slots that swapped extents are both updates, not keeps" {
    // Every extent in the new table is also in the old one, so a diff comparing
    // extents without slots would call this no change at all.
    const have = [_]W{
        .{ .slot = 1, .lba_start = 100, .lba_count = 100 },
        .{ .slot = 2, .lba_start = 300, .lba_count = 100 },
    };
    const a = rc.classify(have[0..], .{ .slot = 1, .lba_start = 300, .lba_count = 100 });
    const b = rc.classify(have[0..], .{ .slot = 2, .lba_start = 100, .lba_count = 100 });
    try std.testing.expectEqual(@as(usize, 0), a.update);
    try std.testing.expectEqual(@as(usize, 1), b.update);
}

test "an entry matching nothing because the disk has no devices is published" {
    const none = [_]W{};
    try std.testing.expect(rc.classify(none[0..], .{ .slot = 0, .lba_start = 1, .lba_count = 1 }) == .publish);
}

// --- orphaned ----------------------------------------------------------------

test "a device whose slot survived is not orphaned, however much it moved" {
    const want = [_]W{.{ .slot = 1, .lba_start = 900, .lba_count = 50 }};
    const have = W{ .slot = 1, .lba_start = 100, .lba_count = 100 };
    try std.testing.expect(!rc.orphaned(want[0..], have));
}

test "a device whose slot is gone from the table is orphaned" {
    const want = [_]W{.{ .slot = 2, .lba_start = 300, .lba_count = 100 }};
    const have = W{ .slot = 1, .lba_start = 100, .lba_count = 100 };
    try std.testing.expect(rc.orphaned(want[0..], have));
}

test "every device is orphaned when the table lost its last entry" {
    // Wiping a disk's table is the case the retirement path exists for, and the
    // one where getting it wrong leaves devices nothing describes.
    const want = [_]W{};
    try std.testing.expect(rc.orphaned(want[0..], .{ .slot = 0, .lba_start = 1, .lba_count = 1 }));
    try std.testing.expect(rc.orphaned(want[0..], .{ .slot = 3, .lba_start = 9, .lba_count = 9 }));
}

test "an extent that coincides with a surviving one does not save a dropped slot" {
    // Slot 1 is gone; slot 2 happens to occupy exactly where slot 1 was. The
    // device for slot 1 must still be retired.
    const want = [_]W{.{ .slot = 2, .lba_start = 100, .lba_count = 100 }};
    const have = W{ .slot = 1, .lba_start = 100, .lba_count = 100 };
    try std.testing.expect(rc.orphaned(want[0..], have));
}

// --- retainFitting -----------------------------------------------------------

test "entries that fit are compacted to the front in order" {
    var entries = [_]W{
        .{ .slot = 0, .lba_start = 0, .lba_count = 100 },
        .{ .slot = 1, .lba_start = 900, .lba_count = 500 }, // runs off the end
        .{ .slot = 2, .lba_start = 200, .lba_count = 100 },
    };
    const kept = rc.retainFitting(entries[0..], DISK);
    try std.testing.expectEqual(@as(usize, 2), kept);
    try std.testing.expectEqual(@as(u32, 0), entries[0].slot);
    try std.testing.expectEqual(@as(u32, 2), entries[1].slot);
}

test "a table of nothing usable compacts to nothing" {
    var entries = [_]W{
        .{ .slot = 0, .lba_start = 0, .lba_count = 0 },
        .{ .slot = 1, .lba_start = DISK, .lba_count = 1 },
    };
    try std.testing.expectEqual(@as(usize, 0), rc.retainFitting(entries[0..], DISK));
}

test "an unfit entry does not shadow a later slot of the same number" {
    // The compaction must not leave the dropped entry readable within the kept
    // range, because `orphaned` walks that range and a stale slot there would
    // rescue a device the table no longer describes.
    var entries = [_]W{
        .{ .slot = 7, .lba_start = DISK, .lba_count = 4 },
        .{ .slot = 8, .lba_start = 0, .lba_count = 4 },
    };
    const kept = rc.retainFitting(entries[0..], DISK);
    try std.testing.expectEqual(@as(usize, 1), kept);
    try std.testing.expect(rc.orphaned(entries[0..kept], .{ .slot = 7, .lba_start = 0, .lba_count = 1 }));
}
