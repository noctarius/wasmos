//! reconcile.zig — what a rescan must do to the partitions already published.
//!
//! A first probe has nothing to compare against: every entry the table holds
//! becomes a device. A RESCAN does, and the difference is the whole of this
//! module. The partitions of a disk are live devices — a filesystem may be
//! mounted on one, a transfer may be in flight to another — so a rescan is a
//! DIFF applied to them, never a teardown and rebuild. Re-registering a class
//! or re-announcing a descriptor that did not change would disturb a mount to
//! no purpose, and withdrawing one that is still described would break it.
//!
//! The identity a diff matches on is the table SLOT, because that is what the
//! published device's canonical id is built from (`<disk-id>p<slot>`) and
//! therefore what its class instance fingerprints. Two tables describing the
//! same slot describe the same device however much else about it changed; two
//! describing different slots never do, even if the windows coincide.
//!
//! Pure, and separate from the driver, so the cases that matter can be stated
//! as data: a slot that moved, a slot that vanished, a slot that appeared, a
//! table that lost everything. Reaching those on a real disk means writing a
//! partition table per case.

/// A partition's extent on its disk, which is all a diff compares. Both the
/// table entries coming in and the devices already published are reduced to
/// this, so neither side's richer type leaks into the comparison.
pub const Window = struct {
    /// Index within the partition table. Identity, not position: a GPT entry
    /// keeps its slot whatever order the entries are scanned in.
    slot: u32 = 0,
    lba_start: u64 = 0,
    lba_count: u64 = 0,
};

/// What a rescan must do with one entry of the NEW table.
pub const Verdict = union(enum) {
    /// No device for this slot exists; publish one.
    publish,
    /// A device exists and describes the same extent; leave it strictly alone.
    keep: usize,
    /// A device exists for this slot but the extent moved or resized. It keeps
    /// its identity — same slot, same canonical id, same class instance — and
    /// only what it describes is restated.
    update: usize,
};

/// Whether `w` describes an extent that exists on a disk of `sector_count`
/// sectors.
///
/// Refused rather than clamped: a table saying something impossible is a table
/// to distrust, and silently shrinking an entry would publish a device nobody
/// wrote. The same rule governs a first probe and a rescan, which is why it is
/// here rather than inline at either call site — a rescan that validated
/// differently would retire a partition the first probe had accepted, or
/// publish one it had refused, and the disagreement would read as a table
/// change.
pub fn fits(w: Window, sector_count: u64) bool {
    if (w.lba_count == 0) return false;
    if (w.lba_start >= sector_count) return false;
    return w.lba_count <= sector_count - w.lba_start;
}

/// Decide what to do with one entry of the new table, given every partition
/// currently published FOR THAT DISK.
///
/// `existing` must hold only the disk's own partitions. Slots are unique within
/// a table but not across disks, so a list spanning disks would match a slot
/// against another device's.
pub fn classify(existing: []const Window, entry: Window) Verdict {
    for (existing, 0..) |have, i| {
        if (have.slot != entry.slot) continue;
        if (have.lba_start == entry.lba_start and have.lba_count == entry.lba_count) {
            return .{ .keep = i };
        }
        return .{ .update = i };
    }
    return .publish;
}

/// Whether a published partition has no counterpart in the new table and must
/// therefore be retired.
///
/// `desired` must already have been filtered through `fits`: an entry the disk
/// cannot hold is not a description of anything, and treating one as a
/// counterpart would keep a device alive against a table that no longer
/// describes it.
pub fn orphaned(desired: []const Window, have: Window) bool {
    for (desired) |entry| {
        if (entry.slot == have.slot) return false;
    }
    return true;
}

/// Compact `entries` in place to those that fit on a disk of `sector_count`
/// sectors, returning how many remain.
///
/// In place because a driver has no allocator, and compacting rather than
/// masking because every later step walks the survivors and none of them should
/// have to carry a validity test the diff has already made.
pub fn retainFitting(entries: []Window, sector_count: u64) usize {
    var kept: usize = 0;
    for (entries) |entry| {
        if (!fits(entry, sector_count)) continue;
        entries[kept] = entry;
        kept += 1;
    }
    return kept;
}
