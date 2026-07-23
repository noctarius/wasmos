//! Object-oriented Rust bindings for the WASMOS stackless WASM coroutine core.
//!
//! Storage remains caller-owned. A task resume callback must record its own
//! state and return `TaskResult::YIELDED` after `Future::await_value()` returns
//! `AwaitResult::Pending`.

use core::ffi::c_void;

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FutureState {
    Pending = 0,
    Ready = 1,
    Failed = 2,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CoroutineState {
    New = 0,
    Ready = 1,
    Running = 2,
    Waiting = 3,
    Dead = 4,
}

pub struct TaskResult;
impl TaskResult {
    pub const COMPLETE: i32 = 0;
    pub const YIELDED: i32 = 1;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AwaitResult {
    Ready { value: usize },
    Failed { status: i32 },
    Pending,
    Invalid,
}

pub type TaskResume = unsafe extern "C" fn(*mut c_void, *mut usize) -> i32;
pub type SuccessCallback = unsafe extern "C" fn(*mut c_void, usize, *mut usize) -> i32;
pub type ErrorCallback = unsafe extern "C" fn(*mut c_void, i32, *mut usize) -> i32;

#[repr(C)]
pub struct Runtime {
    current: *mut Coroutine,
    ready_head: *mut Coroutine,
    ready_tail: *mut Coroutine,
    continuation_head: *mut Continuation,
    continuation_tail: *mut Continuation,
    running: bool,
}

#[repr(C)]
pub struct Future {
    state: FutureState,
    status: i32,
    value: usize,
    runtime: *mut Runtime,
    waiters: *mut Coroutine,
    continuations: *mut Continuation,
}

#[repr(C)]
pub struct Promise {
    future: *mut Future,
}

#[repr(C)]
pub struct Coroutine {
    runtime: *mut Runtime,
    next: *mut Coroutine,
    wait_next: *mut Coroutine,
    resume: Option<TaskResume>,
    user: *mut c_void,
    state: CoroutineState,
    result: i32,
    completion: Future,
    completion_promise: Promise,
}

#[repr(C)]
pub struct Continuation {
    next: *mut Continuation,
    future: *mut Future,
    on_success: Option<SuccessCallback>,
    on_error: Option<ErrorCallback>,
    user: *mut c_void,
    group: *mut FutureGroup,
    group_index: usize,
    child: Future,
    child_promise: Promise,
    active: bool,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FutureGroupKind {
    Race = 0,
    All = 1,
}

#[repr(C)]
pub struct FutureGroup {
    runtime: *mut Runtime,
    future: Future,
    promise: Promise,
    values: *mut usize,
    count: usize,
    completed: usize,
    kind: FutureGroupKind,
    settled: bool,
    active: bool,
}

unsafe extern "C" {
    fn wasmos_wasm_runtime_init(runtime: *mut Runtime);
    fn wasmos_async_start(
        runtime: *mut Runtime,
        coroutine: *mut Coroutine,
        resume: TaskResume,
        user: *mut c_void,
    ) -> *mut Future;
    fn wasmos_wasm_coroutine_run(runtime: *mut Runtime) -> i32;
    fn wasmos_wasm_coroutine_run_budget(runtime: *mut Runtime, budget: usize) -> i32;
    fn wasmos_wasm_coroutine_join(coroutine: *mut Coroutine, result: *mut i32) -> i32;
    fn wasmos_future_init(future: *mut Future, promise: *mut Promise);
    fn wasmos_future_poll(future: *const Future, status: *mut i32, value: *mut usize) -> bool;
    fn wasmos_future_await(future: *mut Future, value: *mut usize) -> i32;
    fn wasmos_promise_resolve(promise: *mut Promise, value: usize) -> bool;
    fn wasmos_promise_reject(promise: *mut Promise, status: i32) -> bool;
    fn wasmos_future_then(
        runtime: *mut Runtime,
        future: *mut Future,
        continuation: *mut Continuation,
        success: Option<SuccessCallback>,
        error: Option<ErrorCallback>,
        user: *mut c_void,
    ) -> *mut Future;
    fn wasmos_future_race(
        runtime: *mut Runtime,
        group: *mut FutureGroup,
        inputs: *const *mut Future,
        count: usize,
        continuations: *mut Continuation,
    ) -> *mut Future;
    fn wasmos_future_all(
        runtime: *mut Runtime,
        group: *mut FutureGroup,
        inputs: *const *mut Future,
        count: usize,
        values: *mut usize,
        continuations: *mut Continuation,
    ) -> *mut Future;
}

impl Runtime {
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    pub fn init(&mut self) {
        unsafe { wasmos_wasm_runtime_init(self) }
    }

    pub fn run(&mut self) -> Result<i32, ()> {
        let count = unsafe { wasmos_wasm_coroutine_run(self) };
        if count < 0 { Err(()) } else { Ok(count) }
    }

    pub fn run_budget(&mut self, budget: usize) -> Result<i32, ()> {
        let count = unsafe { wasmos_wasm_coroutine_run_budget(self, budget) };
        if count < 0 { Err(()) } else { Ok(count) }
    }
}

impl Future {
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    pub fn init(&mut self, promise: &mut Promise) {
        unsafe { wasmos_future_init(self, promise) }
    }

    pub fn poll(&self) -> Option<Result<usize, i32>> {
        let mut status = 0;
        let mut value = 0;
        if !unsafe { wasmos_future_poll(self, &mut status, &mut value) } {
            None
        } else if status == 0 {
            Some(Ok(value))
        } else {
            Some(Err(status))
        }
    }

    pub fn await_value(&mut self) -> AwaitResult {
        let mut value = 0;
        match unsafe { wasmos_future_await(self, &mut value) } {
            0 => AwaitResult::Ready { value },
            status if status < 0 => AwaitResult::Failed { status },
            1 => AwaitResult::Pending,
            _ => AwaitResult::Invalid,
        }
    }

    pub fn then(
        &mut self,
        runtime: &mut Runtime,
        continuation: &mut Continuation,
        success: Option<SuccessCallback>,
        error: Option<ErrorCallback>,
        user: *mut c_void,
    ) -> Option<&mut Future> {
        let child = unsafe { wasmos_future_then(runtime, self, continuation, success, error, user) };
        unsafe { child.as_mut() }
    }
}

impl Promise {
    pub const fn new() -> Self {
        Self { future: core::ptr::null_mut() }
    }

    pub fn resolve(&mut self, value: usize) -> bool {
        unsafe { wasmos_promise_resolve(self, value) }
    }

    pub fn reject(&mut self, status: i32) -> bool {
        status < 0 && unsafe { wasmos_promise_reject(self, status) }
    }
}

impl Coroutine {
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    pub fn start(
        &mut self,
        runtime: &mut Runtime,
        resume: TaskResume,
        user: *mut c_void,
    ) -> Option<&mut Future> {
        let completion = unsafe { wasmos_async_start(runtime, self, resume, user) };
        unsafe { completion.as_mut() }
    }

    pub fn join(&mut self) -> Result<i32, AwaitResult> {
        let mut result = 0;
        match unsafe { wasmos_wasm_coroutine_join(self, &mut result) } {
            0 => Ok(result),
            1 => Err(AwaitResult::Pending),
            status if status < 0 => Err(AwaitResult::Failed { status }),
            _ => Err(AwaitResult::Invalid),
        }
    }
}

impl Continuation {
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
}

impl FutureGroup {
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    pub fn race<'a>(
        &'a mut self,
        runtime: &mut Runtime,
        inputs: &mut [&mut Future],
        continuations: &mut [Continuation],
    ) -> Option<&'a mut Future> {
        if inputs.is_empty() || inputs.len() != continuations.len() { return None; }
        let result = unsafe {
            wasmos_future_race(runtime, self, inputs.as_mut_ptr() as *const *mut Future,
                                inputs.len(), continuations.as_mut_ptr())
        };
        unsafe { result.as_mut() }
    }

    pub fn all<'a>(
        &'a mut self,
        runtime: &mut Runtime,
        inputs: &mut [&mut Future],
        values: &mut [usize],
        continuations: &mut [Continuation],
    ) -> Option<&'a mut Future> {
        if inputs.is_empty() || inputs.len() != values.len() || inputs.len() != continuations.len() {
            return None;
        }
        let result = unsafe {
            wasmos_future_all(runtime, self, inputs.as_mut_ptr() as *const *mut Future,
                              inputs.len(), values.as_mut_ptr(), continuations.as_mut_ptr())
        };
        unsafe { result.as_mut() }
    }
}
