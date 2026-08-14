//! Zig binding tests for the stackless WASM coroutine/future core.
//!
//! `zig test` compiles src/libsys/wasm/coroutine_wasm.c for the build host and
//! links it against src/libc/zig/coroutine.zig, so the core exercised here is
//! the same C implementation every other language suite runs, executed as host
//! code rather than as wasm. What is unique is the BINDING: coroutine.zig
//! re-declares the C types as `extern struct`s and enums, and a wrong field
//! order, width or tag type surfaces only as a garbled value.
//!
//! The scenarios mirror tests/unit/test_wasm_coroutine.c.
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

// ---------------------------------------------------------------------------
// FFI smoke, then the full battery. Mirrors tests/unit/test_wasm_coroutine.c;
// this suite links the same coroutine_wasm.c, so what it uniquely checks is the
// BINDING: that the extern struct field order and types agree with C.
// ---------------------------------------------------------------------------

test "FFI layout round trips through the binding" {
    var runtime: coroutine.Runtime = .{};
    runtime.init();
    var future: coroutine.Future = .{};
    var promise: coroutine.Promise = .{};
    future.init(&promise);

    try std.testing.expect(future.poll() == null);
    try std.testing.expect(promise.resolve(0xABCD1234));
    // A value stored by C is read back through the Zig field declarations; a
    // wrong field order or type would garble it.
    switch (future.poll().?) {
        .ready => |value| try std.testing.expectEqual(@as(usize, 0xABCD1234), value),
        .failed => unreachable,
    }
    try std.testing.expect(!promise.resolve(1));

    var rejected: coroutine.Future = .{};
    var rejected_promise: coroutine.Promise = .{};
    rejected.init(&rejected_promise);
    try std.testing.expect(!rejected_promise.reject(0));
    try std.testing.expect(rejected_promise.reject(-77));
    switch (rejected.poll().?) {
        .ready => unreachable,
        .failed => |status| try std.testing.expectEqual(@as(i32, -77), status),
    }
    try std.testing.expectEqual(@as(i32, 0), runtime.run());
}

const CallbackState = struct { calls: u32 = 0 };

fn countingIncrement(user: ?*anyopaque, value: usize, out: *usize) callconv(.c) i32 {
    const state: *CallbackState = @ptrCast(@alignCast(user.?));
    state.calls += 1;
    out.* = value + 1;
    return 0;
}

fn recoverCallback(user: ?*anyopaque, status: i32, out: *usize) callconv(.c) i32 {
    const state: *CallbackState = @ptrCast(@alignCast(user.?));
    state.calls += 1;
    if (status >= 0) return -1;
    out.* = 55;
    return 0;
}

test "callbacks are deferred, even on an already-settled future" {
    var runtime: coroutine.Runtime = .{};
    runtime.init();
    var state: CallbackState = .{};

    var source: coroutine.Future = .{};
    var source_promise: coroutine.Promise = .{};
    source.init(&source_promise);
    var plus_one: coroutine.Continuation = .{};
    const child = source.then(&runtime, &plus_one, countingIncrement, null, &state).?;
    try std.testing.expect(source_promise.resolve(20));
    // Resolving does not run the callback; the runtime does.
    try std.testing.expectEqual(@as(u32, 0), state.calls);
    try std.testing.expectEqual(@as(i32, 0), runtime.run());
    try std.testing.expectEqual(@as(u32, 1), state.calls);
    switch (child.poll().?) {
        .ready => |value| try std.testing.expectEqual(@as(usize, 21), value),
        .failed => unreachable,
    }

    // The error handler recovers, so the child SUCCEEDS.
    var rejected: coroutine.Future = .{};
    var rejected_promise: coroutine.Promise = .{};
    rejected.init(&rejected_promise);
    var recover_continuation: coroutine.Continuation = .{};
    const recovered = rejected.then(&runtime, &recover_continuation, null, recoverCallback, &state).?;
    try std.testing.expect(rejected_promise.reject(-23));
    try std.testing.expect(!rejected_promise.reject(-24));
    try std.testing.expectEqual(@as(i32, 0), runtime.run());
    switch (recovered.poll().?) {
        .ready => |value| try std.testing.expectEqual(@as(usize, 55), value),
        .failed => unreachable,
    }

    // Registering on an ALREADY-SETTLED future must still defer. Every other
    // case registers before settling, so this is the only one covering that
    // branch.
    var settled: coroutine.Future = .{};
    var settled_promise: coroutine.Promise = .{};
    settled.init(&settled_promise);
    try std.testing.expect(settled_promise.resolve(70));
    state.calls = 0;
    var late: coroutine.Continuation = .{};
    const late_child = settled.then(&runtime, &late, countingIncrement, null, &state).?;
    try std.testing.expectEqual(@as(u32, 0), state.calls);
    try std.testing.expectEqual(@as(i32, 0), runtime.run());
    try std.testing.expectEqual(@as(u32, 1), state.calls);
    switch (late_child.poll().?) {
        .ready => |value| try std.testing.expectEqual(@as(usize, 71), value),
        .failed => unreachable,
    }
}

const ManyWaiter = struct {
    future: *coroutine.Future,
    status: i32 = -1,
    value: usize = 0,
};

fn manyWaiterResume(user: ?*anyopaque, out: *usize) callconv(.c) i32 {
    const state: *ManyWaiter = @ptrCast(@alignCast(user.?));
    switch (state.future.awaitValue()) {
        .pending => return coroutine.TaskResult.yielded,
        .ready => |value| {
            state.status = 0;
            state.value = value;
            out.* = value;
            return coroutine.TaskResult.complete;
        },
        .failed => |status| {
            state.status = status;
            return status;
        },
        .invalid => return -1,
    }
}

test "one settle wakes every waiter" {
    const waiters = 4;
    var failing: u32 = 0;
    while (failing < 2) : (failing += 1) {
        var runtime: coroutine.Runtime = .{};
        runtime.init();
        var future: coroutine.Future = .{};
        var promise: coroutine.Promise = .{};
        future.init(&promise);

        var states: [waiters]ManyWaiter = undefined;
        var coroutines: [waiters]coroutine.Coroutine = undefined;
        for (0..waiters) |i| {
            states[i] = .{ .future = &future };
            coroutines[i] = .{};
            try std.testing.expect(coroutines[i].start(&runtime, manyWaiterResume, &states[i]) != null);
        }
        // The wait list is spliced whole at settle time; with a single waiter
        // that loop runs once with next == null, so several waiters on one
        // future are what exercises it.
        try std.testing.expectEqual(@as(i32, waiters), runtime.run());
        if (failing == 0) {
            try std.testing.expect(promise.resolve(77));
        } else {
            try std.testing.expect(promise.reject(-31));
        }
        try std.testing.expectEqual(@as(i32, waiters), runtime.run());
        for (0..waiters) |i| {
            if (failing == 0) {
                try std.testing.expectEqual(@as(i32, 0), states[i].status);
                try std.testing.expectEqual(@as(usize, 77), states[i].value);
            } else {
                try std.testing.expectEqual(@as(i32, -31), states[i].status);
            }
        }
    }
}

const StressState = struct { remaining: u32, completed: *u32 };

fn stressResume(user: ?*anyopaque, out: *usize) callconv(.c) i32 {
    const state: *StressState = @ptrCast(@alignCast(user.?));
    if (state.remaining > 0) {
        state.remaining -= 1;
        return coroutine.TaskResult.yielded;
    }
    state.completed.* += 1;
    out.* = 0;
    return coroutine.TaskResult.complete;
}

test "round robin is fair at scale" {
    const count = 8;
    const yields = 4;
    var runtime: coroutine.Runtime = .{};
    runtime.init();
    var completed: u32 = 0;
    var states: [count]StressState = undefined;
    var coroutines: [count]coroutine.Coroutine = undefined;
    for (0..count) |i| {
        states[i] = .{ .remaining = yields, .completed = &completed };
        coroutines[i] = .{};
        try std.testing.expect(coroutines[i].start(&runtime, stressResume, &states[i]) != null);
    }
    // The exact resume count is the schedule.
    try std.testing.expectEqual(@as(i32, count * (yields + 1)), runtime.run());
    try std.testing.expectEqual(@as(u32, count), completed);
}

test "race: first to settle wins from any position, losers discarded" {
    for (0..3) |winner| {
        var runtime: coroutine.Runtime = .{};
        runtime.init();
        var futures: [3]coroutine.Future = .{ .{}, .{}, .{} };
        var promises: [3]coroutine.Promise = .{ .{}, .{}, .{} };
        for (0..3) |i| futures[i].init(&promises[i]);
        var continuations: [3]coroutine.Continuation = .{ .{}, .{}, .{} };
        const inputs = [_]*coroutine.Future{ &futures[0], &futures[1], &futures[2] };
        var group: coroutine.FutureGroup = .{};
        const result = group.race(&runtime, &inputs, &continuations).?;

        const expected: usize = 100 + winner;
        try std.testing.expect(promises[winner].resolve(expected));
        // The rest settle before the drain, so they are losers that were
        // already enqueued rather than merely pending.
        var i: usize = 3;
        while (i > 0) {
            i -= 1;
            if (i == winner) continue;
            try std.testing.expect(promises[i].resolve(900 + i));
        }
        try std.testing.expectEqual(@as(i32, 0), runtime.run());
        switch (result.poll().?) {
            .ready => |value| try std.testing.expectEqual(expected, value),
            .failed => unreachable,
        }
        try std.testing.expect(group.settled);
        try std.testing.expect(!group.active);
        // The losers were DISCARDED, not merely outvoted: exactly one group
        // callback ran. A runtime that leaves them queued and lets them run --
        // their resolve refused because the group already settled -- is
        // otherwise indistinguishable.
        try std.testing.expectEqual(@as(usize, 1), group.completed);
        for (0..3) |j| try std.testing.expect(futures[j].continuations == null);
        try std.testing.expectEqual(@as(i32, 0), runtime.run());
    }
}

test "all fails fast from any position, survivors released" {
    for (0..3) |rejecter| {
        var runtime: coroutine.Runtime = .{};
        runtime.init();
        var futures: [3]coroutine.Future = .{ .{}, .{}, .{} };
        var promises: [3]coroutine.Promise = .{ .{}, .{}, .{} };
        for (0..3) |i| futures[i].init(&promises[i]);
        var continuations: [3]coroutine.Continuation = .{ .{}, .{}, .{} };
        var values: [3]usize = .{ 0, 0, 0 };
        const inputs = [_]*coroutine.Future{ &futures[0], &futures[1], &futures[2] };
        var group: coroutine.FutureGroup = .{};
        const result = group.all(&runtime, &inputs, &values, &continuations).?;

        const expected: i32 = -60 - @as(i32, @intCast(rejecter));
        try std.testing.expect(promises[rejecter].reject(expected));
        try std.testing.expectEqual(@as(i32, 0), runtime.run());
        switch (result.poll().?) {
            .ready => unreachable,
            .failed => |status| try std.testing.expectEqual(expected, status),
        }
        try std.testing.expect(group.settled);
        try std.testing.expect(!group.active);
        try std.testing.expectEqual(@as(usize, 1), group.completed);
        for (0..3) |j| {
            if (j != rejecter) try std.testing.expect(futures[j].continuations == null);
        }
        // A survivor settling afterwards is inert.
        for (0..3) |j| {
            if (j == rejecter) continue;
            try std.testing.expect(promises[j].resolve(999));
        }
        try std.testing.expectEqual(@as(i32, 0), runtime.run());
        switch (result.poll().?) {
            .ready => unreachable,
            .failed => |status| try std.testing.expectEqual(expected, status),
        }
    }
}
