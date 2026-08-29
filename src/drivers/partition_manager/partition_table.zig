//! partition_table.zig — GPT and MBR partition-table parsing, with no I/O.
//!
//! The caller reads sectors and feeds them here; nothing in this file knows how
//! to reach a disk. That is what makes the parser testable on the host, and it
//! is where the interesting failure modes live: a table is untrusted input read
//! from a medium anyone can write, so every length and index in it is a bound to
//! check rather than a value to believe.
//!
//! Deliberately free of `@import("std")`: this module is compiled into a
//! freestanding wasm32 driver, where no other module in the tree imports std
//! either. The test cases live in `tests/unit/test_partition_table.zig`, which
//! imports this as a module and may use std freely.
//!
//! Layouts follow UEFI 2.10 §5.3 (GPT header, §5.3.2; entry array, §5.3.3) and
//! the conventional MBR at LBA 0. Field offsets are written as literals against
//! those tables rather than as a packed struct, because a packed struct would
//! also have to promise alignment this parser never needs — it reads from a byte
//! slice.

/// Sector size this parser assumes. Every offset below is relative to a sector
/// boundary, so a 4Kn disk needs more than a different constant here and is not
/// supported.
/// TODO: 4Kn support needs the caller's reported sector size threaded through
/// the GPT reads; `wasmos_block_descriptor_t.sector_bytes` already carries it.
pub const SECTOR_BYTES: usize = 512;

/// Partitions retained from one table. GPT allows 128 entries; keeping fewer is
/// a deliberate bound on this driver's own state, and it does NOT narrow
/// validation — the entry-array CRC is still computed over every byte the header
/// declares, so a table is accepted or rejected as a whole.
pub const MAX_PARTITIONS: usize = 32;

/// Largest entry size accepted. The spec requires a multiple of 8 and at least
/// 128; a ceiling is needed because the value comes off the disk and sizes a
/// buffer.
pub const MAX_ENTRY_BYTES: usize = 512;

/// Bytes reserved for a partition label, matching BLOCK_DESCRIPTOR_LABEL_MAX.
pub const LABEL_BYTES: usize = 144;

pub const Scheme = enum(u32) {
    none = 0,
    mbr = 1,
    gpt = 2,
};

/// One partition, in the form the descriptor wants. GUIDs are the RAW on-disk
/// bytes: GPT stores a GUID mixed-endian (first three fields little-endian), so
/// converting here would force every consumer to know which convention it had
/// been given. Text conversion belongs where a GUID is compared to something a
/// human wrote.
pub const Partition = struct {
    /// Table slot, 1-based. Gaps are PRESERVED: a disk with entries in slots 1
    /// and 3 yields p1 and p3, so deleting a neighbour never renumbers the
    /// survivors.
    slot: u32 = 0,
    lba_start: u64 = 0,
    lba_count: u64 = 0,
    type_guid: [16]u8 = [_]u8{0} ** 16,
    part_guid: [16]u8 = [_]u8{0} ** 16,
    /// MBR partition type byte; zero under GPT.
    mbr_type: u8 = 0,
    /// PARTLABEL as UTF-8, NUL-terminated; empty under MBR, which has no labels.
    label: [LABEL_BYTES]u8 = [_]u8{0} ** LABEL_BYTES,
};

pub const Table = struct {
    scheme: Scheme = .none,
    count: usize = 0,
    entries: [MAX_PARTITIONS]Partition = [_]Partition{.{}} ** MAX_PARTITIONS,

    fn push(self: *Table, part: Partition) void {
        if (self.count >= MAX_PARTITIONS) return;
        self.entries[self.count] = part;
        self.count += 1;
    }
};

// --- CRC32 (IEEE, reflected) -------------------------------------------------

/// Reflected polynomial 0xEDB88320, computed a nibble at a time: a 16-entry
/// table rather than 256, because this runs a few dozen kilobytes at boot and
/// the smaller table is worth more than the cycles.
const CRC_NIBBLE = [16]u32{
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

/// Feed `bytes` into a running CRC. The accumulator is the RAW register value,
/// not a finished CRC: start with `CRC_INIT` and finish with `crc32Final`, so a
/// value split across several calls yields the same result as one call. That is
/// what lets a 16 KiB entry array be validated while it is read sector by
/// sector, without ever holding all of it.
pub const CRC_INIT: u32 = 0xFFFFFFFF;

pub fn crc32Update(crc_in: u32, bytes: []const u8) u32 {
    var crc = crc_in;
    for (bytes) |b| {
        crc = CRC_NIBBLE[@as(usize, (crc ^ b) & 0x0F)] ^ (crc >> 4);
        crc = CRC_NIBBLE[@as(usize, (crc ^ (b >> 4)) & 0x0F)] ^ (crc >> 4);
    }
    return crc;
}

pub fn crc32Final(crc: u32) u32 {
    return crc ^ 0xFFFFFFFF;
}

pub fn crc32(bytes: []const u8) u32 {
    return crc32Final(crc32Update(CRC_INIT, bytes));
}

// --- little-endian readers ---------------------------------------------------

fn rd16(b: []const u8, off: usize) u16 {
    return @as(u16, b[off]) | (@as(u16, b[off + 1]) << 8);
}

fn rd32(b: []const u8, off: usize) u32 {
    return @as(u32, b[off]) | (@as(u32, b[off + 1]) << 8) |
        (@as(u32, b[off + 2]) << 16) | (@as(u32, b[off + 3]) << 24);
}

fn rd64(b: []const u8, off: usize) u64 {
    return @as(u64, rd32(b, off)) | (@as(u64, rd32(b, off + 4)) << 32);
}

// --- MBR ---------------------------------------------------------------------

const MBR_TABLE_OFF: usize = 0x1BE;
const MBR_ENTRY_BYTES: usize = 16;
const MBR_SLOTS: usize = 4;
const MBR_SIG_OFF: usize = 0x1FE;

/// Partition type byte of a PROTECTIVE entry: the whole-disk placeholder a
/// GPT-formatted disk puts in the legacy table so tools that only understand
/// MBR see one unknown partition covering everything instead of free space.
pub const MBR_TYPE_PROTECTIVE: u8 = 0xEE;
/// Extended-partition containers. Not followed: the logical partitions inside
/// form a linked list that needs further reads.
/// TODO: an extended container is reported as an ordinary partition today, so a
/// filesystem probe of its first sector finds nothing and it is skipped. Walking
/// the chain needs the caller to read each logical partition's own header.
pub const MBR_TYPE_EXTENDED_CHS: u8 = 0x05;
pub const MBR_TYPE_EXTENDED_LBA: u8 = 0x0F;

pub const MbrResult = enum {
    /// A usable MBR; `table` holds its partitions.
    ok,
    /// No 0x55AA signature: this is not an MBR at all.
    absent,
    /// A protective entry is present, so the disk claims GPT. The caller must
    /// use the GPT parser; treating the protective entry as a real partition
    /// would hand out one partition covering the whole disk.
    protective,
};

/// Parse the legacy table in `lba0`, which must be a full sector.
///
/// Returns `.protective` as soon as a protective entry is seen, even if other
/// entries look real: a hybrid table is a disk that claims GPT, and the GPT is
/// the authority.
pub fn parseMbr(lba0: []const u8, out: *Table) MbrResult {
    if (lba0.len < SECTOR_BYTES) return .absent;
    if (lba0[MBR_SIG_OFF] != 0x55 or lba0[MBR_SIG_OFF + 1] != 0xAA) return .absent;

    out.* = .{};
    var slot: usize = 0;
    while (slot < MBR_SLOTS) : (slot += 1) {
        const off = MBR_TABLE_OFF + slot * MBR_ENTRY_BYTES;
        const kind = lba0[off + 4];
        if (kind == MBR_TYPE_PROTECTIVE) return .protective;
        if (kind == 0) continue; // empty slot
        const start = rd32(lba0, off + 8);
        const count = rd32(lba0, off + 12);
        if (count == 0) continue; // a zero-length partition addresses nothing
        out.push(.{
            .slot = @intCast(slot + 1),
            .lba_start = start,
            .lba_count = count,
            .mbr_type = kind,
        });
    }
    out.scheme = .mbr;
    return .ok;
}

// --- GPT ---------------------------------------------------------------------

const GPT_SIGNATURE = [8]u8{ 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
const GPT_HEADER_MIN: usize = 92;

const HDR_SIGNATURE: usize = 0;
const HDR_HEADER_SIZE: usize = 12;
const HDR_HEADER_CRC: usize = 16;
const HDR_MY_LBA: usize = 24;
const HDR_ALTERNATE_LBA: usize = 32;
const HDR_ENTRY_LBA: usize = 72;
const HDR_ENTRY_COUNT: usize = 80;
const HDR_ENTRY_SIZE: usize = 84;
const HDR_ENTRY_CRC: usize = 88;

const ENT_TYPE_GUID: usize = 0;
const ENT_PART_GUID: usize = 16;
const ENT_FIRST_LBA: usize = 32;
const ENT_LAST_LBA: usize = 40;
const ENT_NAME: usize = 56;
const ENT_NAME_UNITS: usize = 36;

pub const GptHeader = struct {
    /// LBA holding this header, as the header itself claims.
    my_lba: u64 = 0,
    /// Where the OTHER copy lives. On a primary-header failure this is where to
    /// retry, and it is the whole reason GPT is preferable to MBR.
    alternate_lba: u64 = 0,
    entry_lba: u64 = 0,
    entry_count: u32 = 0,
    entry_size: u32 = 0,
    entry_crc: u32 = 0,

    /// Total bytes of the entry array the header declares.
    pub fn entryBytes(self: GptHeader) u64 {
        return @as(u64, self.entry_count) * @as(u64, self.entry_size);
    }
};

/// Validate a GPT header sector and return it.
///
/// Checks, in order: the signature, that `header_size` lies within
/// [92, sector], and that the stored CRC matches one computed over exactly
/// `header_size` bytes with the CRC field itself taken as zero — which is how
/// the spec defines it, and why the header cannot simply be CRC'd as read.
///
/// Returns null on any failure; the caller then tries `alternate_lba` from
/// whichever copy it did manage to read, or gives up.
pub fn parseGptHeader(sector: []const u8) ?GptHeader {
    if (sector.len < SECTOR_BYTES) return null;
    for (GPT_SIGNATURE, 0..) |c, i| {
        if (sector[HDR_SIGNATURE + i] != c) return null;
    }
    const header_size = rd32(sector, HDR_HEADER_SIZE);
    if (header_size < GPT_HEADER_MIN or header_size > SECTOR_BYTES) return null;

    const stored = rd32(sector, HDR_HEADER_CRC);
    var crc = crc32Update(CRC_INIT, sector[0..HDR_HEADER_CRC]);
    crc = crc32Update(crc, &[_]u8{ 0, 0, 0, 0 });
    crc = crc32Update(crc, sector[HDR_HEADER_CRC + 4 .. header_size]);
    if (crc32Final(crc) != stored) return null;

    const entry_size = rd32(sector, HDR_ENTRY_SIZE);
    if (entry_size < 128 or entry_size > MAX_ENTRY_BYTES or (entry_size % 8) != 0) return null;
    const entry_count = rd32(sector, HDR_ENTRY_COUNT);
    if (entry_count == 0) return null;
    // Bound the declared array so a corrupt count cannot ask the caller for an
    // unbounded read. 128 entries is the conventional maximum; accept a little
    // more, refuse absurdity.
    if (entry_count > 512) return null;

    return .{
        .my_lba = rd64(sector, HDR_MY_LBA),
        .alternate_lba = rd64(sector, HDR_ALTERNATE_LBA),
        .entry_lba = rd64(sector, HDR_ENTRY_LBA),
        .entry_count = entry_count,
        .entry_size = entry_size,
        .entry_crc = rd32(sector, HDR_ENTRY_CRC),
    };
}

/// Streams the entry array past the parser while the caller reads it.
///
/// The array can reach 128 x 128 bytes = 32 sectors, which does not fit the 8
/// KiB block buffer, so it is fed in chunks. The CRC accumulates over EVERY byte
/// the header declared even though only the first MAX_PARTITIONS usable entries
/// are retained: validation covers the whole array or it covers nothing, and a
/// table truncated to what happened to fit is not a table that was checked.
pub const GptEntryScan = struct {
    header: GptHeader,
    table: Table = .{},
    crc: u32 = CRC_INIT,
    consumed: u64 = 0,
    /// An entry may straddle a chunk boundary; this holds the leading part until
    /// the rest arrives.
    partial: [MAX_ENTRY_BYTES]u8 = [_]u8{0} ** MAX_ENTRY_BYTES,
    partial_len: usize = 0,
    /// Index of the entry currently being assembled, i.e. its table slot minus 1.
    index: u32 = 0,

    pub fn init(header: GptHeader) GptEntryScan {
        return .{ .header = header };
    }

    /// Feed the next bytes of the entry array. Bytes past the declared array
    /// length are ignored, so a caller may hand over whole sectors without
    /// trimming the tail of the last one.
    pub fn feed(self: *GptEntryScan, bytes: []const u8) void {
        const total = self.header.entryBytes();
        var chunk = bytes;
        if (self.consumed + chunk.len > total) {
            const room = total - self.consumed;
            chunk = chunk[0..@intCast(room)];
        }
        self.crc = crc32Update(self.crc, chunk);
        self.consumed += chunk.len;

        const entry_size: usize = @intCast(self.header.entry_size);
        var rest = chunk;
        while (rest.len > 0) {
            const want = entry_size - self.partial_len;
            const take = if (rest.len < want) rest.len else want;
            var i: usize = 0;
            while (i < take) : (i += 1) {
                self.partial[self.partial_len + i] = rest[i];
            }
            self.partial_len += take;
            rest = rest[take..];
            if (self.partial_len == entry_size) {
                self.takeEntry(self.partial[0..entry_size]);
                self.partial_len = 0;
                self.index += 1;
            }
        }
    }

    /// An entry whose type GUID is all zero is an unused slot, not a partition.
    fn takeEntry(self: *GptEntryScan, entry: []const u8) void {
        var zero = true;
        for (entry[ENT_TYPE_GUID .. ENT_TYPE_GUID + 16]) |b| {
            if (b != 0) {
                zero = false;
                break;
            }
        }
        if (zero) return;

        const first = rd64(entry, ENT_FIRST_LBA);
        const last = rd64(entry, ENT_LAST_LBA);
        // EndingLBA is INCLUSIVE (UEFI 2.10 §5.3.3), so a one-sector partition
        // has last == first. A last below first describes nothing.
        if (last < first) return;

        var part = Partition{
            .slot = self.index + 1,
            .lba_start = first,
            .lba_count = last - first + 1,
        };
        var i: usize = 0;
        while (i < 16) : (i += 1) {
            part.type_guid[i] = entry[ENT_TYPE_GUID + i];
            part.part_guid[i] = entry[ENT_PART_GUID + i];
        }
        utf16leToUtf8(entry[ENT_NAME .. ENT_NAME + ENT_NAME_UNITS * 2], &part.label);
        self.table.push(part);
    }

    /// Finish the scan. Returns the table only if the whole declared array was
    /// fed and its CRC matches the header's.
    pub fn finish(self: *GptEntryScan) ?Table {
        if (self.consumed != self.header.entryBytes()) return null;
        if (crc32Final(self.crc) != self.header.entry_crc) return null;
        var out = self.table;
        out.scheme = .gpt;
        return out;
    }
};

// --- label decoding ----------------------------------------------------------

/// Decode a GPT partition name (UTF-16LE, NUL-padded) into NUL-terminated UTF-8.
///
/// Truncates rather than overflowing, and never emits a partial code point. The
/// worst case is 3 UTF-8 bytes per code unit — a code point needing 4 lies
/// outside the BMP and costs TWO units — so LABEL_BYTES clears 36 units with
/// room to spare and truncation cannot happen for a well-formed name.
///
/// An unpaired surrogate is dropped rather than encoded: WTF-8 in a field that
/// is compared against rule text would produce a label nothing can match.
pub fn utf16leToUtf8(name: []const u8, out: *[LABEL_BYTES]u8) void {
    for (out) |*b| b.* = 0;
    var out_len: usize = 0;
    var i: usize = 0;
    while (i + 1 < name.len) : (i += 2) {
        var cp: u32 = @as(u32, name[i]) | (@as(u32, name[i + 1]) << 8);
        if (cp == 0) break; // NUL terminates; the rest is padding
        if (cp >= 0xD800 and cp <= 0xDBFF) {
            // High surrogate: needs its low half to mean anything.
            if (i + 3 >= name.len) return;
            const lo: u32 = @as(u32, name[i + 2]) | (@as(u32, name[i + 3]) << 8);
            if (lo < 0xDC00 or lo > 0xDFFF) return;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        } else if (cp >= 0xDC00 and cp <= 0xDFFF) {
            return; // lone low surrogate
        }

        const need: usize = if (cp < 0x80) 1 else if (cp < 0x800) @as(usize, 2) else if (cp < 0x10000) @as(usize, 3) else @as(usize, 4);
        if (out_len + need + 1 > out.len) return; // keep room for the NUL
        switch (need) {
            1 => out[out_len] = @intCast(cp),
            2 => {
                out[out_len] = @intCast(0xC0 | (cp >> 6));
                out[out_len + 1] = @intCast(0x80 | (cp & 0x3F));
            },
            3 => {
                out[out_len] = @intCast(0xE0 | (cp >> 12));
                out[out_len + 1] = @intCast(0x80 | ((cp >> 6) & 0x3F));
                out[out_len + 2] = @intCast(0x80 | (cp & 0x3F));
            },
            else => {
                out[out_len] = @intCast(0xF0 | (cp >> 18));
                out[out_len + 1] = @intCast(0x80 | ((cp >> 12) & 0x3F));
                out[out_len + 2] = @intCast(0x80 | ((cp >> 6) & 0x3F));
                out[out_len + 3] = @intCast(0x80 | (cp & 0x3F));
            },
        }
        out_len += need;
    }
}
