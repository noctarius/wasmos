const std = @import("std");
const libsys = @import("libsys");

const AsyncState = struct {
    future: *libsys.Future,
    await_status: i32 = -999,
    await_value: usize = 0,
};

fn asyncAwaitEntry(arg: ?*anyopaque) callconv(.c) void {
    const state: *AsyncState = @ptrCast(@alignCast(arg.?));
    state.await_status = libsys.futureAwait(state.future, &state.await_value);
}

fn increment(_: ?*anyopaque, value: usize, out_value: [*c]usize) callconv(.c) i32 {
    out_value[0] = value + 1;
    return 0;
}

test "native coroutine Zig wrapper starts and completes an async task" {
    var runtime: libsys.NativeCoroutineRuntime = .{};
    var source: libsys.Future = undefined;
    var promise: libsys.Promise = undefined;
    var coroutine: libsys.NativeCoroutine = .{};
    var stack: [4096]u8 align(16) = undefined;
    var state = AsyncState{ .future = &source };
    var status: i32 = -1;
    var value: usize = 0;

    libsys.coroutineRuntimeInit(&runtime);
    libsys.futureInit(&source, &promise);
    const completion = libsys.asyncStart(&runtime, &coroutine, &stack, asyncAwaitEntry, &state);
    try std.testing.expect(completion == &coroutine.completion);
    try std.testing.expectEqual(@as(i32, 1), libsys.coroutineRun(&runtime));
    try std.testing.expect(libsys.promiseResolve(&promise, 42));
    try std.testing.expectEqual(@as(i32, 1), libsys.coroutineRun(&runtime));
    try std.testing.expectEqual(@as(i32, 0), state.await_status);
    try std.testing.expectEqual(@as(usize, 42), state.await_value);
    try std.testing.expect(libsys.futurePoll(completion.?, &status, &value));
    try std.testing.expectEqual(@as(i32, 0), status);
    try std.testing.expectEqual(@as(usize, 0), value);
}

test "native coroutine Zig wrapper chains futures" {
    var runtime: libsys.NativeCoroutineRuntime = .{};
    var source: libsys.Future = undefined;
    var promise: libsys.Promise = undefined;
    var continuation: libsys.FutureContinuation = .{};
    var status: i32 = -1;
    var value: usize = 0;

    libsys.coroutineRuntimeInit(&runtime);
    libsys.futureInit(&source, &promise);
    const child = libsys.futureThen(&runtime, &source, &continuation, increment, null, null);
    try std.testing.expect(child != null);
    try std.testing.expect(libsys.promiseResolve(&promise, 41));
    try std.testing.expectEqual(@as(i32, 0), libsys.coroutineRun(&runtime));
    try std.testing.expect(libsys.futurePoll(child.?, &status, &value));
    try std.testing.expectEqual(@as(i32, 0), status);
    try std.testing.expectEqual(@as(usize, 42), value);
}

test "native coroutine Zig wrapper races slice inputs" {
    var runtime: libsys.NativeCoroutineRuntime = .{};
    var first: libsys.Future = undefined;
    var first_promise: libsys.Promise = undefined;
    var second: libsys.Future = undefined;
    var second_promise: libsys.Promise = undefined;
    var group: libsys.FutureGroup = .{};
    var continuations: [2]libsys.FutureContinuation = .{ .{}, .{} };
    const inputs = [_]*libsys.Future{ &first, &second };
    var status: i32 = -1;
    var value: usize = 0;

    libsys.coroutineRuntimeInit(&runtime);
    libsys.futureInit(&first, &first_promise);
    libsys.futureInit(&second, &second_promise);
    const result = libsys.futureRace(&runtime, &group, &inputs, &continuations);
    try std.testing.expect(result != null);
    try std.testing.expect(libsys.promiseResolve(&second_promise, 22));
    try std.testing.expectEqual(@as(i32, 0), libsys.coroutineRun(&runtime));
    try std.testing.expect(libsys.futurePoll(result.?, &status, &value));
    try std.testing.expectEqual(@as(i32, 0), status);
    try std.testing.expectEqual(@as(usize, 22), value);
    try std.testing.expect(libsys.promiseResolve(&first_promise, 11));
    try std.testing.expectEqual(@as(i32, 0), libsys.coroutineRun(&runtime));
    try std.testing.expect(!group.active);
}

test "native coroutine Zig wrapper collects ordered all values" {
    var runtime: libsys.NativeCoroutineRuntime = .{};
    var first: libsys.Future = undefined;
    var first_promise: libsys.Promise = undefined;
    var second: libsys.Future = undefined;
    var second_promise: libsys.Promise = undefined;
    var group: libsys.FutureGroup = .{};
    var continuations: [2]libsys.FutureContinuation = .{ .{}, .{} };
    const inputs = [_]*libsys.Future{ &first, &second };
    var values = [_]usize{ 0, 0 };
    var status: i32 = -1;
    var value: usize = 0;

    libsys.coroutineRuntimeInit(&runtime);
    libsys.futureInit(&first, &first_promise);
    libsys.futureInit(&second, &second_promise);
    const result = libsys.futureAll(&runtime, &group, &inputs, &values, &continuations);
    try std.testing.expect(result != null);
    try std.testing.expect(libsys.promiseResolve(&second_promise, 2));
    try std.testing.expect(libsys.promiseResolve(&first_promise, 1));
    try std.testing.expectEqual(@as(i32, 0), libsys.coroutineRun(&runtime));
    try std.testing.expect(libsys.futurePoll(result.?, &status, &value));
    try std.testing.expectEqual(@as(i32, 0), status);
    try std.testing.expectEqual(@intFromPtr(&values), value);
    try std.testing.expectEqualSlices(usize, &.{ 1, 2 }, &values);
    try std.testing.expect(!group.active);
}

test "native coroutine Zig wrapper rejects empty and mismatched slices" {
    var runtime: libsys.NativeCoroutineRuntime = .{};
    var future: libsys.Future = undefined;
    var promise: libsys.Promise = undefined;
    var group: libsys.FutureGroup = .{};
    var empty_inputs: [0]*libsys.Future = .{};
    var empty_continuations: [0]libsys.FutureContinuation = .{};
    var one_input = [_]*libsys.Future{&future};
    var one_value = [_]usize{0};
    var one_continuation = [_]libsys.FutureContinuation{.{}};

    libsys.coroutineRuntimeInit(&runtime);
    libsys.futureInit(&future, &promise);
    try std.testing.expect(libsys.futureRace(&runtime, &group, &empty_inputs, &empty_continuations) == null);
    try std.testing.expect(libsys.futureAll(&runtime, &group, &empty_inputs, &.{}, &empty_continuations) == null);
    try std.testing.expect(libsys.futureRace(&runtime, &group, &one_input, &empty_continuations) == null);
    try std.testing.expect(libsys.futureAll(&runtime, &group, &one_input, &.{}, &one_continuation) == null);
    try std.testing.expect(libsys.futureAll(&runtime, &group, &one_input, &one_value, &empty_continuations) == null);
}

test "native coroutine Zig wrapper initializes an IPC future" {
    var operation: libsys.NativeIpcFuture = undefined;

    libsys.ipcFutureInit(&operation, null, null);
    try std.testing.expect(!libsys.futurePoll(&operation.future, null, null));
}

test "native coroutine Zig wrapper initializes a service runtime" {
    var service: libsys.NativeService = undefined;
    var stack: [4096]u8 align(16) = undefined;

    libsys.serviceInit(&service, &stack);
    try std.testing.expect(service.root_stack != null);
}
