const std = @import("std");
const wasmos = @import("wasmos.zig");
const coroutine = wasmos.coroutine;
const Future = coroutine.Future;
const AsyncFsOp = coroutine.AsyncFsOp;
const O_RDONLY = wasmos.O_RDONLY;
const O_WRONLY = wasmos.O_WRONLY;
const O_CREAT = wasmos.O_CREAT;
const O_TRUNC = wasmos.O_TRUNC;

const path = "zig-long-file-check.txt";
const content = "zig shim long filename\n";

// The promise callbacks are plain function pointers and therefore cannot
// capture, so the small amount of state threaded through the chain (the active
// fd and the read scratch buffers) lives in these app-owned globals.
var app_fd: i32 = -1;
var startup_buf: [1]u8 = undefined;
var check_buf: [32]u8 = undefined;

fn failStartup() ?*Future {
    _ = wasmos.stdlib.println("startup.nsh readable: false", .{}) catch {};
    return null;
}

fn failWrite() ?*Future {
    _ = wasmos.stdlib.println("long filename write: false", .{}) catch {};
    return null;
}

fn failUnlink() ?*Future {
    _ = wasmos.stdlib.println("long filename unlink: false", .{}) catch {};
    return null;
}

fn appStart() ?*Future {
    const op = coroutine.openAsync("/boot/startup.nsh", O_RDONLY) orelse return failStartup();
    return op.then(startupOpened);
}

fn startupOpened(open: *AsyncFsOp) ?*Future {
    const fd = open.result();
    if (fd < 0) return failStartup();
    app_fd = fd;
    const op = coroutine.readAsync(fd, startup_buf[0..]) orelse return failStartup();
    return op.then(startupRead);
}

fn startupRead(read: *AsyncFsOp) ?*Future {
    if (read.result() != 1) return failStartup();
    const op = coroutine.closeAsync(app_fd) orelse return failStartup();
    return op.then(startupClosed);
}

fn startupClosed(close: *AsyncFsOp) ?*Future {
    if (close.result() < 0) return failStartup();
    _ = wasmos.stdlib.println("startup.nsh readable: true", .{}) catch {};
    const op = coroutine.openAsync(path, O_WRONLY | O_CREAT | O_TRUNC) orelse return failWrite();
    return op.then(fileCreated);
}

fn fileCreated(create: *AsyncFsOp) ?*Future {
    const fd = create.result();
    if (fd < 0) return failWrite();
    app_fd = fd;
    const op = coroutine.writeAsync(fd, content) orelse return failWrite();
    return op.then(fileWritten);
}

fn fileWritten(write: *AsyncFsOp) ?*Future {
    if (write.result() != @as(i32, @intCast(content.len))) return failWrite();
    const op = coroutine.closeAsync(app_fd) orelse return failWrite();
    return op.then(writeClosed);
}

fn writeClosed(close: *AsyncFsOp) ?*Future {
    if (close.result() < 0) return failWrite();
    const op = coroutine.openAsync(path, O_RDONLY) orelse return failWrite();
    return op.then(verifyOpened);
}

fn verifyOpened(open: *AsyncFsOp) ?*Future {
    const fd = open.result();
    if (fd < 0) return failWrite();
    app_fd = fd;
    const op = coroutine.readAsync(fd, check_buf[0..]) orelse return failWrite();
    return op.then(verifyRead);
}

fn verifyRead(read: *AsyncFsOp) ?*Future {
    const matched = read.result() == @as(i32, @intCast(content.len)) and
        std.mem.eql(u8, check_buf[0..content.len], content);
    if (!matched) return failWrite();
    const op = coroutine.closeAsync(app_fd) orelse return failWrite();
    return op.then(verifyClosed);
}

fn verifyClosed(close: *AsyncFsOp) ?*Future {
    if (close.result() < 0) return failWrite();
    const op = coroutine.unlinkAsync(path) orelse return failUnlink();
    return op.then(fileUnlinked);
}

fn fileUnlinked(unlink: *AsyncFsOp) ?*Future {
    if (unlink.result() < 0) return failUnlink();
    // A stat of the just-unlinked path is expected to reject; catch converts
    // that rejection into the success report.
    const op = coroutine.statAsync(path) orelse return failUnlink();
    return op.catchReject(statRejected);
}

fn statRejected(status: i32) i32 {
    _ = status;
    _ = wasmos.stdlib.println("long filename write: true", .{}) catch {};
    _ = wasmos.stdlib.println("long filename unlink: true", .{}) catch {};
    return 0;
}

pub fn main() u8 {
    _ = wasmos.stdlib.println("Hello from Zig on WASMOS!", .{}) catch {};
    _ = wasmos.stdlib.println("This is a tiny WASMOS-APP written in Zig.", .{}) catch {};
    _ = wasmos.stdlib.printf("Entry: {s}\n", .{"main"}) catch {};
    _ = coroutine.runAsyncApp(appStart);
    return 0;
}
