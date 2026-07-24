const std = @import("std");
const wasmos = @import("wasmos.zig");
var printed: bool = false;
const TaskState = struct { phase: u32 = 0 };
fn resumeTask(user: ?*anyopaque, out: *usize) callconv(.c) i32 {
    const state: *TaskState = @ptrCast(@alignCast(user.?));
    if (state.phase == 0) {
        state.phase = 1;
        return wasmos.coroutine.TaskResult.yielded;
    }
    out.* = 42;
    return wasmos.coroutine.TaskResult.complete;
}
pub fn main() u8 {
    if (!printed) {
        const path = "zig-long-file-check.txt";
        const content = "zig shim long filename\n";
        var long_file_ok = false;
        var file = wasmos.fs.openRead("/boot/startup.nsh") catch |err| {
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
        const unlink_ok = if (long_file_ok) blk: {
            wasmos.fs.unlink(path) catch break :blk false;
            _ = wasmos.fs.stat(path) catch break :blk true;
            break :blk false;
        } else false;
        var buffer: [96]u8 = undefined;
        const count = file.read(buffer[0..]) catch 0;
        const readable = std.mem.indexOf(u8, buffer[0..count], "BOOTX64.EFI") != null;
        var runtime: wasmos.coroutine.Runtime = .{};
        var coroutine: wasmos.coroutine.Coroutine = .{};
        var state: TaskState = .{};
        runtime.init();
        const coroutine_ok = coroutine.start(&runtime, resumeTask, &state) != null and
            runtime.run() >= 0 and
            if (coroutine.completion.poll()) |result| switch (result) {
                .ready => |value| value == 42,
                else => false,
            } else false;
        printed = true;
        _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
        _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
        _ = wasmos.stdlib.printf("Entry: {s}\n", .{"main"}) catch {};
        _ = wasmos.stdlib.println("startup.nsh readable: {}", .{readable}) catch {};
        _ = wasmos.stdlib.println("long filename write: {}", .{long_file_ok}) catch {};
        _ = wasmos.stdlib.println("long filename unlink: {}", .{unlink_ok}) catch {};
        if (coroutine_ok) {
            _ = wasmos.stdlib.println("coroutine task: ready", .{}) catch {};
        } else {
            _ = wasmos.stdlib.println("coroutine task: failed", .{}) catch {};
        }
    }
    return 0;
}
