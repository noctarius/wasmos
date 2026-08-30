//! recognise.zig — identify the filesystem on a volume from a bounded prefix,
//! with no I/O.
//!
//! A recogniser answers "what format is this" without that format's driver being
//! present. It is not authoritative: the filesystem driver does the real parse
//! and is what refuses a volume a recogniser misread. What it produces is an
//! INPUT to choosing a mount — the rule engine matches `ATTR{fstype}` to decide
//! which driver to spawn — so it has to run before the decision it informs, which
//! is why it cannot live in the drivers themselves. They are spawned BY that
//! decision. See `docs/architecture/37-volume-manager.md` §3.
//!
//! Shape borrowed from Linux's `libblkid`: one file per format, mirroring its
//! `superblocks/`, with table detection beside it rather than in a second
//! library. Recognition and table detection read the same bytes, so they are one
//! pass over one prefix.
//!
//! Deliberately free of `@import("std")`, like `partition_table.zig`: this is
//! compiled into a freestanding wasm32 service. The cases live in
//! `tests/unit/test_fs_recognise.zig`, which imports this as a module and may use
//! std freely.
const pt = @import("partition_table.zig");
const fat = @import("recognise_fat.zig");
const wfs = @import("recognise_wfs.zig");
/// Public for the same reason `Scheme` is re-exported below: a file reached both
/// by path and as a named module belongs to two modules, which Zig refuses, so a
/// caller that imports this one by name takes the generated constants through it
/// rather than importing them again.
pub const abi = @import("wasmos_constants.zig");

/// Bytes every recogniser shares, read once from the volume's OWN LBA 0.
///
/// One figure, stated once, because it sizes the buffer the volume manager
/// acquires: a recogniser that wants more than this cannot have it without
/// changing the figure deliberately. 4 KiB covers everything the tree recognises
/// today with room to spare — the widest reach is the WFS superblock, which ends
/// at byte 2048.
///
/// "Its own LBA 0" is the part that bites. The partition manager rebases every
/// forwarded transfer onto the partition's window, so a descriptor's `lba_start`
/// says where the volume SITS and is never an address a client sends. A caller
/// that seeks to `lba_start` reads past the superblock it was looking for.
pub const PREFIX_BYTES: usize = 4096;

/// Longest label a recogniser reports, NUL included. FAT's is 11 bytes and WFS
/// carries none; the ceiling is set by what the `volume` descriptor reserves, so
/// a future format with a long name does not force a descriptor change.
pub const LABEL_MAX: usize = 64;

/// Bytes of volume UUID. The widest is WFS's 16; FAT's volume serial is 4 and
/// occupies the low bytes with the rest zero, so a comparison is always over the
/// full field and a short id can never alias a long one's prefix.
pub const UUID_MAX: usize = 16;

/// What a recogniser found. `fs_type` is always meaningful; the label and uuid
/// are present only when the format carries them, which is why each has its own
/// flag rather than being inferred from an empty value. A volume genuinely
/// labelled "" and a volume that cannot be labelled are different facts, and a
/// rule matching `ATTR{label}==""` must not match the second.
pub const Verdict = struct {
    fs_type: u32 = 0,
    has_label: bool = false,
    label: [LABEL_MAX]u8 = [_]u8{0} ** LABEL_MAX,
    has_uuid: bool = false,
    uuid: [UUID_MAX]u8 = [_]u8{0} ** UUID_MAX,
    /// Bytes of `uuid` the format actually defines. A reader comparing two
    /// volumes compares the whole field regardless; this is for rendering.
    uuid_len: u8 = 0,
};

/// One entry of the precedence table.
const Recogniser = struct {
    /// Returns true and fills `out` when the prefix is this format.
    probe: *const fn (prefix: []const u8, out: *Verdict) bool,
};

/// Precedence, most specific first. Order is a property of THIS table, not
/// something a recogniser asserts about itself, so it is readable in one place —
/// the arrangement HelenOS's `fstab[]` and libblkid's probe chain both use.
///
/// A permissive recogniser placed first claims volumes it should not. WFS leads
/// because it demands a 32-bit magic at a fixed offset; FAT follows because its
/// evidence is a plausibility argument about a BPB, and plausibility can be
/// satisfied by accident.
const CHAIN = [_]Recogniser{
    .{ .probe = wfs.probe },
    .{ .probe = fat.probe },
};

/// Re-exported so a caller naming a scheme does not import the table parser
/// separately. Zig gives a file imported both by path and as a named module two
/// distinct instances, and their enums are then two distinct TYPES that compare
/// unequal — so the test suite, which reaches this module by name, must get the
/// enum from here rather than from its own copy.
pub const Scheme = pt.Scheme;

/// Whether a device carries a partition table, and therefore holds partitions
/// rather than a filesystem.
///
/// Detection reuses `partition_table.zig` rather than re-reading the signatures
/// here: one parser, two callers. A second implementation of "is this a GPT"
/// could disagree with the one the partition manager acts on, and the disagreement
/// would show up as a volume that exists for a disk whose partitions also exist.
pub fn detectScheme(prefix: []const u8) pt.Scheme {
    return pt.detectScheme(prefix);
}

/// Identify the filesystem in `prefix`, which must be the volume's own first
/// bytes. Returns a verdict whose `fs_type` is `FS_TYPE_UNKNOWN` when nothing
/// matched.
///
/// "Nothing matched" is a real answer, not a failure: an unrecognised format
/// reads exactly like an empty volume from here, and both are things a rule may
/// legitimately select with `ATTR{fstype}=="unknown"`.
pub fn recognise(prefix: []const u8) Verdict {
    const out = Verdict{ .fs_type = @intCast(abi.FS_TYPE_UNKNOWN) };
    for (CHAIN) |r| {
        var candidate = Verdict{ .fs_type = @intCast(abi.FS_TYPE_UNKNOWN) };
        if (r.probe(prefix, &candidate)) return candidate;
    }
    return out;
}

/// Copy `src` into a verdict's label field, dropping trailing spaces and NULs.
///
/// Shared rather than duplicated per recogniser because the padding convention is
/// the same wherever a fixed-width on-disk name appears: FAT pads its 11 bytes
/// with spaces, so "QEMU VVFAT " and "QEMU VVFAT" are the same name and only one
/// of them is what a rule author would type.
pub fn setLabel(out: *Verdict, src: []const u8) void {
    var end: usize = src.len;
    while (end > 0 and (src[end - 1] == ' ' or src[end - 1] == 0)) : (end -= 1) {}
    if (end == 0 or end >= LABEL_MAX) {
        if (end == 0) {
            // An empty label is a label: the format carries the field and it is
            // blank, which is not the same as carrying none (WFS). The buffer is
            // cleared on this path too, so a Verdict written twice cannot leave
            // an earlier label standing behind has_label.
            for (out.label[0..]) |*b| b.* = 0;
            out.has_label = true;
            return;
        }
        end = LABEL_MAX - 1;
    }
    for (out.label[0..]) |*b| b.* = 0;
    var i: usize = 0;
    while (i < end) : (i += 1) out.label[i] = src[i];
    out.has_label = true;
}

/// Copy `src` into a verdict's uuid field, left-aligned and zero-padded.
pub fn setUuid(out: *Verdict, src: []const u8) void {
    for (out.uuid[0..]) |*b| b.* = 0;
    const n = if (src.len < UUID_MAX) src.len else UUID_MAX;
    var i: usize = 0;
    while (i < n) : (i += 1) out.uuid[i] = src[i];
    out.has_uuid = true;
    out.uuid_len = @intCast(n);
}
