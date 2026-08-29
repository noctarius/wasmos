//! recognise_wfs.zig — WFS recogniser.
//!
//! WFS puts a 32-bit magic at a fixed byte offset, so recognition is a constant
//! comparison rather than the plausibility argument FAT needs. That is what puts
//! this first in the chain: a format that can prove itself should get the chance
//! before one that can only fail to be disproved.
//!
//! Layout from `src/drivers/fs_wfs/wfs_format.h` §4, which is the authority; the
//! offsets below are the ones that file pins with `_Static_assert`, so a
//! superblock change that moves them breaks its own build before it reaches
//! here.
const r = @import("recognise.zig");
const abi = @import("wasmos_constants.zig");

/// The primary superblock sits at a fixed BYTE offset, not a block one:
/// `block_size` is itself a superblock field, so no block unit exists until the
/// superblock has been read.
const SUPER_OFFSET: usize = 1024;
const SUPER_SIZE: usize = 1024;

const SB_MAGIC: usize = 0;
const SB_VERSION: usize = 4;
/// Offset of `uuid` within the superblock, pinned by wfs_format.h.
const SB_UUID: usize = 144;
const UUID_LEN: usize = 16;

/// 'W' 'F' 'S' '1', little-endian.
const WFS_MAGIC: u32 = 0x31534657;
/// The only format version this tree writes. A later one is deliberately NOT
/// recognised: reporting a volume as WFS invites a rule to mount it, and a
/// driver that would refuse the version is a worse place to discover the
/// mismatch than here.
const WFS_VERSION: u32 = 1;

fn rd32(b: []const u8, off: usize) u32 {
    return @as(u32, b[off]) | (@as(u32, b[off + 1]) << 8) |
        (@as(u32, b[off + 2]) << 16) | (@as(u32, b[off + 3]) << 24);
}

pub fn probe(prefix: []const u8, out: *r.Verdict) bool {
    if (prefix.len < SUPER_OFFSET + SUPER_SIZE) return false;
    const sb = prefix[SUPER_OFFSET .. SUPER_OFFSET + SUPER_SIZE];

    if (rd32(sb, SB_MAGIC) != WFS_MAGIC) return false;
    if (rd32(sb, SB_VERSION) != WFS_VERSION) return false;

    out.fs_type = @intCast(abi.FS_TYPE_WFS);
    // WFS carries no volume label, so `has_label` stays false and a rule
    // matching a label can never select a WFS volume. That is the format's
    // property, not a gap here.
    r.setUuid(out, sb[SB_UUID .. SB_UUID + UUID_LEN]);
    return true;
}
