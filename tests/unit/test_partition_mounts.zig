//! Unit tests for reading fs-manager's mount listing
//! (src/drivers/partition_manager/mounts.zig).
//!
//! This is the gate that stands between a rescan and a live filesystem. It
//! replaced the volume manager's CLAIM, which a CI run proved unsound: a
//! partition whose volume the volume manager had failed to probe carried no
//! volume record at all, `mount` succeeded anyway because mounting needs none,
//! and "no volume" was read as "nothing holds it".
//!
//! The case these exist for is a PREFIX collision. Partition ids are built by
//! appending a slot number, so `block:ata:0p1` is a prefix of `block:ata:0p11`,
//! and the two failure directions are opposite and both silent: a matcher that
//! stopped short would refuse to retire a partition because a DIFFERENT one is
//! mounted, and one that matched a prefix of the id would retire a mounted
//! partition because a shorter id appears somewhere in the text.
const std = @import("std");
const mnt = @import("mounts");

const LISTING =
    "mounts:\n" ++
    "/boot -> fat on block:ata:0p1\n" ++
    "/user -> fat on block:ata:1p1\n" ++
    "/tmp -> tmpfs\n" ++
    "/wfs -> wfs on block:ata:2\n";

test "a device that backs a mount is found" {
    try std.testing.expect(mnt.mountedOn(LISTING, "block:ata:0p1"));
    try std.testing.expect(mnt.mountedOn(LISTING, "block:ata:1p1"));
    try std.testing.expect(mnt.mountedOn(LISTING, "block:ata:2"));
}

test "a device that backs nothing is not found" {
    try std.testing.expect(!mnt.mountedOn(LISTING, "block:ata:3"));
    try std.testing.expect(!mnt.mountedOn(LISTING, "block:virtio-blk:40p1"));
}

test "an id that is a PREFIX of a mounted one does not match" {
    // The whole reason this is matched to the end of the line. `block:ata:0p1`
    // is mounted; `block:ata:0p11` is a different partition and is not.
    const listing = "mounts:\n/boot -> fat on block:ata:0p11\n";
    try std.testing.expect(mnt.mountedOn(listing, "block:ata:0p11"));
    try std.testing.expect(!mnt.mountedOn(listing, "block:ata:0p1"));
    try std.testing.expect(!mnt.mountedOn(listing, "block:ata:0"));
}

test "an id that EXTENDS a mounted one does not match" {
    try std.testing.expect(!mnt.mountedOn(LISTING, "block:ata:0p1x"));
    try std.testing.expect(!mnt.mountedOn(LISTING, "block:ata:2p1"));
}

test "a mount with no device cannot be matched by its filesystem name" {
    // "/tmp -> tmpfs" has no " on " at all, so nothing about it is a device.
    try std.testing.expect(!mnt.mountedOn(LISTING, "tmpfs"));
}

test "the last line matches even without a trailing newline" {
    // fs-manager truncates when its buffer fills, which can cut the newline.
    const listing = "mounts:\n/wfs -> wfs on block:ata:2";
    try std.testing.expect(mnt.mountedOn(listing, "block:ata:2"));
}

test "a carriage return ends the token too" {
    const listing = "mounts:\r\n/wfs -> wfs on block:ata:2\r\n";
    try std.testing.expect(mnt.mountedOn(listing, "block:ata:2"));
}

test "an empty id never matches" {
    // It names no device. Reporting it as mounted would refuse every change;
    // the caller rejects an unusable id before reaching here, and this is the
    // second line of that defence.
    try std.testing.expect(!mnt.mountedOn(LISTING, ""));
    // The listing that makes the guard load-bearing rather than decorative: a
    // device field present but EMPTY, which is what a truncation mid-token
    // leaves behind. Without the guard the empty id matches the empty token and
    // every partition reads as mounted.
    try std.testing.expect(!mnt.mountedOn("mounts:\n/boot -> fat on \n", ""));
    try std.testing.expect(!mnt.mountedOn("mounts:\n/boot -> fat on ", ""));
}

test "an empty listing matches nothing" {
    try std.testing.expect(!mnt.mountedOn("", "block:ata:0p1"));
    try std.testing.expect(!mnt.mountedOn("mounts:\n", "block:ata:0p1"));
}

test "a truncated marker at the very end does not run off the buffer" {
    // fs-manager stops appending mid-table when the next line would not fit.
    try std.testing.expect(!mnt.mountedOn("mounts:\n/boot -> fat on", "block:ata:0p1"));
    try std.testing.expect(!mnt.mountedOn("mounts:\n/boot -> fat o", "block:ata:0p1"));
}
