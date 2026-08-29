//! Unit tests for the filesystem recognisers
//! (src/services/volume_manager/recognise*.zig) and for the table detection
//! they depend on (partition_table.detectScheme).
//!
//! The recognisers do no I/O, so every case builds a prefix in memory and feeds
//! it in. That is the whole reason they take byte slices: a superblock is
//! untrusted input from a medium anyone can write, and the cases that matter are
//! the ones that ALMOST match — an MBR that carries a boot signature, a FAT
//! volume with an impossible cluster size, a WFS superblock from a future
//! version. Producing those on a real device is impractical and here they are
//! three lines.
//!
//! Two properties get most of the attention, because they are what a recogniser
//! gets wrong in a way nothing downstream catches:
//!
//!  - **A table is not a filesystem.** An MBR and a GPT protective MBR both
//!    carry 0x55AA at offset 510, which is also FAT's signature. A probe that
//!    stops there reports a partitioned disk as a FAT volume, and the rule engine
//!    mounts the partition table.
//!  - **Precedence is a property of the chain**, not of a recogniser. A
//!    permissive probe placed first claims volumes a specific one should have.
//!
//! What is deliberately NOT tested here: that a real volume recognises. That is
//! the integration test's job — a hand-built image asserting against itself only
//! proves the builder and the probe share a misunderstanding. The one exception
//! is the vvfat case below, which is a byte-for-byte capture of what QEMU
//! actually presents rather than something this file invented.
const std = @import("std");
const r = @import("recognise");
const fx = @import("fixtures_disk_images");

const abi = r.abi;

const SECTOR = 512;
const PREFIX = r.PREFIX_BYTES;

// --- builders ----------------------------------------------------------------

fn put16(buf: []u8, off: usize, v: u16) void {
    buf[off] = @intCast(v & 0xFF);
    buf[off + 1] = @intCast((v >> 8) & 0xFF);
}

fn put32(buf: []u8, off: usize, v: u32) void {
    put16(buf, off, @intCast(v & 0xFFFF));
    put16(buf, off + 2, @intCast((v >> 16) & 0xFFFF));
}

/// A minimal but valid FAT16 boot sector: jump, plausible BPB, label, serial.
fn buildFat16(prefix: *[PREFIX]u8, label: []const u8) void {
    @memset(prefix, 0);
    prefix[0] = 0xEB; // short jump ...
    prefix[2] = 0x90; // ... plus NOP
    put16(prefix, 11, 512); // bytes per sector
    prefix[13] = 4; // sectors per cluster
    put16(prefix, 14, 4); // reserved sectors
    prefix[16] = 2; // FATs
    put16(prefix, 17, 512); // root entries — non-zero, so FAT16 not FAT32
    prefix[21] = 0xF8; // media descriptor
    put16(prefix, 22, 250); // fat_size_16 — non-zero, so FAT16 not FAT32
    put32(prefix, 39, 0xFABE1AFD); // volume serial
    @memset(prefix[43..54], ' ');
    @memcpy(prefix[43 .. 43 + label.len], label);
    prefix[510] = 0x55;
    prefix[511] = 0xAA;
}

/// The same, in FAT32 shape: the 16-bit FAT size and root entry count are zero,
/// which is what moves the label and serial 28 bytes later.
fn buildFat32(prefix: *[PREFIX]u8, label: []const u8) void {
    buildFat16(prefix, "");
    put16(prefix, 17, 0); // root entries
    put16(prefix, 22, 0); // fat_size_16
    put32(prefix, 67, 0x12345678); // volume serial, FAT32 offset
    @memset(prefix[71..82], ' ');
    @memcpy(prefix[71 .. 71 + label.len], label);
}

/// A WFS superblock at its fixed byte offset, with a recognisable uuid.
fn buildWfs(prefix: *[PREFIX]u8, version: u32) void {
    @memset(prefix, 0);
    put32(prefix, 1024, 0x31534657); // 'W' 'F' 'S' '1'
    put32(prefix, 1024 + 4, version);
    var i: usize = 0;
    while (i < 16) : (i += 1) prefix[1024 + 144 + i] = @intCast(0xA0 + i);
}

/// A classic MBR: boot code, one populated entry, the 0x55AA signature.
fn buildMbr(prefix: *[PREFIX]u8, kind: u8) void {
    @memset(prefix, 0);
    const off = 0x1BE;
    prefix[off] = 0x80; // bootable
    prefix[off + 4] = kind;
    put32(prefix, off + 8, 63); // start LBA
    put32(prefix, off + 12, 1032129); // sector count
    prefix[510] = 0x55;
    prefix[511] = 0xAA;
}

fn labelOf(v: *const r.Verdict) []const u8 {
    var n: usize = 0;
    while (n < v.label.len and v.label[n] != 0) : (n += 1) {}
    return v.label[0..n];
}

// --- FAT ---------------------------------------------------------------------

test "fat: a FAT16 boot sector recognises, with its label and serial" {
    var prefix: [PREFIX]u8 = undefined;
    buildFat16(&prefix, "WASMOSBOOT");
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_FAT), v.fs_type);
    try std.testing.expect(v.has_label);
    try std.testing.expectEqualStrings("WASMOSBOOT", labelOf(&v));
    try std.testing.expect(v.has_uuid);
    // Little-endian on disk, so the low byte comes first.
    try std.testing.expectEqual(@as(u8, 0xFD), v.uuid[0]);
    try std.testing.expectEqual(@as(u8, 0xFA), v.uuid[3]);
    try std.testing.expectEqual(@as(u8, 4), v.uuid_len);
}

test "fat: FAT32 reads its label from the 32-bit extended boot record" {
    // The offsets differ by 28 bytes between FAT16 and FAT32. Reading a FAT32
    // volume at FAT16 offsets yields boot-code bytes as a label, which looks
    // like a corrupt name rather than a bug in the probe.
    var prefix: [PREFIX]u8 = undefined;
    buildFat32(&prefix, "BIGVOLUME");
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_FAT), v.fs_type);
    try std.testing.expectEqualStrings("BIGVOLUME", labelOf(&v));
    try std.testing.expectEqual(@as(u8, 0x78), v.uuid[0]);
}

test "fat: trailing pad spaces are not part of the label" {
    // FAT pads to 11 bytes with spaces, so "QEMU VVFAT " and "QEMU VVFAT" are
    // the same name -- and only the second is what a rule author would type.
    var prefix: [PREFIX]u8 = undefined;
    buildFat16(&prefix, "QEMU VVFAT");
    const v = r.recognise(&prefix);
    try std.testing.expectEqualStrings("QEMU VVFAT", labelOf(&v));
}

test "fat: an all-space label is present but empty" {
    // Distinct from a format that carries no label at all: has_label says the
    // volume COULD be named, which is what keeps ATTR{label}=="" from matching
    // a WFS volume.
    var prefix: [PREFIX]u8 = undefined;
    buildFat16(&prefix, "");
    const v = r.recognise(&prefix);
    try std.testing.expect(v.has_label);
    try std.testing.expectEqualStrings("", labelOf(&v));
}

test "fat: the boot signature alone is not enough" {
    // 0x55AA is carried by every MBR too. A probe that stops at the signature
    // reports a partition table as a filesystem.
    var prefix: [PREFIX]u8 = undefined;
    @memset(&prefix, 0);
    prefix[510] = 0x55;
    prefix[511] = 0xAA;
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_UNKNOWN), r.recognise(&prefix).fs_type);
}

test "fat: implausible geometry is refused" {
    const Case = struct { off: usize, val: u8, why: []const u8 };
    // Each mutation is individually fatal, so each must be tested against an
    // otherwise-valid sector rather than in combination.
    const cases = [_]Case{
        .{ .off = 0, .val = 0x00, .why = "no boot jump" },
        .{ .off = 13, .val = 3, .why = "cluster size not a power of two" },
        .{ .off = 13, .val = 0, .why = "zero sectors per cluster" },
        .{ .off = 16, .val = 0, .why = "zero FATs" },
        .{ .off = 21, .val = 0x00, .why = "media descriptor out of range" },
    };
    for (cases) |c| {
        var prefix: [PREFIX]u8 = undefined;
        buildFat16(&prefix, "X");
        prefix[c.off] = c.val;
        const v = r.recognise(&prefix);
        std.testing.expectEqual(@as(u32, abi.FS_TYPE_UNKNOWN), v.fs_type) catch |e| {
            std.debug.print("accepted a volume with {s}\n", .{c.why});
            return e;
        };
    }
}

test "fat: a zero bytes-per-sector is refused" {
    // Not in the byte-sized table above because the field is 16 bits, and zero
    // here is what makes every derived count a division by zero downstream.
    var prefix: [PREFIX]u8 = undefined;
    buildFat16(&prefix, "X");
    put16(&prefix, 11, 0);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_UNKNOWN), r.recognise(&prefix).fs_type);
}

test "fat: a sector-sized prefix is enough" {
    // The chain reads WFS first, which needs 2 KiB. A short prefix must not make
    // the FAT probe unreachable -- it must simply skip the probes that cannot
    // read their evidence.
    var prefix: [PREFIX]u8 = undefined;
    buildFat16(&prefix, "SHORT");
    const v = r.recognise(prefix[0..SECTOR]);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_FAT), v.fs_type);
}

// --- WFS ---------------------------------------------------------------------

test "wfs: a superblock recognises, with its uuid" {
    var prefix: [PREFIX]u8 = undefined;
    buildWfs(&prefix, 1);
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_WFS), v.fs_type);
    try std.testing.expect(v.has_uuid);
    try std.testing.expectEqual(@as(u8, 16), v.uuid_len);
    try std.testing.expectEqual(@as(u8, 0xA0), v.uuid[0]);
    try std.testing.expectEqual(@as(u8, 0xAF), v.uuid[15]);
}

test "wfs: carries no label, which is not the same as an empty one" {
    var prefix: [PREFIX]u8 = undefined;
    buildWfs(&prefix, 1);
    try std.testing.expect(!r.recognise(&prefix).has_label);
}

test "wfs: a future format version is not claimed" {
    // Reporting it as WFS invites a rule to mount it, and the driver refusing
    // the version later is a worse place to find the mismatch than here.
    var prefix: [PREFIX]u8 = undefined;
    buildWfs(&prefix, 2);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_UNKNOWN), r.recognise(&prefix).fs_type);
}

test "wfs: a prefix too short to hold the superblock is not WFS" {
    var prefix: [PREFIX]u8 = undefined;
    buildWfs(&prefix, 1);
    try std.testing.expectEqual(
        @as(u32, abi.FS_TYPE_UNKNOWN),
        r.recognise(prefix[0..1024]).fs_type,
    );
}

// --- precedence --------------------------------------------------------------

test "precedence: WFS wins over a FAT-shaped first sector" {
    // The case the chain order exists for. A volume can carry both a plausible
    // BPB at LBA 0 and a real WFS superblock at byte 1024 -- mkfs leaving the
    // previous filesystem's boot sector in place does exactly this -- and the
    // specific magic is the one to believe.
    var prefix: [PREFIX]u8 = undefined;
    buildFat16(&prefix, "STALE");
    put32(&prefix, 1024, 0x31534657);
    put32(&prefix, 1024 + 4, 1);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_WFS), r.recognise(&prefix).fs_type);
}

test "recognise: an empty volume is unknown, not an error" {
    // "No superblock matched" is a publishable answer: an unrecognised format
    // reads exactly like a blank disk, and ATTR{fstype}=="unknown" is a
    // legitimate matcher over both.
    var prefix: [PREFIX]u8 = [_]u8{0} ** PREFIX;
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_UNKNOWN), v.fs_type);
    try std.testing.expect(!v.has_label);
    try std.testing.expect(!v.has_uuid);
}

// --- table detection ---------------------------------------------------------

test "table: a classic MBR is detected" {
    var prefix: [PREFIX]u8 = undefined;
    buildMbr(&prefix, 0x06); // FAT16
    try std.testing.expectEqual(r.Scheme.mbr, r.detectScheme(&prefix));
}

test "table: a protective MBR reports GPT, not MBR" {
    // A GPT disk carries an MBR whose single entry spans the device. Reading it
    // as a legacy table describes one partition covering everything, which is a
    // volume nobody wrote.
    var prefix: [PREFIX]u8 = undefined;
    buildMbr(&prefix, 0xEE);
    try std.testing.expectEqual(r.Scheme.gpt, r.detectScheme(&prefix));
}

test "table: a FAT boot sector is not a partition table" {
    // The whole point of the suppression rule depending on detection rather
    // than on recognition failing: a raw FAT volume carries 0x55AA and must
    // still publish a volume.
    var prefix: [PREFIX]u8 = undefined;
    buildFat16(&prefix, "RAWVOL");
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(&prefix));
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_FAT), r.recognise(&prefix).fs_type);
}

test "table: a WFS volume carries no table" {
    var prefix: [PREFIX]u8 = undefined;
    buildWfs(&prefix, 1);
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(&prefix));
}

test "table: an empty slot table is not a table" {
    // Boot code that happens to end in 0x55AA with no usable entry is not
    // something to suppress a volume over.
    var prefix: [PREFIX]u8 = undefined;
    @memset(&prefix, 0);
    prefix[510] = 0x55;
    prefix[511] = 0xAA;
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(&prefix));
}

test "table: an entry with a type but no sectors addresses nothing" {
    var prefix: [PREFIX]u8 = undefined;
    buildMbr(&prefix, 0x06);
    put32(&prefix, 0x1BE + 12, 0); // sector count
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(&prefix));
}

test "table: one sector is enough to detect an MBR" {
    // LBA 1 is only needed for a GPT header. Demanding it to see a legacy table
    // made a one-sector read report `.none` for a disk that plainly has one.
    var prefix: [PREFIX]u8 = undefined;
    buildMbr(&prefix, 0x06);
    try std.testing.expectEqual(r.Scheme.mbr, r.detectScheme(prefix[0..SECTOR]));
}

test "table: one sector still sees a GPT through its protective entry" {
    var prefix: [PREFIX]u8 = undefined;
    buildMbr(&prefix, 0xEE);
    try std.testing.expectEqual(r.Scheme.gpt, r.detectScheme(prefix[0..SECTOR]));
}

test "table: nothing can be ruled in from less than a sector" {
    var prefix: [PREFIX]u8 = undefined;
    buildMbr(&prefix, 0x06);
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(prefix[0..64]));
}

// --- captured volumes --------------------------------------------------------
//
// Bytes below are CAPTURES, not constructions. Each was written once by a real
// tool and pasted here, so a case in this section cannot pass because the
// builders above and the probes share a misunderstanding -- which is the one
// failure mode a suite of self-built images has no way to detect.
//
// One case per on-disk shape this code has to tell apart: an MBR, a GPT,
// FAT16, FAT32, the vvfat volume the ESP actually is, and WFS. The bytes and
// the recipe that produced them live in `fixtures_disk_images.zig`, shared
// with the partition-table suite because an MBR is a table to one of them and
// a not-a-filesystem to the other.

/// Lay a captured boot-sector head into a zeroed prefix and set the signature,
/// which sits at 510 and is past every FAT capture above.
fn withBootSig(prefix: *[PREFIX]u8, boot: []const u8) void {
    @memset(prefix, 0);
    @memcpy(prefix[0..boot.len], boot);
    prefix[510] = 0x55;
    prefix[511] = 0xAA;
}

/// Lay a captured whole-sector block in at offset 0, signature included.
fn withSectors(prefix: *[PREFIX]u8, sectors: []const u8) void {
    @memset(prefix, 0);
    @memcpy(prefix[0..sectors.len], sectors);
}

test "captured mbr: a real table is detected, and is not a filesystem" {
    // The case the whole suppression rule turns on. This sector carries 0x55AA
    // exactly as a FAT boot sector does, so a probe that stops at the signature
    // publishes a volume for a disk whose partitions are also volumes.
    var prefix: [PREFIX]u8 = undefined;
    withSectors(&prefix, &fx.MBR_LBA0);
    try std.testing.expectEqual(r.Scheme.mbr, r.detectScheme(&prefix));
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_UNKNOWN), r.recognise(&prefix).fs_type);
}

test "captured gpt: a real GPT is detected as gpt, not mbr" {
    // A GPT disk carries a protective MBR whose single entry spans the device.
    // Reporting `.mbr` here would describe one partition covering everything --
    // a volume nobody wrote.
    var prefix: [PREFIX]u8 = undefined;
    withSectors(&prefix, &fx.GPT_HEAD);
    try std.testing.expectEqual(r.Scheme.gpt, r.detectScheme(&prefix));
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_UNKNOWN), r.recognise(&prefix).fs_type);
}

test "captured fat16: newfs_msdos output recognises with its label and serial" {
    var prefix: [PREFIX]u8 = undefined;
    withBootSig(&prefix, &fx.FAT16_BOOT);
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_FAT), v.fs_type);
    // Exactly 11 characters, so this is also the no-padding case: the label
    // field is full and there is no trailing space to trim.
    try std.testing.expectEqualStrings("WASMOSFAT16", labelOf(&v));
    try std.testing.expectEqual(@as(u8, 0x0D), v.uuid[0]);
    try std.testing.expectEqual(@as(u8, 0x47), v.uuid[3]);
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(&prefix));
}

test "captured vvfat: the ESP is FAT labelled QEMU VVFAT" {
    // The volume `/boot` will be selected by once mount policy moves to volumes,
    // which is why its label is asserted exactly. Padded to 11 bytes on disk, so
    // the trailing space must not become part of the name a rule matches.
    var prefix: [PREFIX]u8 = undefined;
    withBootSig(&prefix, &fx.VVFAT_BOOT);
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_FAT), v.fs_type);
    try std.testing.expectEqualStrings("QEMU VVFAT", labelOf(&v));
    try std.testing.expectEqual(@as(u8, 0xFD), v.uuid[0]);
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(&prefix));
}

test "captured fat32: the 32-bit boot record's offsets are the right ones" {
    // The case most likely to be wrong by construction: FAT32 moves the serial
    // and the label 28 bytes, and a probe using FAT16 offsets reads boot code as
    // a name. An independently written volume is what proves the offsets rather
    // than restating them.
    var prefix: [PREFIX]u8 = undefined;
    withBootSig(&prefix, &fx.FAT32_BOOT);
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_FAT), v.fs_type);
    try std.testing.expectEqualStrings("WASMOSFAT32", labelOf(&v));
    try std.testing.expectEqual(@as(u8, 0x0E), v.uuid[0]);
    try std.testing.expectEqual(@as(u8, 0xFD), v.uuid[3]);
}

test "captured wfs: mkfs_wfs output carries the uuid it was given" {
    // mkfs_wfs was told --uuid 0123456789ABCDEF0123456789ABCDEF, so the bytes
    // asserted here are the ones a rule would match on rather than an artefact
    // of where the field happens to sit.
    var prefix: [PREFIX]u8 = [_]u8{0} ** PREFIX;
    @memcpy(prefix[1024 .. 1024 + fx.WFS_SUPER.len], &fx.WFS_SUPER);
    const v = r.recognise(&prefix);
    try std.testing.expectEqual(@as(u32, abi.FS_TYPE_WFS), v.fs_type);
    try std.testing.expect(!v.has_label);
    try std.testing.expectEqual(@as(u8, 16), v.uuid_len);
    try std.testing.expectEqual(@as(u8, 0x01), v.uuid[0]);
    try std.testing.expectEqual(@as(u8, 0x23), v.uuid[1]);
    try std.testing.expectEqual(@as(u8, 0xEF), v.uuid[15]);
    try std.testing.expectEqual(r.Scheme.none, r.detectScheme(&prefix));
}
