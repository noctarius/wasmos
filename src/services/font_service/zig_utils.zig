pub fn packName16(name: []const u8, out: *[4]u32) void {
    out.* = .{ 0, 0, 0, 0 };
    var i: usize = 0;
    while (i < name.len and i < 16) : (i += 1) {
        const slot: usize = i / 4;
        const shift: u5 = @intCast((i % 4) * 8);
        out[slot] |= (@as(u32, name[i]) << shift);
    }
}

pub fn byteCopy(dst: [*]u8, src: [*]const u8, len: usize) void {
    var i: usize = 0;
    while (i < len) : (i += 1) {
        dst[i] = src[i];
    }
}

pub fn beU16(data: []const u8, off: usize) ?u16 {
    if (off + 2 > data.len) return null;
    return (@as(u16, data[off]) << 8) | @as(u16, data[off + 1]);
}

pub fn beI16(data: []const u8, off: usize) ?i16 {
    return @bitCast(beU16(data, off) orelse return null);
}

pub fn beU32(data: []const u8, off: usize) ?u32 {
    if (off + 4 > data.len) return null;
    return (@as(u32, data[off]) << 24) |
        (@as(u32, data[off + 1]) << 16) |
        (@as(u32, data[off + 2]) << 8) |
        @as(u32, data[off + 3]);
}

pub fn findTable(data: []const u8, tag: [4]u8) ?usize {
    const num_tables = beU16(data, 4) orelse return null;
    var i: usize = 0;
    while (i < num_tables) : (i += 1) {
        const rec = 12 + i * 16;
        if (rec + 16 > data.len) return null;
        if (data[rec] == tag[0] and data[rec + 1] == tag[1] and data[rec + 2] == tag[2] and data[rec + 3] == tag[3]) {
            const offset = beU32(data, rec + 8) orelse return null;
            return @intCast(offset);
        }
    }
    return null;
}

pub fn packU16Pair(a: u32, b: u32) u32 {
    const a16: u16 = @intCast(a & 0xFFFF);
    const b16: u16 = @intCast(b & 0xFFFF);
    return @as(u32, a16) | (@as(u32, b16) << 16);
}

pub fn packS16Pair(a: i32, b: i32) u32 {
    const a16: u16 = @bitCast(@as(i16, @truncate(a)));
    const b16: u16 = @bitCast(@as(i16, @truncate(b)));
    return @as(u32, a16) | (@as(u32, b16) << 16);
}
