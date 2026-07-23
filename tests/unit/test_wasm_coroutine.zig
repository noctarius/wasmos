const std = @import("std");
const coroutine = @import("coroutine");

const Waiter = struct {
    pc: i32 = 0,
    future: *coroutine.Future,
    value: usize = 0,
    status: i32 = -1,
};

fn waiterResume(user: ?*anyopaque, out: *usize) callconv(.c) i32 {
    const state: *Waiter = @ptrCast(@alignCast(user.?));
    if (state.pc == 0) {
        state.pc = 1;
        switch (state.future.awaitValue()) {
            .pending => return coroutine.TaskResult.yielded,
            .ready => |value| state.value = value,
            .failed => |status| return status,
            .invalid => return -1,
        }
    }
    switch (state.future.awaitValue()) {
        .pending => return coroutine.TaskResult.yielded,
        .ready => |value| {
            state.value = value;
            state.status = 0;
            out.* = value;
            return coroutine.TaskResult.complete;
        },
        .failed => |status| return status,
        .invalid => return -1,
    }
}

fn increment(_: ?*anyopaque, value: usize, out: *usize) callconv(.c) i32 {
    out.* = value + 1;
    return 0;
}

test "WASM Zig coroutine methods use the shared future core" {
    var runtime: coroutine.Runtime = .{};
    runtime.init();
    var future: coroutine.Future = .{};
    var promise: coroutine.Promise = .{};
    future.init(&promise);
    var task: coroutine.Coroutine = .{};
    var waiter = Waiter{ .future = &future };
    try std.testing.expect(task.start(&runtime, waiterResume, &waiter) != null);
    try std.testing.expectEqual(@as(i32, 1), runtime.run());
    try std.testing.expect(promise.resolve(42));
    try std.testing.expectEqual(@as(i32, 1), runtime.run());
    try std.testing.expectEqual(@as(usize, 42), waiter.value);
    var joined: i32 = 0;
    try std.testing.expectEqual(@as(i32, 0), task.join(&joined));
    try std.testing.expectEqual(@as(i32, 42), joined);

    var source: coroutine.Future = .{};
    var source_promise: coroutine.Promise = .{};
    source.init(&source_promise);
    var continuation: coroutine.Continuation = .{};
    const child = source.then(&runtime, &continuation, increment, null, null) orelse unreachable;
    try std.testing.expect(source_promise.resolve(20));
    try std.testing.expect(child.poll() == null);
    try std.testing.expectEqual(@as(i32, 0), runtime.run());
    switch (child.poll().?) {
        .ready => |value| try std.testing.expectEqual(@as(usize, 21), value),
        .failed => unreachable,
    }

    var first: coroutine.Future = .{};
    var first_promise: coroutine.Promise = .{};
    first.init(&first_promise);
    var second: coroutine.Future = .{};
    var second_promise: coroutine.Promise = .{};
    second.init(&second_promise);
    var group: coroutine.FutureGroup = .{};
    var continuations = [_]coroutine.Continuation{ .{}, .{} };
    const inputs = [_]*coroutine.Future{ &first, &second };
    try std.testing.expect(group.race(&runtime, &inputs, &continuations) != null);
    try std.testing.expect(second_promise.resolve(7));
    try std.testing.expect(first_promise.reject(-2));
    try std.testing.expectEqual(@as(i32, 0), runtime.run());
}
