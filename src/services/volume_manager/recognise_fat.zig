//! recognise_fat.zig — FAT12/16/32 recogniser.
//!
//! Reads the BIOS Parameter Block in the volume's first sector. There is no FAT
//! magic number: the format is identified by a set of fields being individually
//! plausible, which is why this recogniser sits LAST in the chain and why every
//! field it trusts is bounded here rather than merely read.
//!
//! The bar is deliberately the same one `fat_parse_boot`
//! (`src/drivers/fs_fat/fat_geom.c`) sets before it will mount, plus the checks
//! that separate a BPB from the two things that share its first sector's
//! signature: an MBR and a GPT protective MBR. Those carry 0x55AA too, so the
//! signature alone identifies nothing — reading `bytes_per_sector` out of an
//! MBR's boot code is what a naive probe does, and it usually fails only by
//! luck.
const r = @import("recognise.zig");
const abi = @import("wasmos_constants.zig");

const SECTOR_BYTES: usize = 512;

// Offsets into the boot sector, from Microsoft's FAT specification. Written as
// literals against that table for the same reason partition_table.zig does: a
// packed struct would promise an alignment this code never needs, since it reads
// from a byte slice.
const BPB_JUMP: usize = 0;
const BPB_BYTES_PER_SECTOR: usize = 11;
const BPB_SECTORS_PER_CLUSTER: usize = 13;
const BPB_RESERVED_SECTORS: usize = 14;
const BPB_NUM_FATS: usize = 16;
const BPB_ROOT_ENTRIES: usize = 17;
const BPB_MEDIA: usize = 21;
const BPB_FAT_SIZE_16: usize = 22;
/// FAT12/16 extended boot record.
const EBR16_SERIAL: usize = 39;
const EBR16_LABEL: usize = 43;
/// FAT32 extended boot record, which the 32-bit FAT size pushes 28 bytes later.
const EBR32_SERIAL: usize = 67;
const EBR32_LABEL: usize = 71;
const LABEL_BYTES: usize = 11;
const SIG_OFF: usize = 510;

fn rd16(b: []const u8, off: usize) u16 {
    return @as(u16, b[off]) | (@as(u16, b[off + 1]) << 8);
}

/// A boot sector begins with a jump the BIOS would execute: a short jump plus a
/// NOP, or a near jump. An MBR's first bytes are its own boot code and match
/// this only by coincidence, so it is cheap evidence but real evidence.
fn plausibleJump(sector: []const u8) bool {
    return (sector[BPB_JUMP] == 0xEB and sector[BPB_JUMP + 2] == 0x90) or
        sector[BPB_JUMP] == 0xE9;
}

pub fn probe(prefix: []const u8, out: *r.Verdict) bool {
    if (prefix.len < SECTOR_BYTES) return false;
    const sector = prefix[0..SECTOR_BYTES];

    if (sector[SIG_OFF] != 0x55 or sector[SIG_OFF + 1] != 0xAA) return false;
    if (!plausibleJump(sector)) return false;

    const bytes_per_sector = rd16(sector, BPB_BYTES_PER_SECTOR);
    if (bytes_per_sector != 512 and bytes_per_sector != 1024 and
        bytes_per_sector != 2048 and bytes_per_sector != 4096) return false;

    // A power of two in 1..128. Zero would make the cluster count a division by
    // zero in every consumer; a non-power-of-two is not expressible in the
    // format at all.
    const spc = sector[BPB_SECTORS_PER_CLUSTER];
    if (spc == 0 or (spc & (spc -% 1)) != 0) return false;

    // The reserved region holds the boot sector itself, so it is never empty.
    if (rd16(sector, BPB_RESERVED_SECTORS) == 0) return false;

    // One FAT or two in every volume anyone writes; more is legal in the format
    // and rejected here, because accepting it widens what this claims without a
    // volume to test it against.
    const num_fats = sector[BPB_NUM_FATS];
    if (num_fats != 1 and num_fats != 2) return false;

    // Media descriptor: 0xF0, or 0xF8..0xFF. The same byte repeats as the first
    // FAT entry, which is what makes it worth checking at all.
    const media = sector[BPB_MEDIA];
    if (media != 0xF0 and media < 0xF8) return false;

    out.fs_type = @intCast(abi.FS_TYPE_FAT);

    // FAT32 is the volume that has moved both the FAT size and the root
    // directory out of the fixed BPB, which is exactly where its extended boot
    // record differs. Testing both fields rather than either keeps a corrupt
    // FAT16 from being read with FAT32 offsets.
    const fat32 = rd16(sector, BPB_FAT_SIZE_16) == 0 and rd16(sector, BPB_ROOT_ENTRIES) == 0;
    const serial_off = if (fat32) EBR32_SERIAL else EBR16_SERIAL;
    const label_off = if (fat32) EBR32_LABEL else EBR16_LABEL;

    r.setLabel(out, sector[label_off .. label_off + LABEL_BYTES]);
    // The volume serial is FAT's whole notion of identity: four bytes, no
    // format-level guarantee of uniqueness, and the only thing a rule can match
    // when a volume carries no label.
    r.setUuid(out, sector[serial_off .. serial_off + 4]);
    return true;
}
