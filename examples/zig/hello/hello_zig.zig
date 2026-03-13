const std = @import("std");
const wasmos = @import("wasmos.zig");

var printed: bool = false;

pub fn main() void {}

pub export fn wasmos_entry(
    ignored_arg0: i32,
    ignored_arg1: i32,
    ignored_arg2: i32,
    ignored_arg3: i32,
) callconv(.c) void {
    _ = ignored_arg0;
    _ = ignored_arg1;
    _ = ignored_arg2;
    _ = ignored_arg3;

    if (!printed) {
        var file = wasmos.fs.openRead("/startup.nsh") catch |err| {
            printed = true;
            _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
            _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
            _ = wasmos.stdlib.println("Entry: {s}", .{"main"}) catch {};
            _ = wasmos.stdlib.println("startup.nsh: {s}", .{@errorName(err)}) catch {};
            return;
        };
        defer file.close() catch {};

        var buffer: [96]u8 = undefined;
        const count = file.read(buffer[0..]) catch 0;
        const readable = std.mem.indexOf(u8, buffer[0..count], "BOOTX64.EFI") != null;

        printed = true;
        _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
        _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
        _ = wasmos.stdlib.println("Entry: {s}", .{"main"}) catch {};
        _ = wasmos.stdlib.println("startup.nsh readable: {}", .{readable}) catch {};
    }
}
