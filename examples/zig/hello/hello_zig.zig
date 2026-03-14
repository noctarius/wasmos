const std = @import("std");
const wasmos = @import("wasmos.zig");

var printed: bool = false;

pub fn main() u8 {
    if (!printed) {
        const path = "/zig-long-file-check.txt";
        const content = "zig shim long filename\n";
        var long_file_ok = false;
        var file = wasmos.fs.openRead("/startup.nsh") catch |err| {
            printed = true;
            _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
            _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
            _ = wasmos.stdlib.printf("Entry: {s}\n", .{"main"}) catch {};
            _ = wasmos.stdlib.println("startup.nsh: {s}", .{@errorName(err)}) catch {};
            return 0;
        };
        defer file.close() catch {};

        var out = wasmos.fs.create(path) catch |err| {
            printed = true;
            _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
            _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
            _ = wasmos.stdlib.printf("Entry: {s}\n", .{"main"}) catch {};
            _ = wasmos.stdlib.println("long filename write: {s}", .{@errorName(err)}) catch {};
            return 0;
        };
        _ = out.write(content) catch 0;
        out.close() catch {};

        var verify = wasmos.fs.openRead(path) catch |err| {
            printed = true;
            _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
            _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
            _ = wasmos.stdlib.printf("Entry: {s}\n", .{"main"}) catch {};
            _ = wasmos.stdlib.println("long filename write: {s}", .{@errorName(err)}) catch {};
            return 0;
        };
        var verify_buf: [32]u8 = undefined;
        const verify_count = verify.read(verify_buf[0..]) catch 0;
        verify.close() catch {};
        long_file_ok = std.mem.eql(u8, verify_buf[0..verify_count], content);
        const unlink_ok = if (long_file_ok)
            blk: {
                wasmos.fs.unlink(path) catch break :blk false;
                _ = wasmos.fs.stat(path) catch break :blk true;
                break :blk false;
            }
        else
            false;

        var buffer: [96]u8 = undefined;
        const count = file.read(buffer[0..]) catch 0;
        const readable = std.mem.indexOf(u8, buffer[0..count], "BOOTX64.EFI") != null;

        printed = true;
        _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
        _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
        _ = wasmos.stdlib.printf("Entry: {s}\n", .{"main"}) catch {};
        _ = wasmos.stdlib.println("startup.nsh readable: {}", .{readable}) catch {};
        _ = wasmos.stdlib.println("long filename write: {}", .{long_file_ok}) catch {};
        _ = wasmos.stdlib.println("long filename unlink: {}", .{unlink_ok}) catch {};
    }
    return 0;
}
