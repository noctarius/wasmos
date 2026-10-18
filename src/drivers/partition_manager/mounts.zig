//! mounts.zig — reading fs-manager's mount listing.
//!
//! The listing is the authority on what is MOUNTED, and the partition manager
//! consults it before changing a partition that is already published: a
//! retirement makes a device unfindable while a filesystem may still be writing
//! to it, and a window change redirects those writes. Nothing else in the
//! system can answer the question -- the device manager's `active_service` is
//! never set, and the volume manager's CLAIM only covers devices it managed to
//! recognise a filesystem on.
//!
//! Format, from `fsmgr_emit_mounts`:
//!
//!     mounts:
//!     /boot -> fat on block:ata:0p1
//!     /tmp -> tmpfs
//!
//! The ` on <canonical id>` suffix is present only when the filesystem has a
//! backing device; a memory-backed one prints nothing there rather than a
//! placeholder.
//!
//! Pure and separate from the driver so the case that matters can be stated as
//! data. That case is an id that is a PREFIX of another: partition ids are
//! built by appending a slot number, so `block:ata:0p1` is a prefix of
//! `block:ata:0p11`, and a matcher that stopped at the shorter one would refuse
//! to retire a partition because a different partition was mounted -- or, worse
//! the other way round, retire one because the match ran off the end.

/// Whether `listing` names `id` as some mount's backing device.
///
/// Matched on the WHOLE token after " on ", to the end of the line, which is
/// what makes a prefix collision impossible. An empty id never matches: it
/// names no device, and treating it as found would refuse every change.
pub fn mountedOn(listing: []const u8, id: []const u8) bool {
    if (id.len == 0) return false;
    const marker = " on ";
    var i: usize = 0;
    while (i + marker.len <= listing.len) : (i += 1) {
        if (!eql(listing[i .. i + marker.len], marker)) continue;
        const start = i + marker.len;
        var end = start;
        while (end < listing.len and listing[end] != '\n' and listing[end] != '\r') end += 1;
        if (eql(listing[start..end], id)) return true;
    }
    return false;
}

fn eql(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |x, y| {
        if (x != y) return false;
    }
    return true;
}
