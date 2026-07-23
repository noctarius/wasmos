#![allow(dead_code)]

extern crate core;

#[path = "../../src/libc/rust/coroutine.rs"]
mod coroutine;

use core::ffi::c_void;
use coroutine::{AwaitResult, Continuation, Coroutine, Future, FutureGroup, Promise, Runtime, TaskResult};

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
    let mut waiter_state = WaiterState { pc: 0, future: &mut future, status: None };
    assert!(waiter_coro.start(&mut runtime, waiter, (&mut waiter_state as *mut WaiterState).cast()).is_some());
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
    let child = source.then(&mut runtime, &mut continuation, Some(increment), None,
                            core::ptr::null_mut()).unwrap() as *mut Future;
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
    assert!(group.race(&mut runtime, &mut inputs, &mut continuations).is_some());
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
    let all = all_group.all(&mut runtime, &mut all_inputs, &mut values, &mut all_continuations)
        .unwrap() as *mut Future;
    assert!(all_second_promise.resolve(2));
    assert!(all_first_promise.resolve(1));
    assert_eq!(runtime.run(), Ok(0));
    assert_eq!(unsafe { (&*all).poll() }, Some(Ok(values.as_ptr() as usize)));
    assert_eq!(values, [1, 2]);
}
