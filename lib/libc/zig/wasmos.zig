extern "wasmos" fn console_write(ptr: i32, len: i32) callconv(.c) i32;

pub fn putsn(bytes: []const u8) i32 {
    if (bytes.len == 0) {
        return 0;
    }
    return console_write(@intCast(@intFromPtr(bytes.ptr)), @intCast(bytes.len));
}
