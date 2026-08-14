//! Rust binding tests for the stackless WASM coroutine/future core.
//!
//! The cases drive src/libc/rust/coroutine.rs -- pulled in by path rather than
//! as a crate -- against src/libsys/wasm/coroutine_wasm.c compiled for the build
//! host, so the core is the same C implementation every other language suite
//! runs and what is unique here is the `#[repr(C)]` binding.
//!
//! The harness must run single-threaded (`--test-threads=1`). coroutine_wasm.c
//! keeps the runtime that is currently resuming a task in a file-global, which
//! is sound for a single-threaded guest but not for libtest's default parallel
//! cases: two of them racing on it park a waiter against another case's runtime,
//! which then never wakes.
#![allow(dead_code)]

extern crate core;

#[path = "../../src/libc/rust/coroutine.rs"]
mod coroutine;

use core::ffi::c_void;
use coroutine::{
    AwaitResult, Continuation, Coroutine, Future, FutureGroup, Promise, Runtime, TaskResult,
};

struct WaiterState {
    pc: i32,
    future: *mut Future,
    status: Option<AwaitResult>,
}

unsafe extern "C" fn waiter(user: *mut c_void, out: *mut usize) -> i32 {
    let state = unsafe { &mut *(user as *mut WaiterState) };
    if state.pc == 0 {
        state.pc = 1;
        state.status = Some(unsafe { (&mut *state.future).await_value() });
        if state.status == Some(AwaitResult::Pending) {
            return TaskResult::YIELDED;
        }
    }
    state.status = Some(unsafe { (&mut *state.future).await_value() });
    match state.status {
        Some(AwaitResult::Ready { value }) => {
            unsafe { *out = value };
            TaskResult::COMPLETE
        }
        Some(AwaitResult::Failed { status }) => status,
        _ => -1,
    }
}

unsafe extern "C" fn increment(_user: *mut c_void, value: usize, out: *mut usize) -> i32 {
    unsafe { *out = value + 1 };
    0
}

#[test]
fn futures_promises_and_object_methods() {
    let mut runtime = Runtime::new();
    runtime.init();
    let mut future = Future::new();
    let mut promise = Promise::new();
    future.init(&mut promise);
    let mut waiter_coro = Coroutine::new();
    let mut waiter_state = WaiterState {
        pc: 0,
        future: &mut future,
        status: None,
    };
    assert!(waiter_coro
        .start(
            &mut runtime,
            waiter,
            (&mut waiter_state as *mut WaiterState).cast()
        )
        .is_some());
    assert_eq!(runtime.run(), Ok(1));
    assert_eq!(waiter_state.status, Some(AwaitResult::Pending));
    assert!(promise.resolve(42));
    assert_eq!(runtime.run(), Ok(1));
    assert_eq!(waiter_state.status, Some(AwaitResult::Ready { value: 42 }));
    assert_eq!(waiter_coro.join(), Ok(42));

    let mut source = Future::new();
    let mut source_promise = Promise::new();
    source.init(&mut source_promise);
    let mut continuation = Continuation::new();
    let child = source
        .then(
            &mut runtime,
            &mut continuation,
            Some(increment),
            None,
            core::ptr::null_mut(),
        )
        .unwrap() as *mut Future;
    assert!(source_promise.resolve(20));
    assert_eq!(unsafe { (&*child).poll() }, None);
    assert_eq!(runtime.run(), Ok(0));
    assert_eq!(unsafe { (&*child).poll() }, Some(Ok(21)));

    let mut first = Future::new();
    let mut first_promise = Promise::new();
    first.init(&mut first_promise);
    let mut second = Future::new();
    let mut second_promise = Promise::new();
    second.init(&mut second_promise);
    let mut group = FutureGroup::new();
    let mut continuations = [Continuation::new(), Continuation::new()];
    let mut inputs = [&mut first, &mut second];
    assert!(group
        .race(&mut runtime, &mut inputs, &mut continuations)
        .is_some());
    assert!(second_promise.resolve(7));
    assert!(first_promise.reject(-2));
    assert_eq!(runtime.run(), Ok(0));

    let mut all_first = Future::new();
    let mut all_first_promise = Promise::new();
    all_first.init(&mut all_first_promise);
    let mut all_second = Future::new();
    let mut all_second_promise = Promise::new();
    all_second.init(&mut all_second_promise);
    let mut all_group = FutureGroup::new();
    let mut all_continuations = [Continuation::new(), Continuation::new()];
    let mut all_inputs = [&mut all_first, &mut all_second];
    let mut values = [0usize; 2];
    let all = all_group
        .all(
            &mut runtime,
            &mut all_inputs,
            &mut values,
            &mut all_continuations,
        )
        .unwrap() as *mut Future;
    assert!(all_second_promise.resolve(2));
    assert!(all_first_promise.resolve(1));
    assert_eq!(runtime.run(), Ok(0));
    assert_eq!(
        unsafe { (&*all).poll() },
        Some(Ok(values.as_ptr() as usize))
    );
    assert_eq!(values, [1, 2]);
}

// ---------------------------------------------------------------------------
// FFI smoke, then the full battery. Mirrors tests/unit/test_wasm_coroutine.c;
// this suite links the same coroutine_wasm.c, so what it uniquely checks is the
// BINDING: that #[repr(C)] field order and types agree with the C layout.
// ---------------------------------------------------------------------------

/// Values written by C must be read back through the Rust field declarations.
/// A size mismatch is impossible here (repr(C) is computed from the fields),
/// but a wrong field ORDER or type would show up as a garbled read.
#[test]
fn ffi_layout_round_trips_through_the_binding() {
    let mut runtime = Runtime::new();
    runtime.init();
    let mut future = Future::new();
    let mut promise = Promise::new();
    future.init(&mut promise);

    assert_eq!(future.poll(), None, "a pending future reports nothing");
    assert!(promise.resolve(0xABCD_1234));
    assert_eq!(
        future.poll(),
        Some(Ok(0xABCD_1234)),
        "the value C stored is read back through the Rust fields"
    );
    assert!(
        !promise.resolve(1),
        "a settled promise refuses to settle again"
    );

    let mut rejected = Future::new();
    let mut rejected_promise = Promise::new();
    rejected.init(&mut rejected_promise);
    assert!(
        !rejected_promise.reject(0),
        "rejection needs a negative status"
    );
    assert!(rejected_promise.reject(-77));
    assert_eq!(rejected.poll(), Some(Err(-77)));
    assert_eq!(runtime.run(), Ok(0), "an empty runtime drains to zero");
}

struct CallbackState {
    calls: u32,
}

unsafe extern "C" fn counting_increment(user: *mut c_void, value: usize, out: *mut usize) -> i32 {
    let state = unsafe { &mut *(user as *mut CallbackState) };
    state.calls += 1;
    unsafe { *out = value + 1 };
    0
}

unsafe extern "C" fn recover(user: *mut c_void, status: i32, out: *mut usize) -> i32 {
    let state = unsafe { &mut *(user as *mut CallbackState) };
    state.calls += 1;
    if status >= 0 {
        return -1;
    }
    unsafe { *out = 55 };
    0
}

#[test]
fn callbacks_are_deferred_even_on_a_settled_future() {
    let mut runtime = Runtime::new();
    runtime.init();
    let mut state = CallbackState { calls: 0 };

    let mut source = Future::new();
    let mut promise = Promise::new();
    source.init(&mut promise);
    let mut continuation = Continuation::new();
    let child = source
        .then(
            &mut runtime,
            &mut continuation,
            Some(counting_increment),
            None,
            &mut state as *mut _ as *mut c_void,
        )
        .expect("then registers");
    assert!(promise.resolve(20));
    assert_eq!(state.calls, 0, "resolving does not run the callback");
    assert_eq!(runtime.run(), Ok(0));
    assert_eq!(state.calls, 1, "the runtime does");
    assert_eq!(unsafe { (*child).poll() }, Some(Ok(21)));

    // Recovery through the error callback settles the child SUCCESSFULLY.
    let mut rejected = Future::new();
    let mut rejected_promise = Promise::new();
    rejected.init(&mut rejected_promise);
    let mut recover_continuation = Continuation::new();
    let child = rejected
        .then(
            &mut runtime,
            &mut recover_continuation,
            None,
            Some(recover),
            &mut state as *mut _ as *mut c_void,
        )
        .expect("then registers");
    assert!(rejected_promise.reject(-23));
    assert!(!rejected_promise.reject(-24));
    assert_eq!(runtime.run(), Ok(0));
    assert_eq!(unsafe { (*child).poll() }, Some(Ok(55)));

    // Registering on an ALREADY-SETTLED future must still defer. Every other
    // case registers before settling, so this is the only one covering that
    // branch.
    let mut settled = Future::new();
    let mut settled_promise = Promise::new();
    settled.init(&mut settled_promise);
    assert!(settled_promise.resolve(70));
    state.calls = 0;
    let mut late = Continuation::new();
    let child = settled
        .then(
            &mut runtime,
            &mut late,
            Some(counting_increment),
            None,
            &mut state as *mut _ as *mut c_void,
        )
        .expect("then registers on a settled future");
    assert_eq!(state.calls, 0, "still deferred");
    assert_eq!(runtime.run(), Ok(0));
    assert_eq!(state.calls, 1);
    assert_eq!(unsafe { (*child).poll() }, Some(Ok(71)));
}

struct ManyWaiter {
    future: *mut Future,
    status: i32,
    value: usize,
}

unsafe extern "C" fn many_waiter(user: *mut c_void, out: *mut usize) -> i32 {
    let state = unsafe { &mut *(user as *mut ManyWaiter) };
    match unsafe { (&mut *state.future).await_value() } {
        AwaitResult::Pending => TaskResult::YIELDED,
        AwaitResult::Ready { value } => {
            state.status = 0;
            state.value = value;
            unsafe { *out = value };
            TaskResult::COMPLETE
        }
        AwaitResult::Failed { status } => {
            state.status = status;
            status
        }
        AwaitResult::Invalid => -1,
    }
}

/// The runtime splices a whole wait list at settle time; with a single waiter
/// that loop body runs once with next == NULL, so several waiters on one future
/// are what exercises it.
#[test]
fn one_settle_wakes_every_waiter() {
    for failing in 0..2 {
        let mut runtime = Runtime::new();
        runtime.init();
        let mut future = Future::new();
        let mut promise = Promise::new();
        future.init(&mut promise);

        const WAITERS: usize = 4;
        let mut states: Vec<ManyWaiter> = (0..WAITERS)
            .map(|_| ManyWaiter {
                future: &mut future,
                status: -1,
                value: 0,
            })
            .collect();
        let mut coroutines: Vec<Coroutine> = (0..WAITERS).map(|_| Coroutine::new()).collect();
        for i in 0..WAITERS {
            let user = &mut states[i] as *mut _ as *mut c_void;
            assert!(coroutines[i]
                .start(&mut runtime, many_waiter, user)
                .is_some());
        }
        assert_eq!(runtime.run(), Ok(WAITERS as i32), "all four park");
        if failing == 0 {
            assert!(promise.resolve(77));
        } else {
            assert!(promise.reject(-31));
        }
        assert_eq!(runtime.run(), Ok(WAITERS as i32), "all four wake");
        for state in &states {
            if failing == 0 {
                assert_eq!((state.status, state.value), (0, 77));
            } else {
                assert_eq!(state.status, -31);
            }
        }
    }
}

struct StressState {
    remaining: u32,
    completed: *mut u32,
}

unsafe extern "C" fn stress(user: *mut c_void, out: *mut usize) -> i32 {
    let state = unsafe { &mut *(user as *mut StressState) };
    if state.remaining > 0 {
        state.remaining -= 1;
        return TaskResult::YIELDED;
    }
    unsafe { *state.completed += 1 };
    unsafe { *out = 0 };
    TaskResult::COMPLETE
}

/// The exact resume count is the schedule, so starving or double-scheduling
/// anyone changes it.
#[test]
fn round_robin_is_fair_at_scale() {
    const COUNT: usize = 8;
    const YIELDS: u32 = 4;
    let mut runtime = Runtime::new();
    runtime.init();
    let mut completed: u32 = 0;
    let mut states: Vec<StressState> = (0..COUNT)
        .map(|_| StressState {
            remaining: YIELDS,
            completed: &mut completed,
        })
        .collect();
    let mut coroutines: Vec<Coroutine> = (0..COUNT).map(|_| Coroutine::new()).collect();
    for i in 0..COUNT {
        let user = &mut states[i] as *mut _ as *mut c_void;
        assert!(coroutines[i].start(&mut runtime, stress, user).is_some());
    }
    assert_eq!(runtime.run(), Ok((COUNT as i32) * (YIELDS as i32 + 1)));
    assert_eq!(completed, COUNT as u32);
}

/// Race with three candidates that all settle, once per winning position. The
/// losers must be DISCARDED, which `completed == 1` is what actually proves:
/// a runtime that leaves them queued and lets them run looks identical
/// otherwise, since their resolve is refused by the settled group.
#[test]
fn race_first_to_settle_wins_from_any_position() {
    for winner in 0..3usize {
        let mut runtime = Runtime::new();
        runtime.init();
        let mut futures: Vec<Future> = (0..3).map(|_| Future::new()).collect();
        let mut promises: Vec<Promise> = (0..3).map(|_| Promise::new()).collect();
        for i in 0..3 {
            let promise: *mut Promise = &mut promises[i];
            futures[i].init(unsafe { &mut *promise });
        }
        let mut continuations: Vec<Continuation> = (0..3).map(|_| Continuation::new()).collect();
        let mut group = FutureGroup::new();
        let raw: Vec<*mut Future> = futures.iter_mut().map(|f| f as *mut Future).collect();
        let mut inputs: Vec<&mut Future> = raw.iter().map(|p| unsafe { &mut **p }).collect();
        let result = group
            .race(&mut runtime, &mut inputs, &mut continuations)
            .expect("race starts") as *mut Future;

        let expected = 100 + winner;
        assert!(promises[winner].resolve(expected));
        // The rest settle before the drain, so they are losers that were
        // already enqueued rather than merely pending.
        for i in (0..3).rev() {
            if i == winner {
                continue;
            }
            assert!(promises[i].resolve(900 + i));
        }
        assert_eq!(runtime.run(), Ok(0));
        assert_eq!(unsafe { (*result).poll() }, Some(Ok(expected)));
        assert!(group.settled());
        assert!(!group.active());
        assert_eq!(group.completed(), 1, "only the winner's callback ran");
        for future in futures.iter() {
            assert!(!future.has_continuations(), "losers were released");
        }
        assert_eq!(runtime.run(), Ok(0));
        assert_eq!(unsafe { (*result).poll() }, Some(Ok(expected)));
    }
}

/// Fail-fast all: whichever source rejects settles the group, and the rest are
/// released rather than left to run.
#[test]
fn all_fails_fast_from_any_position() {
    for rejecter in 0..3usize {
        let mut runtime = Runtime::new();
        runtime.init();
        let mut futures: Vec<Future> = (0..3).map(|_| Future::new()).collect();
        let mut promises: Vec<Promise> = (0..3).map(|_| Promise::new()).collect();
        for i in 0..3 {
            let promise: *mut Promise = &mut promises[i];
            futures[i].init(unsafe { &mut *promise });
        }
        let mut continuations: Vec<Continuation> = (0..3).map(|_| Continuation::new()).collect();
        let mut values: Vec<usize> = vec![0; 3];
        let mut group = FutureGroup::new();
        let raw: Vec<*mut Future> = futures.iter_mut().map(|f| f as *mut Future).collect();
        let mut inputs: Vec<&mut Future> = raw.iter().map(|p| unsafe { &mut **p }).collect();
        let result = group
            .all(&mut runtime, &mut inputs, &mut values, &mut continuations)
            .expect("all starts") as *mut Future;

        let expected = -60 - (rejecter as i32);
        assert!(promises[rejecter].reject(expected));
        assert_eq!(runtime.run(), Ok(0));
        assert_eq!(unsafe { (*result).poll() }, Some(Err(expected)));
        assert!(group.settled());
        assert!(!group.active());
        assert_eq!(group.completed(), 1, "only the rejecter's callback ran");
        for (i, future) in futures.iter().enumerate() {
            if i != rejecter {
                assert!(!future.has_continuations(), "survivors were released");
            }
        }
        // A survivor settling afterwards is inert.
        for i in 0..3 {
            if i == rejecter {
                continue;
            }
            assert!(promises[i].resolve(999));
        }
        assert_eq!(runtime.run(), Ok(0));
        assert_eq!(unsafe { (*result).poll() }, Some(Err(expected)));
    }
}
