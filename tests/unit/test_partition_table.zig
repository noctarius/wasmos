//! Unit tests for the GPT/MBR partition-table parser
//! (src/drivers/partition_manager/partition_table.zig).
//!
//! The parser does no I/O, so every case here builds a sector image in memory
//! and feeds it in. That is the whole reason the parser takes byte slices rather
//! than reading a disk: a partition table is untrusted input from a medium
//! anyone can write, and the cases that matter are the malformed ones, which are
//! impractical to produce on a real device and trivial here.
//!
//! The tests live in their own module because `partition_table.zig` is compiled
//! into a freestanding wasm32 driver and no module in the tree imports std.
//! Here std is free.
//!
//! What is deliberately NOT tested: that a real disk's table parses. That is the
//! integration test's job, and a hand-built image asserting against itself would
//! only prove the builder and the parser share a misunderstanding.
const std = @import("std");
const pt = @import("partition_table");

const SECTOR = pt.SECTOR_BYTES;

// --- helpers -----------------------------------------------------------------

fn put32(buf: []u8, off: usize, v: u32) void {
    buf[off] = @intCast(v & 0xFF);
    buf[off + 1] = @intCast((v >> 8) & 0xFF);
    buf[off + 2] = @intCast((v >> 16) & 0xFF);
    buf[off + 3] = @intCast((v >> 24) & 0xFF);
}

fn put64(buf: []u8, off: usize, v: u64) void {
    put32(buf, off, @intCast(v & 0xFFFFFFFF));
    put32(buf, off + 4, @intCast((v >> 32) & 0xFFFFFFFF));
}

/// An MBR with `count` populated slots starting at slot 0, each 100 sectors
/// apart, type 0x0C (FAT32 LBA).
fn buildMbr(buf: *[SECTOR]u8, count: usize) void {
    @memset(buf, 0);
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const off = 0x1BE + i * 16;
        buf[off + 4] = 0x0C;
        put32(buf, off + 8, @intCast(63 + i * 100));
        put32(buf, off + 12, 100);
    }
    buf[0x1FE] = 0x55;
    buf[0x1FF] = 0xAA;
}

const GptImage = struct {
    header: [SECTOR]u8 = [_]u8{0} ** SECTOR,
    /// Four 128-byte entries, i.e. one sector's worth.
    entries: [SECTOR]u8 = [_]u8{0} ** SECTOR,
    entry_count: u32 = 4,
    entry_size: u32 = 128,
};

/// Build a GPT header plus a one-sector entry array holding `slots` partitions.
/// `slots` names WHICH entry indexes are populated, so gaps can be expressed.
fn buildGpt(img: *GptImage, slots: []const u32) void {
    img.* = .{};
    for (slots) |slot| {
        const off = slot * 128;
        // A non-zero type GUID is what marks an entry used.
        img.entries[off] = 0x28;
        img.entries[off + 15] = 0x3B;
        img.entries[off + 16] = 0xA1; // unique GUID
        put64(&img.entries, off + 32, 2048 + @as(u64, slot) * 1000);
        // EndingLBA is INCLUSIVE, so this is 1000 sectors.
        put64(&img.entries, off + 40, 2048 + @as(u64, slot) * 1000 + 999);
    }

    const h = &img.header;
    @memcpy(h[0..8], "EFI PART");
    put32(h, 8, 0x00010000); // revision
    put32(h, 12, 92); // header_size
    put64(h, 24, 1); // my_lba
    put64(h, 32, 4095); // alternate_lba
    put64(h, 72, 2); // entry_lba
    put32(h, 80, img.entry_count);
    put32(h, 84, img.entry_size);
    put32(h, 88, pt.crc32(img.entries[0 .. img.entry_count * img.entry_size]));
    put32(h, 16, 0); // header CRC field is zero while computing it
    put32(h, 16, pt.crc32(h[0..92]));
}

/// Drive the streaming entry scan a sector at a time, the way the service will.
fn scanEntries(img: *const GptImage, header: pt.GptHeader) ?pt.Table {
    var scan = pt.GptEntryScan.init(header);
    var fed: usize = 0;
    const total: usize = @intCast(header.entryBytes());
    while (fed < total) {
        const room = total - fed;
        const take = if (room < SECTOR) room else SECTOR;
        scan.feed(img.entries[fed .. fed + take]);
        fed += take;
    }
    return scan.finish();
}

// --- CRC32 -------------------------------------------------------------------

test "crc32 matches the published IEEE check value" {
    // The standard check vector: CRC-32/ISO-HDLC of "123456789".
    try std.testing.expectEqual(@as(u32, 0xCBF43926), pt.crc32("123456789"));
    try std.testing.expectEqual(@as(u32, 0x00000000), pt.crc32(""));
}

test "crc32 fed in pieces equals crc32 fed at once" {
    // This is the property the entry-array scan depends on: a 16 KiB array is
    // validated while it is read sector by sector, never held whole.
    const data = "the quick brown fox jumps over the lazy dog";
    var crc = pt.CRC_INIT;
    crc = pt.crc32Update(crc, data[0..10]);
    crc = pt.crc32Update(crc, data[10..11]);
    crc = pt.crc32Update(crc, data[11..]);
    try std.testing.expectEqual(pt.crc32(data), pt.crc32Final(crc));
}

// --- MBR ---------------------------------------------------------------------

test "mbr: parses populated slots and skips empty ones" {
    var sector: [SECTOR]u8 = undefined;
    buildMbr(&sector, 2);
    var table = pt.Table{};
    try std.testing.expectEqual(pt.MbrResult.ok, pt.parseMbr(&sector, &table));
    try std.testing.expectEqual(pt.Scheme.mbr, table.scheme);
    try std.testing.expectEqual(@as(usize, 2), table.count);
    try std.testing.expectEqual(@as(u32, 1), table.entries[0].slot);
    try std.testing.expectEqual(@as(u64, 63), table.entries[0].lba_start);
    try std.testing.expectEqual(@as(u64, 100), table.entries[0].lba_count);
    try std.testing.expectEqual(@as(u8, 0x0C), table.entries[0].mbr_type);
    try std.testing.expectEqual(@as(u32, 2), table.entries[1].slot);
    // MBR has no labels; the field must be empty rather than uninitialised.
    try std.testing.expectEqual(@as(u8, 0), table.entries[0].label[0]);
}

test "mbr: slot numbers follow the table, not discovery order" {
    // Only slot 2 (0-based) is populated, so the partition is p3 and there is no
    // p1 or p2. Renumbering it to p1 would rename a volume whenever a neighbour
    // was deleted.
    var sector: [SECTOR]u8 = [_]u8{0} ** SECTOR;
    const off = 0x1BE + 2 * 16;
    sector[off + 4] = 0x83;
    put32(&sector, off + 8, 2048);
    put32(&sector, off + 12, 500);
    sector[0x1FE] = 0x55;
    sector[0x1FF] = 0xAA;

    var table = pt.Table{};
    try std.testing.expectEqual(pt.MbrResult.ok, pt.parseMbr(&sector, &table));
    try std.testing.expectEqual(@as(usize, 1), table.count);
    try std.testing.expectEqual(@as(u32, 3), table.entries[0].slot);
}

test "mbr: a missing 0x55AA signature is not a table" {
    var sector: [SECTOR]u8 = undefined;
    buildMbr(&sector, 1);
    sector[0x1FF] = 0x00;
    var table = pt.Table{};
    try std.testing.expectEqual(pt.MbrResult.absent, pt.parseMbr(&sector, &table));
}

test "mbr: a protective entry means the disk claims GPT" {
    // Reported even though the other slots look like real partitions: a hybrid
    // table is a disk claiming GPT, and treating the protective entry as real
    // would hand out one partition covering the whole disk.
    var sector: [SECTOR]u8 = undefined;
    buildMbr(&sector, 2);
    sector[0x1BE + 4] = pt.MBR_TYPE_PROTECTIVE;
    var table = pt.Table{};
    try std.testing.expectEqual(pt.MbrResult.protective, pt.parseMbr(&sector, &table));
}

test "mbr: a zero-length partition addresses nothing and is skipped" {
    var sector: [SECTOR]u8 = undefined;
    buildMbr(&sector, 1);
    put32(&sector, 0x1BE + 12, 0);
    var table = pt.Table{};
    try std.testing.expectEqual(pt.MbrResult.ok, pt.parseMbr(&sector, &table));
    try std.testing.expectEqual(@as(usize, 0), table.count);
}

// --- GPT header --------------------------------------------------------------

test "gpt: a well-formed header parses" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{ 0, 1 });
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    try std.testing.expectEqual(@as(u64, 1), header.my_lba);
    try std.testing.expectEqual(@as(u64, 4095), header.alternate_lba);
    try std.testing.expectEqual(@as(u64, 2), header.entry_lba);
    try std.testing.expectEqual(@as(u32, 128), header.entry_size);
    try std.testing.expectEqual(@as(u64, 512), header.entryBytes());
}

test "gpt: a bad signature is refused" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    img.header[0] = 'X';
    try std.testing.expect(pt.parseGptHeader(&img.header) == null);
}

test "gpt: a corrupt header byte is caught by the header CRC" {
    // The byte flipped is INSIDE header_size and outside the CRC field, which is
    // the only region the CRC covers. This is the case a signature check alone
    // would pass.
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    img.header[80] ^= 0xFF; // entry_count
    try std.testing.expect(pt.parseGptHeader(&img.header) == null);
}

test "gpt: header_size outside [92, sector] is refused" {
    var img: GptImage = undefined;

    buildGpt(&img, &[_]u32{0});
    put32(&img.header, 12, 91);
    put32(&img.header, 16, 0);
    put32(&img.header, 16, pt.crc32(img.header[0..91]));
    try std.testing.expect(pt.parseGptHeader(&img.header) == null);

    // Larger than a sector: the CRC would read past the buffer, so the bound is
    // a memory-safety check and not only a validity one.
    buildGpt(&img, &[_]u32{0});
    put32(&img.header, 12, SECTOR + 8);
    try std.testing.expect(pt.parseGptHeader(&img.header) == null);
}

test "gpt: an implausible entry size or count is refused" {
    var img: GptImage = undefined;

    // Below the 128-byte minimum.
    buildGpt(&img, &[_]u32{0});
    put32(&img.header, 84, 64);
    put32(&img.header, 16, 0);
    put32(&img.header, 16, pt.crc32(img.header[0..92]));
    try std.testing.expect(pt.parseGptHeader(&img.header) == null);

    // Not a multiple of 8.
    buildGpt(&img, &[_]u32{0});
    put32(&img.header, 84, 132);
    put32(&img.header, 16, 0);
    put32(&img.header, 16, pt.crc32(img.header[0..92]));
    try std.testing.expect(pt.parseGptHeader(&img.header) == null);

    // A count that would ask the caller for an unbounded read.
    buildGpt(&img, &[_]u32{0});
    put32(&img.header, 80, 100000);
    put32(&img.header, 16, 0);
    put32(&img.header, 16, pt.crc32(img.header[0..92]));
    try std.testing.expect(pt.parseGptHeader(&img.header) == null);
}

// --- GPT entry array ---------------------------------------------------------

test "gpt: entries parse with inclusive EndingLBA" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{ 0, 1 });
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    const table = scanEntries(&img, header) orelse return error.EntriesRejected;

    try std.testing.expectEqual(pt.Scheme.gpt, table.scheme);
    try std.testing.expectEqual(@as(usize, 2), table.count);
    try std.testing.expectEqual(@as(u32, 1), table.entries[0].slot);
    try std.testing.expectEqual(@as(u64, 2048), table.entries[0].lba_start);
    // 2048..3047 inclusive is 1000 sectors, not 999.
    try std.testing.expectEqual(@as(u64, 1000), table.entries[0].lba_count);
    try std.testing.expectEqual(@as(u32, 2), table.entries[1].slot);
    try std.testing.expectEqual(@as(u64, 3048), table.entries[1].lba_start);
}

test "gpt: unused slots are skipped and gaps are preserved" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{ 0, 2 });
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    const table = scanEntries(&img, header) orelse return error.EntriesRejected;
    try std.testing.expectEqual(@as(usize, 2), table.count);
    try std.testing.expectEqual(@as(u32, 1), table.entries[0].slot);
    try std.testing.expectEqual(@as(u32, 3), table.entries[1].slot);
}

test "gpt: a corrupt entry is caught by the array CRC" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    img.entries[40] ^= 0xFF; // EndingLBA of entry 0
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    try std.testing.expect(scanEntries(&img, header) == null);
}

test "gpt: corruption in an UNUSED entry still fails the array CRC" {
    // The array is validated as a whole. A parser that CRC'd only the entries it
    // retained would accept a table whose tail had rotted, which is exactly the
    // case the retention cap makes tempting.
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    img.entries[3 * 128 + 64] ^= 0xFF; // inside unused slot 3
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    try std.testing.expect(scanEntries(&img, header) == null);
}

test "gpt: a short feed is refused" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    var scan = pt.GptEntryScan.init(header);
    scan.feed(img.entries[0..256]); // half the declared array
    try std.testing.expect(scan.finish() == null);
}

test "gpt: the length check catches a short feed the CRC cannot" {
    // The case above is caught by the CRC as much as by the length, so it does
    // not isolate the length check. This one does: crc32 of NOTHING is 0, so a
    // header declaring entry_crc = 0 and a caller that fed nothing agree on the
    // checksum. Only "did I receive every byte the header declared" separates
    // them, which is why that check is not redundant with the CRC.
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    put32(&img.header, 88, 0); // entry_crc = crc32("")
    put32(&img.header, 16, 0);
    put32(&img.header, 16, pt.crc32(img.header[0..92]));

    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    try std.testing.expectEqual(@as(u32, 0), pt.crc32(""));
    var scan = pt.GptEntryScan.init(header);
    try std.testing.expect(scan.finish() == null);
}

test "gpt: entries split across feeds reassemble" {
    // The service feeds whole sectors, and an entry size that does not divide the
    // sector splits one across two feeds. Fed a byte at a time here, which is the
    // same code path taken to its extreme.
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{ 0, 1 });
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;

    var scan = pt.GptEntryScan.init(header);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        scan.feed(img.entries[i .. i + 1]);
    }
    const table = scan.finish() orelse return error.EntriesRejected;
    try std.testing.expectEqual(@as(usize, 2), table.count);
    try std.testing.expectEqual(@as(u64, 2048), table.entries[0].lba_start);
}

test "gpt: an entry whose end precedes its start describes nothing" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    put64(&img.entries, 40, 100); // EndingLBA below FirstUsableLBA (2048)
    put32(&img.header, 88, pt.crc32(img.entries[0..512]));
    put32(&img.header, 16, 0);
    put32(&img.header, 16, pt.crc32(img.header[0..92]));

    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    const table = scanEntries(&img, header) orelse return error.EntriesRejected;
    try std.testing.expectEqual(@as(usize, 0), table.count);
}

// --- labels ------------------------------------------------------------------

test "utf16: ascii name decodes and terminates" {
    var name: [72]u8 = [_]u8{0} ** 72;
    const text = "boot";
    for (text, 0..) |c, i| name[i * 2] = c;

    var out: [pt.LABEL_BYTES]u8 = undefined;
    pt.utf16leToUtf8(&name, &out);
    try std.testing.expectEqualStrings("boot", std.mem.sliceTo(&out, 0));
}

test "utf16: multi-byte code points encode at the right width" {
    var name: [72]u8 = [_]u8{0} ** 72;
    // U+00E4 (2 bytes in UTF-8), U+20AC (3 bytes).
    name[0] = 0xE4;
    name[1] = 0x00;
    name[2] = 0xAC;
    name[3] = 0x20;

    var out: [pt.LABEL_BYTES]u8 = undefined;
    pt.utf16leToUtf8(&name, &out);
    try std.testing.expectEqualStrings("ä€", std.mem.sliceTo(&out, 0));
}

test "utf16: a surrogate pair becomes one four-byte code point" {
    var name: [72]u8 = [_]u8{0} ** 72;
    // U+1F600, encoded as the pair D83D DE00.
    name[0] = 0x3D;
    name[1] = 0xD8;
    name[2] = 0x00;
    name[3] = 0xDE;

    var out: [pt.LABEL_BYTES]u8 = undefined;
    pt.utf16leToUtf8(&name, &out);
    const decoded = std.mem.sliceTo(&out, 0);
    try std.testing.expectEqual(@as(usize, 4), decoded.len);
    try std.testing.expectEqualStrings("\u{1F600}", decoded);
}

test "utf16: a lone surrogate is dropped, not encoded as WTF-8" {
    // A label compared against rule text must not contain an encoding nothing
    // else produces; the alternative is a label no rule can ever match.
    var name: [72]u8 = [_]u8{0} ** 72;
    name[0] = 0x00;
    name[1] = 0xDC; // lone LOW surrogate
    var out: [pt.LABEL_BYTES]u8 = undefined;
    pt.utf16leToUtf8(&name, &out);
    try std.testing.expectEqual(@as(usize, 0), std.mem.sliceTo(&out, 0).len);
}

test "utf16: a 36-unit name of 3-byte code points still fits" {
    // The bound that sized LABEL_BYTES: worst case is 3 UTF-8 bytes per code
    // UNIT, because a 4-byte code point costs two units. 36 * 3 + NUL = 109.
    var name: [72]u8 = undefined;
    var i: usize = 0;
    while (i < 36) : (i += 1) {
        name[i * 2] = 0xAC; // U+20AC
        name[i * 2 + 1] = 0x20;
    }
    var out: [pt.LABEL_BYTES]u8 = undefined;
    pt.utf16leToUtf8(&name, &out);
    try std.testing.expectEqual(@as(usize, 108), std.mem.sliceTo(&out, 0).len);
}

test "gpt: a label survives the entry parse" {
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    const text = "user";
    for (text, 0..) |c, i| img.entries[56 + i * 2] = c;
    put32(&img.header, 88, pt.crc32(img.entries[0..512]));
    put32(&img.header, 16, 0);
    put32(&img.header, 16, pt.crc32(img.header[0..92]));

    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    const table = scanEntries(&img, header) orelse return error.EntriesRejected;
    try std.testing.expectEqual(@as(usize, 1), table.count);
    try std.testing.expectEqualStrings("user", std.mem.sliceTo(&table.entries[0].label, 0));
}

test "gpt: type and partition GUIDs are the raw on-disk bytes" {
    // Kept verbatim because GPT stores a GUID mixed-endian; converting here would
    // force every consumer to know which convention it had been handed.
    var img: GptImage = undefined;
    buildGpt(&img, &[_]u32{0});
    const header = pt.parseGptHeader(&img.header) orelse return error.HeaderRejected;
    const table = scanEntries(&img, header) orelse return error.EntriesRejected;
    try std.testing.expectEqual(@as(u8, 0x28), table.entries[0].type_guid[0]);
    try std.testing.expectEqual(@as(u8, 0x3B), table.entries[0].type_guid[15]);
    try std.testing.expectEqual(@as(u8, 0xA1), table.entries[0].part_guid[0]);
}
