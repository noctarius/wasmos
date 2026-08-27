//! Object-oriented Rust bindings for the WASMOS stackless WASM coroutine core.
//!
//! Storage remains caller-owned. A task resume callback must record its own
//! state and return `TaskResult::YIELDED` after `Future::await_value()` returns
//! `AwaitResult::Pending`.

use core::ffi::c_void;

/// Lifecycle of a future: pending until a promise settles it, then ready (status
/// 0, value meaningful) or failed (negative status). A settled future never
/// changes state again.
#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FutureState {
    Pending = 0,
    Ready = 1,
    Failed = 2,
}

/// Lifecycle of a coroutine: `New` before `start`, `Ready` while queued,
/// `Running` inside its resume callback, `Waiting` while parked on a future,
/// `Dead` once the callback returned anything other than `TaskResult::YIELDED`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CoroutineState {
    New = 0,
    Ready = 1,
    Running = 2,
    Waiting = 3,
    Dead = 4,
}

/// Return values of a `TaskResume` callback.
pub struct TaskResult;
impl TaskResult {
    /// The task finished; the value written to its out-parameter resolves its
    /// completion future.
    pub const COMPLETE: i32 = 0;
    /// The task wants to be resumed again -- from the ready queue, or when the
    /// future it parked on settles. Any other return is taken as a failure
    /// status and rejects the completion future, so it must be negative.
    pub const YIELDED: i32 = 1;
}

/// Outcome of `Future::await_value`. `Pending` means the calling task was parked
/// and must return `TaskResult::YIELDED` at once without touching its
/// out-parameter; `Invalid` is the runtime refusing the await (no running
/// coroutine, or a future owned by another runtime) and never parks.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AwaitResult {
    Ready { value: usize },
    Failed { status: i32 },
    Pending,
    Invalid,
}

/// A stackless task body: receives the `user` pointer given to
/// `Coroutine::start` plus an out-parameter for its completion value, and
/// returns a `TaskResult`. It must record its own progress in caller-owned
/// state, because no call stack survives a yield.
pub type TaskResume = unsafe extern "C" fn(*mut c_void, *mut usize) -> i32;
/// Continuation callback for a resolved future: receives the future's value and
/// writes the child's value. Returns 0 to resolve the child, a negative status
/// to reject it, or `FUTURE_CHAIN_NEXT` (from a flat registration only) with the
/// next future's address in the out-parameter.
pub type SuccessCallback = unsafe extern "C" fn(*mut c_void, usize, *mut usize) -> i32;
/// Continuation callback for a rejected future: receives the negative status and
/// returns the status to propagate, or 0 to convert the rejection into a
/// resolved child carrying the out-parameter's value.
pub type ErrorCallback = unsafe extern "C" fn(*mut c_void, i32, *mut usize) -> i32;

/// The cooperative scheduler: a queue of runnable coroutines and a queue of
/// continuations whose source future has settled. Caller-owned storage matching
/// `wasmos_wasm_runtime_t`; a guest normally has one, and nothing runs until
/// `run` or `run_budget` is called.
#[repr(C)]
pub struct Runtime {
    current: *mut Coroutine,
    ready_head: *mut Coroutine,
    ready_tail: *mut Coroutine,
    continuation_head: *mut Continuation,
    continuation_tail: *mut Continuation,
    running: bool,
}

/// The observing half of a one-shot result, settled exactly once by the
/// `Promise` bound to it. Caller-owned and reusable, but only through `init`,
/// which returns it to pending and drops the waiters and continuations
/// registered against the previous incarnation.
#[repr(C)]
pub struct Future {
    state: FutureState,
    status: i32,
    value: usize,
    runtime: *mut Runtime,
    waiters: *mut Coroutine,
    continuations: *mut Continuation,
}

/// The settling half of a future, bound by `Future::init`. Whoever holds it
/// decides the outcome, once.
#[repr(C)]
pub struct Promise {
    future: *mut Future,
}

/// One stackless task and its completion future. Caller-owned; `start`
/// overwrites the whole record, so it may be reused once the previous run is
/// dead.
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

/// Caller-owned registration record for one `then` callback, carrying the child
/// future that callback settles. It must stay live until the callback runs and
/// cannot be reused while a registration on it is live.
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

/// Settlement rule of a `FutureGroup`: `Race` settles on the first input to
/// settle either way, `All` on the first failure or on every input succeeding.
#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FutureGroupKind {
    Race = 0,
    All = 1,
}

/// Caller-owned storage combining several futures into one. The group, its
/// continuation slice and (for `all`) its value slice must stay live until the
/// group future settles; at that point the runtime unlinks the continuations
/// still registered on unsettled inputs, so they need not outlive the slowest
/// source.
#[repr(C)]
pub struct FutureGroup {
    runtime: *mut Runtime,
    future: Future,
    promise: Promise,
    continuations: *mut Continuation,
    values: *mut usize,
    count: usize,
    completed: usize,
    kind: FutureGroupKind,
    settled: bool,
    active: bool,
}

/// One IPC message in the layout the C event loop uses
/// (`wasmos_ipc_message_t`). Field order differs from `ipc::Reply` in
/// `wasmos.rs`: `source` and `destination` come last here.
#[repr(C)]
pub struct IpcMessage {
    pub type_: i32,
    pub request_id: i32,
    pub arg0: i32,
    pub arg1: i32,
    pub arg2: i32,
    pub arg3: i32,
    pub source: i32,
    pub destination: i32,
}

/// Protocol validator for a reply: returns 0 to resolve the `IpcFuture`, or a
/// negative status to reject it. Runs from the event loop while dispatching the
/// reply, before the future settles.
pub type IpcReplyStatus = unsafe extern "C" fn(*mut c_void, *const IpcMessage) -> i32;

/// The IPC demultiplexer: one receive endpoint, a table of in-flight requests
/// keyed by request id, and a table of per-message-type handlers. Caller-owned
/// storage matching `wasmos_sys_event_loop_t`; the two word arrays are opaque
/// storage for the C records.
#[repr(C)]
pub struct EventLoop {
    receiver_endpoint: i32,
    select_id: i32,
    next_request_id: i32,
    default_on_message: Option<unsafe extern "C" fn(*mut c_void, *const IpcMessage)>,
    default_user: *mut c_void,
    intents: [u32; 64],
    handlers: [u32; 64],
    /// Bound on how long `poll` parks with nothing queued, in milliseconds; 0
    /// parks until a message arrives. A positive value plus `on_timeout` is how
    /// a reactor holds a deadline without driving its own pump.
    poll_timeout_ms: i32,
    /// Runs when a bounded park elapses with nothing delivered. Called on the
    /// poller's stack, so it must not block.
    on_timeout: Option<unsafe extern "C" fn(*mut c_void)>,
    timeout_user: *mut c_void,
}

/// One in-flight request exposed as a future. Caller-owned and reusable: `init`
/// re-arms it, `send` refuses while the previous round trip is live, and the
/// reply is copied into the record before the future settles.
#[repr(C)]
pub struct IpcFuture {
    future: Future,
    promise: Promise,
    r#loop: *mut EventLoop,
    reply: IpcMessage,
    reply_status: Option<IpcReplyStatus>,
    user: *mut c_void,
    request_id: i32,
    active: u8,
    _padding: [u8; 3],
}

/// An `IpcFuture` pre-armed for the filesystem protocol: its future resolves
/// only on an `FS_IPC_RESP` reply and rejects anything else, error replies
/// included, so the backend's packed status is not surfaced.
#[repr(transparent)]
pub struct FsRequest(pub IpcFuture);

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
    fn wasmos_sys_event_loop_init(
        loop_: *mut EventLoop,
        receiver_endpoint: i32,
        request_id_base: i32,
    );
    fn wasmos_sys_event_loop_poll(loop_: *mut EventLoop, budget: i32) -> i32;
    fn wasmos_sys_wasm_ipc_future_init(
        operation: *mut IpcFuture,
        reply_status: Option<IpcReplyStatus>,
        user: *mut c_void,
    );
    fn wasmos_sys_wasm_ipc_future_send(
        loop_: *mut EventLoop,
        operation: *mut IpcFuture,
        destination: i32,
        source: i32,
        msg_type: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_ipc_future_cancel(operation: *mut IpcFuture, status: i32);
    fn wasmos_sys_wasm_ipc_future_reply(operation: *const IpcFuture) -> *const IpcMessage;
    fn wasmos_sys_wasm_fs_request_init(request: *mut FsRequest);
    fn wasmos_sys_wasm_fs_request_send(
        loop_: *mut EventLoop,
        request: *mut FsRequest,
        fs_endpoint: i32,
        reply_endpoint: i32,
        msg_type: i32,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_fs_request_reply(request: *const FsRequest) -> *const IpcMessage;
}

impl Runtime {
    /// A zeroed (idle) runtime, usable in a `static`.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    /// Zeroes the runtime. Any coroutine still queued on it is forgotten, so
    /// only init a runtime that is not being driven.
    pub fn init(&mut self) {
        unsafe { wasmos_wasm_runtime_init(self) }
    }

    /// Resumes ready coroutines and dispatches settled continuations until both
    /// queues are empty, returning the number of coroutine resumptions.
    /// `Err(())` means the call was re-entrant -- made from inside a task or a
    /// continuation. Coroutines parked on futures nothing settles are left
    /// parked rather than reported.
    pub fn run(&mut self) -> Result<i32, ()> {
        let count = unsafe { wasmos_wasm_coroutine_run(self) };
        if count < 0 {
            Err(())
        } else {
            Ok(count)
        }
    }

    /// As `run`, but resumes at most `budget` coroutines; queued continuations
    /// are still drained to exhaustion afterwards. A budget of 0 dispatches
    /// continuations only.
    pub fn run_budget(&mut self, budget: usize) -> Result<i32, ()> {
        let count = unsafe { wasmos_wasm_coroutine_run_budget(self, budget) };
        if count < 0 {
            Err(())
        } else {
            Ok(count)
        }
    }
}

impl Future {
    /// True while at least one continuation is still registered on this
    /// future. Used to check that a settled group released its losers.
    pub fn has_continuations(&self) -> bool {
        !self.continuations.is_null()
    }

    /// A zeroed (pending, unbound) future, usable in a `static`. It cannot
    /// settle until `init` binds a promise to it.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    /// Resets to pending and makes `promise` the only handle that can settle it.
    /// Any coroutine still parked on the previous incarnation is dropped and
    /// never woken.
    pub fn init(&mut self, promise: &mut Promise) {
        unsafe { wasmos_future_init(self, promise) }
    }

    /// Non-blocking observation: `None` while pending, otherwise `Ok(value)` or
    /// `Err(status)`. Does not park and does not consume the result -- a settled
    /// future keeps answering.
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

    /// Awaits from inside a running task. A settled future answers immediately;
    /// a pending one parks the calling coroutine and returns
    /// `AwaitResult::Pending`, on which the task MUST return
    /// `TaskResult::YIELDED` straight away -- it is resumed from the top when
    /// the future settles, so its own state machine has to re-enter at the same
    /// point. `Invalid` means nothing was parked: no coroutine is running, or
    /// the future belongs to another runtime.
    pub fn await_value(&mut self) -> AwaitResult {
        let mut value = 0;
        match unsafe { wasmos_future_await(self, &mut value) } {
            0 => AwaitResult::Ready { value },
            status if status < 0 => AwaitResult::Failed { status },
            1 => AwaitResult::Pending,
            _ => AwaitResult::Invalid,
        }
    }

    /// Registers `continuation` on this future and returns the child future that
    /// settles with the callback's result, or `None` when the continuation is
    /// already in use or the future belongs to another runtime.
    ///
    /// The callback never runs inline: it is queued on the runtime -- at once if
    /// this future has already settled -- and dispatched from `run`/`run_budget`.
    /// It runs at most once, and `continuation` and `user` must stay live until
    /// it does. With no matching callback the child inherits the source's
    /// outcome.
    pub fn then(
        &mut self,
        runtime: &mut Runtime,
        continuation: &mut Continuation,
        success: Option<SuccessCallback>,
        error: Option<ErrorCallback>,
        user: *mut c_void,
    ) -> Option<&mut Future> {
        let child =
            unsafe { wasmos_future_then(runtime, self, continuation, success, error, user) };
        unsafe { child.as_mut() }
    }
}

impl Promise {
    /// An unbound promise; `Future::init` binds it to a future.
    pub const fn new() -> Self {
        Self {
            future: core::ptr::null_mut(),
        }
    }

    /// Settles the bound future as ready with `value`, waking every parked
    /// waiter and queueing every registered continuation. False when there is no
    /// bound future or it has already settled.
    pub fn resolve(&mut self, value: usize) -> bool {
        unsafe { wasmos_promise_resolve(self, value) }
    }

    /// Settles the bound future as failed with `status`, which must be negative:
    /// a zero or positive status is refused and returns false, as does an
    /// already-settled future.
    pub fn reject(&mut self, status: i32) -> bool {
        status < 0 && unsafe { wasmos_promise_reject(self, status) }
    }
}

impl Coroutine {
    /// A zeroed coroutine in the `New` state, usable in a `static`.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    /// Schedules the task on `runtime` and returns its completion future, which
    /// resolves with the task's out-value or rejects with its failure status.
    /// `None` when the coroutine is neither new nor dead, so a live task cannot
    /// be restarted. Nothing runs until the runtime is driven; `user` is
    /// borrowed and must outlive the task.
    pub fn start(
        &mut self,
        runtime: &mut Runtime,
        resume: TaskResume,
        user: *mut c_void,
    ) -> Option<&mut Future> {
        let completion = unsafe { wasmos_async_start(runtime, self, resume, user) };
        unsafe { completion.as_mut() }
    }

    /// Awaits this coroutine's completion from inside another task: `Ok(result)`
    /// once it finished, `Err(Pending)` after parking the caller (which must
    /// then yield), `Err(Failed)` with its failure status. Not a blocking join
    /// -- called outside a running coroutine it yields `Err(Failed)` with -1.
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
    /// A zeroed, unregistered continuation record, usable in a `static` or an
    /// array for `race`/`all`.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
}

impl FutureGroup {
    /// Number of source callbacks that have run. A settled race or fail-fast
    /// all leaves this at 1: the rest were discarded, not merely outvoted.
    /// Zig and AssemblyScript expose the field directly; Rust keeps the struct
    /// private, so the same observation is available through here.
    pub fn completed(&self) -> usize {
        self.completed
    }

    /// True until the group settles and releases its remaining sources.
    pub fn active(&self) -> bool {
        self.active
    }

    /// True once the group future has been resolved or rejected.
    pub fn settled(&self) -> bool {
        self.settled
    }

    /// A zeroed, unstarted group, usable in a `static`.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    /// Settles with the first input to settle, resolving with its value or
    /// rejecting with its status. The losers are abandoned, not cancelled: their
    /// own work continues and their results are discarded.
    ///
    /// `None` unless `inputs` and `continuations` are non-empty and equal in
    /// length, every continuation unused, and every input either unowned or
    /// owned by `runtime`. The input pointers are read during this call only,
    /// but the futures themselves and both slices must stay live until the
    /// returned future settles.
    pub fn race<'a>(
        &'a mut self,
        runtime: &mut Runtime,
        inputs: &mut [&mut Future],
        continuations: &mut [Continuation],
    ) -> Option<&'a mut Future> {
        if inputs.is_empty() || inputs.len() != continuations.len() {
            return None;
        }
        let result = unsafe {
            wasmos_future_race(
                runtime,
                self,
                inputs.as_mut_ptr() as *const *mut Future,
                inputs.len(),
                continuations.as_mut_ptr(),
            )
        };
        unsafe { result.as_mut() }
    }

    /// Settles once every input has resolved, filling `values` in input order
    /// and resolving with `values.as_mut_ptr()` as a `usize`; the first input to
    /// reject rejects the group with that status and abandons the rest.
    ///
    /// `None` unless all three slices are non-empty and equal in length, with
    /// the same continuation and runtime conditions as `race`. `values` is
    /// written from source callbacks, so it must stay live until the returned
    /// future settles, and is only fully populated on success.
    pub fn all<'a>(
        &'a mut self,
        runtime: &mut Runtime,
        inputs: &mut [&mut Future],
        values: &mut [usize],
        continuations: &mut [Continuation],
    ) -> Option<&'a mut Future> {
        if inputs.is_empty() || inputs.len() != values.len() || inputs.len() != continuations.len()
        {
            return None;
        }
        let result = unsafe {
            wasmos_future_all(
                runtime,
                self,
                inputs.as_mut_ptr() as *const *mut Future,
                inputs.len(),
                values.as_mut_ptr(),
                continuations.as_mut_ptr(),
            )
        };
        unsafe { result.as_mut() }
    }
}

impl EventLoop {
    /// A zeroed loop, usable in a `static`; `init` binds it to an endpoint.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
    /// Binds the loop to `receiver_endpoint` and seeds its request ids at
    /// `request_id_base`, clearing both tables. Also creates a select set over
    /// the endpoint so `poll` can park; without one, `poll` degrades to a
    /// non-blocking drain. Bases must not collide between loops in one process,
    /// since the request id is what routes a reply to its intent.
    pub fn init(&mut self, receiver_endpoint: i32, request_id_base: i32) {
        unsafe { wasmos_sys_event_loop_init(self, receiver_endpoint, request_id_base) }
    }
    /// Dispatches up to `budget` messages (0 means 1) and returns how many were
    /// handled. A message matching an in-flight request id resolves that intent,
    /// otherwise a type handler runs, otherwise the default handler; anything
    /// unclaimed is dropped. With nothing queued the first iteration parks on
    /// the select set instead of spinning.
    pub fn poll(&mut self, budget: i32) -> i32 {
        unsafe { wasmos_sys_event_loop_poll(self, budget) }
    }
}

impl IpcFuture {
    /// A zeroed record, usable in a `static`; `init` arms it.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
    /// Zeroes the record and re-arms its future. `reply_status` decides how the
    /// reply settles the future (`None` accepts any reply); `user` is borrowed
    /// and handed back to it.
    pub fn init(&mut self, reply_status: Option<IpcReplyStatus>, user: *mut c_void) {
        unsafe { wasmos_sys_wasm_ipc_future_init(self, reply_status, user) }
    }
    /// Sends the request through `loop_` and returns the future that settles
    /// when the reply is dispatched, together with the allocated request id.
    ///
    /// `None` when the record is already in flight or was not re-armed. A send
    /// that fails still returns the future, already rejected, so a caller that
    /// chained onto it observes the failure through the same path as a rejecting
    /// reply. The future resolves with the address of the stored reply.
    pub fn send(
        &mut self,
        loop_: &mut EventLoop,
        destination: i32,
        source: i32,
        msg_type: i32,
        args: [i32; 4],
    ) -> Option<(&mut Future, i32)> {
        let mut request_id = 0;
        let future = unsafe {
            wasmos_sys_wasm_ipc_future_send(
                loop_,
                self,
                destination,
                source,
                msg_type,
                args[0],
                args[1],
                args[2],
                args[3],
                &mut request_id,
            )
        };
        unsafe { future.as_mut().map(|future| (future, request_id)) }
    }
    /// Stops tracking an in-flight request and rejects its future with `status`
    /// (a non-negative status becomes -1). Only local tracking stops: a late
    /// reply from the peer is then dispatched as an ordinary message.
    pub fn cancel(&mut self, status: i32) {
        unsafe { wasmos_sys_wasm_ipc_future_cancel(self, status) }
    }
    /// The stored reply. Meaningful only after the future settles -- it starts
    /// zeroed -- and overwritten by the next `send` on this record.
    pub fn reply(&self) -> &IpcMessage {
        unsafe { &*wasmos_sys_wasm_ipc_future_reply(self) }
    }
}

impl FsRequest {
    /// A zeroed request, usable in a `static`; `init` arms it.
    pub const fn new() -> Self {
        Self(unsafe { core::mem::zeroed() })
    }
    /// Re-arms the request with the FS reply validator installed.
    pub fn init(&mut self) {
        unsafe { wasmos_sys_wasm_fs_request_init(self) }
    }
    /// Sends one FS protocol message; see `IpcFuture::send`. `None` when either
    /// endpoint is negative. Any transfer buffer named by `args` stays
    /// caller-owned until the future settles.
    pub fn send(
        &mut self,
        loop_: &mut EventLoop,
        fs_endpoint: i32,
        reply_endpoint: i32,
        msg_type: i32,
        args: [i32; 4],
    ) -> Option<(&mut Future, i32)> {
        let mut request_id = 0;
        let future = unsafe {
            wasmos_sys_wasm_fs_request_send(
                loop_,
                self,
                fs_endpoint,
                reply_endpoint,
                msg_type,
                args[0],
                args[1],
                args[2],
                args[3],
                &mut request_id,
            )
        };
        unsafe { future.as_mut().map(|future| (future, request_id)) }
    }
    /// The stored FS reply; see `IpcFuture::reply`. `arg0` carries the FS status
    /// or byte count.
    pub fn reply(&self) -> &IpcMessage {
        unsafe { &*wasmos_sys_wasm_fs_request_reply(self) }
    }
}

// ============================================================================
// Typed asynchronous filesystem operations and the C-owned application wrapper.
//
// These mirror the Go `AsyncFSOperation` / `RunAsyncApp` API so a Rust app can
// express a filesystem workflow as a Promise-style chain of `.then` callbacks
// driven by the shared C coroutine runtime.  Unlike TinyGo, Rust marshals raw
// pointer arguments across the linked C boundary correctly, so buffers are
// passed straight through with no staging copy, and Rust `extern "C"` functions
// are used directly as continuation callbacks (no per-callback trampoline).
// ============================================================================

/// A success callback returns this to hand its returned future back to the
/// runtime; the child future then adopts that future's eventual result.
pub const FUTURE_CHAIN_NEXT: i32 = 2;

#[link(wasm_import_module = "wasmos")]
unsafe extern "C" {
    fn fs_endpoint() -> i32;
}

/// Matches `wasmos_sys_wasm_fs_operation_t`: one FS request plus the transfer
/// buffer bookkeeping the C helpers own until `finish`.
#[repr(C)]
pub struct FsOperation {
    request: FsRequest,
    buffer_id: i32,
    buffer_borrow: i32,
    length: i32,
    has_buffer: u8,
}

type PrepareFn = unsafe extern "C" fn(*mut c_void, i32, i32, i32, i32);

/// Matches `wasmos_sys_wasm_async_config_t`: the C wrapper owns the runtime,
/// root coroutine, event loop, and private reply endpoint for the whole app.
#[repr(C)]
pub struct AsyncAppConfig {
    runtime: Runtime,
    root: Coroutine,
    event_loop: EventLoop,
    reply_endpoint: i32,
    resume: Option<TaskResume>,
    prepare: Option<PrepareFn>,
    user: *mut c_void,
}

impl AsyncAppConfig {
    /// A zeroed config, usable in a `static`; `run_async_app` fills in the
    /// resume hook and hands it to the C wrapper.
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
}

unsafe extern "C" {
    fn wasmos_sys_wasm_fs_operation_init(op: *mut FsOperation);
    fn wasmos_sys_wasm_fs_open_async(
        loop_: *mut EventLoop,
        op: *mut FsOperation,
        fs_endpoint: i32,
        reply_endpoint: i32,
        path: *const u8,
        flags: i32,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_fs_read_async(
        loop_: *mut EventLoop,
        op: *mut FsOperation,
        fs_endpoint: i32,
        reply_endpoint: i32,
        fd: i32,
        dst: *mut c_void,
        len: i32,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_fs_write_async(
        loop_: *mut EventLoop,
        op: *mut FsOperation,
        fs_endpoint: i32,
        reply_endpoint: i32,
        fd: i32,
        src: *const c_void,
        len: i32,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_fs_close_async(
        loop_: *mut EventLoop,
        op: *mut FsOperation,
        fs_endpoint: i32,
        reply_endpoint: i32,
        fd: i32,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_fs_unlink_async(
        loop_: *mut EventLoop,
        op: *mut FsOperation,
        fs_endpoint: i32,
        reply_endpoint: i32,
        path: *const u8,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_fs_stat_async(
        loop_: *mut EventLoop,
        op: *mut FsOperation,
        fs_endpoint: i32,
        reply_endpoint: i32,
        path: *const u8,
        out_request_id: *mut i32,
    ) -> *mut Future;
    fn wasmos_sys_wasm_fs_operation_finish(
        op: *mut FsOperation,
        read_dst: *mut c_void,
        read_capacity: i32,
        out_reply: *mut IpcMessage,
    ) -> i32;
    fn wasmos_future_then_flat(
        runtime: *mut Runtime,
        future: *mut Future,
        continuation: *mut Continuation,
        adopt_continuation: *mut Continuation,
        on_success: Option<SuccessCallback>,
        on_error: Option<ErrorCallback>,
        user: *mut c_void,
    ) -> *mut Future;
    fn wasmos_sys_wasm_async_run(
        config: *mut AsyncAppConfig,
        arg0: i32,
        arg1: i32,
        arg2: i32,
        arg3: i32,
    ) -> i32;
    fn wasmos_sys_wasm_async_event_loop() -> *mut EventLoop;
    fn wasmos_sys_wasm_async_reply_endpoint() -> i32;
    fn wasmos_sys_wasm_async_runtime() -> *mut Runtime;
}

/// Body of an `AsyncFsOp::then` step: receives the settled operation (call
/// `result` on it to take the payload) and returns the next future, or null to
/// reject the chain with -1.
pub type ChainCallback = extern "C" fn(&mut AsyncFsOp) -> *mut Future;
/// Body of an `AsyncFsOp::catch` handler: receives the rejecting negative status
/// and returns 0 to resolve the chain, or a negative status to keep it rejected.
pub type CatchCallback = extern "C" fn(i32) -> i32;

/// One typed asynchronous filesystem operation plus the owner storage its
/// promise chain needs.  Instances come from a fixed leak pool: the WASM app
/// targets are `no_std` with no heap, and every op in a chain must stay live
/// until the whole chain settles (the flattened futures adopt one another).
pub struct AsyncFsOp {
    operation: FsOperation,
    future: *mut Future,
    continuation: Continuation,
    adopt: Continuation,
    reply: IpcMessage,
    read_ptr: *mut u8,
    read_len: i32,
    path: [u8; 256],
    chain: Option<ChainCallback>,
    catch: Option<CatchCallback>,
}

impl AsyncFsOp {
    const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    /// Copy out a settled read payload and return the response status/result.
    /// Idempotent buffer release, matching the C helper.
    ///
    /// The value is the reply's `arg0` -- an fd for open, a byte count for
    /// read/write, a size for stat -- or -1 when the payload could not be copied
    /// out. Call it only after the operation's future has settled: the stored
    /// reply starts zeroed, so an early call reports 0 rather than a real status.
    pub fn result(&mut self) -> i32 {
        let op = self as *mut AsyncFsOp;
        unsafe {
            let dst = if (*op).read_ptr.is_null() {
                core::ptr::null_mut()
            } else {
                (*op).read_ptr as *mut c_void
            };
            wasmos_sys_wasm_fs_operation_finish(
                &mut (*op).operation,
                dst,
                (*op).read_len,
                &mut (*op).reply,
            )
        }
    }

    /// JavaScript-Promise `then`: the callback returns the next future and the
    /// returned future adopts its eventual result.
    ///
    /// The callback runs from the runtime's continuation queue after this
    /// operation settles, never inline. A rejected operation skips it and
    /// forwards the status. Null when the operation never started; the op holds
    /// one continuation record, so `then` and `catch` share it and only one
    /// registration can be live at a time.
    pub fn then(&'static mut self, chain: ChainCallback) -> *mut Future {
        let op = self as *mut AsyncFsOp;
        unsafe {
            if (*op).future.is_null() {
                return core::ptr::null_mut();
            }
            (*op).chain = Some(chain);
            wasmos_future_then_flat(
                wasmos_sys_wasm_async_runtime(),
                (*op).future,
                &mut (*op).continuation,
                &mut (*op).adopt,
                Some(async_fs_chain_success),
                Some(async_fs_chain_error),
                op as *mut c_void,
            )
        }
    }

    /// Promise `catch`: convert a rejected operation into a resolved one. The
    /// handler returns zero to resolve or a negative status to keep rejecting.
    ///
    /// A resolved operation passes through with its value untouched, so the
    /// handler runs only on rejection. Registers on the same continuation record
    /// as `then`; null when the operation never started.
    pub fn catch(&'static mut self, handler: CatchCallback) -> *mut Future {
        let op = self as *mut AsyncFsOp;
        unsafe {
            if (*op).future.is_null() {
                return core::ptr::null_mut();
            }
            (*op).catch = Some(handler);
            wasmos_future_then(
                wasmos_sys_wasm_async_runtime(),
                (*op).future,
                &mut (*op).continuation,
                None,
                Some(async_fs_catch_error),
                op as *mut c_void,
            )
        }
    }
}

unsafe extern "C" fn async_fs_chain_success(
    user: *mut c_void,
    _value: usize,
    out: *mut usize,
) -> i32 {
    let op = user as *mut AsyncFsOp;
    unsafe {
        if let Some(chain) = (*op).chain {
            let next = chain(&mut *op);
            if next.is_null() {
                return -1;
            }
            *out = next as usize;
            return FUTURE_CHAIN_NEXT;
        }
    }
    -1
}

unsafe extern "C" fn async_fs_chain_error(_user: *mut c_void, status: i32, out: *mut usize) -> i32 {
    unsafe { *out = 0 };
    status
}

unsafe extern "C" fn async_fs_catch_error(user: *mut c_void, status: i32, out: *mut usize) -> i32 {
    let op = user as *mut AsyncFsOp;
    unsafe {
        *out = 0;
        match (*op).catch {
            Some(handler) => handler(status),
            None => status,
        }
    }
}

const ASYNC_FS_OP_MAX: usize = 24;
static mut FS_OP_POOL: [AsyncFsOp; ASYNC_FS_OP_MAX] = [const { AsyncFsOp::new() }; ASYNC_FS_OP_MAX];
static mut FS_OP_NEXT: usize = 0;

fn fs_op_alloc() -> Option<&'static mut AsyncFsOp> {
    unsafe {
        if FS_OP_NEXT >= ASYNC_FS_OP_MAX {
            return None;
        }
        let op = core::ptr::addr_of_mut!(FS_OP_POOL[FS_OP_NEXT]);
        FS_OP_NEXT += 1;
        *op = AsyncFsOp::new();
        Some(&mut *op)
    }
}

fn stage_path(op: *mut AsyncFsOp, path: &str) -> bool {
    let bytes = path.as_bytes();
    unsafe {
        let buf = &mut (*op).path;
        if bytes.is_empty() || bytes.len() + 1 > buf.len() {
            return false;
        }
        buf[..bytes.len()].copy_from_slice(bytes);
        buf[bytes.len()] = 0;
    }
    true
}

// The six *_async starters below all draw an AsyncFsOp from the fixed 24-entry
// leak pool (None once it is exhausted, since ops are never returned to it) and
// submit one FS request through the wrapper's event loop and reply endpoint.
// They are only usable from inside run_async_app: outside it the wrapper has no
// active config and passes null pointers to the C helpers. None of them blocks;
// the result is taken with AsyncFsOp::result once the operation settles, and a
// backend refusal arrives as an FS error message that rejects the future with
// -1, losing the packed WASMOS_ERR_FS_* code.
/// Open `path` with `flags`; returns a promise-chainable operation. `path` is
/// copied into the operation and must be non-empty and under 256 bytes.
pub fn open_async(path: &str, flags: i32) -> Option<&'static mut AsyncFsOp> {
    let op = fs_op_alloc()? as *mut AsyncFsOp;
    if !stage_path(op, path) {
        return None;
    }
    unsafe {
        let mut request_id = 0;
        let future = wasmos_sys_wasm_fs_open_async(
            wasmos_sys_wasm_async_event_loop(),
            &mut (*op).operation,
            fs_endpoint(),
            wasmos_sys_wasm_async_reply_endpoint(),
            (*op).path.as_ptr(),
            flags,
            &mut request_id,
        );
        if future.is_null() {
            return None;
        }
        (*op).future = future;
        Some(&mut *op)
    }
}

/// Read up to `len` bytes for `fd`. The destination at `dst` must stay live
/// until the operation settles and `result` copies the payload into it. One
/// request, so the byte count `result` returns may be short of `len`; `None` for
/// a null `dst` or a non-positive `len`.
pub fn read_async(fd: i32, dst: *mut u8, len: i32) -> Option<&'static mut AsyncFsOp> {
    if dst.is_null() || len <= 0 {
        return None;
    }
    let op = fs_op_alloc()? as *mut AsyncFsOp;
    unsafe {
        (*op).read_ptr = dst;
        (*op).read_len = len;
        let mut request_id = 0;
        let future = wasmos_sys_wasm_fs_read_async(
            wasmos_sys_wasm_async_event_loop(),
            &mut (*op).operation,
            fs_endpoint(),
            wasmos_sys_wasm_async_reply_endpoint(),
            fd,
            dst as *mut c_void,
            len,
            &mut request_id,
        );
        if future.is_null() {
            return None;
        }
        (*op).future = future;
        Some(&mut *op)
    }
}

/// Write `len` bytes from `src` to `fd`. `src` is copied synchronously into the
/// transfer buffer, so it need only be live for this call. Not chunked: one
/// buffer is acquired for the whole payload, so a `len` beyond the
/// transfer-buffer limit fails the acquire and returns `None`, and a short write
/// is reported through `result`.
pub fn write_async(fd: i32, src: *const u8, len: i32) -> Option<&'static mut AsyncFsOp> {
    if src.is_null() || len <= 0 {
        return None;
    }
    let op = fs_op_alloc()? as *mut AsyncFsOp;
    unsafe {
        let mut request_id = 0;
        let future = wasmos_sys_wasm_fs_write_async(
            wasmos_sys_wasm_async_event_loop(),
            &mut (*op).operation,
            fs_endpoint(),
            wasmos_sys_wasm_async_reply_endpoint(),
            fd,
            src as *const c_void,
            len,
            &mut request_id,
        );
        if future.is_null() {
            return None;
        }
        (*op).future = future;
        Some(&mut *op)
    }
}

/// Close `fd`. Uses no transfer buffer; `result` returns the reply's status.
pub fn close_async(fd: i32) -> Option<&'static mut AsyncFsOp> {
    let op = fs_op_alloc()? as *mut AsyncFsOp;
    unsafe {
        let mut request_id = 0;
        let future = wasmos_sys_wasm_fs_close_async(
            wasmos_sys_wasm_async_event_loop(),
            &mut (*op).operation,
            fs_endpoint(),
            wasmos_sys_wasm_async_reply_endpoint(),
            fd,
            &mut request_id,
        );
        if future.is_null() {
            return None;
        }
        (*op).future = future;
        Some(&mut *op)
    }
}

/// Unlink `path`. A refusal by the backend rejects the returned future rather
/// than reporting a status through `result`.
pub fn unlink_async(path: &str) -> Option<&'static mut AsyncFsOp> {
    let op = fs_op_alloc()? as *mut AsyncFsOp;
    if !stage_path(op, path) {
        return None;
    }
    unsafe {
        let mut request_id = 0;
        let future = wasmos_sys_wasm_fs_unlink_async(
            wasmos_sys_wasm_async_event_loop(),
            &mut (*op).operation,
            fs_endpoint(),
            wasmos_sys_wasm_async_reply_endpoint(),
            (*op).path.as_ptr(),
            &mut request_id,
        );
        if future.is_null() {
            return None;
        }
        (*op).future = future;
        Some(&mut *op)
    }
}

/// Stat `path`; rejects when the path does not exist. On success `result`
/// returns the file size.
pub fn stat_async(path: &str) -> Option<&'static mut AsyncFsOp> {
    let op = fs_op_alloc()? as *mut AsyncFsOp;
    if !stage_path(op, path) {
        return None;
    }
    unsafe {
        let mut request_id = 0;
        let future = wasmos_sys_wasm_fs_stat_async(
            wasmos_sys_wasm_async_event_loop(),
            &mut (*op).operation,
            fs_endpoint(),
            wasmos_sys_wasm_async_reply_endpoint(),
            (*op).path.as_ptr(),
            &mut request_id,
        );
        if future.is_null() {
            return None;
        }
        (*op).future = future;
        Some(&mut *op)
    }
}

struct AppState {
    start: Option<extern "C" fn() -> *mut Future>,
    completion: *mut Future,
    started: bool,
}

static mut APP_STATE: AppState = AppState {
    start: None,
    completion: core::ptr::null_mut(),
    started: false,
};
static mut APP_CONFIG: AsyncAppConfig = AsyncAppConfig::new();

unsafe extern "C" fn app_resume(_user: *mut c_void, out: *mut usize) -> i32 {
    unsafe {
        if !APP_STATE.started {
            APP_STATE.started = true;
            APP_STATE.completion = match APP_STATE.start {
                Some(start) => start(),
                None => core::ptr::null_mut(),
            };
        }
        if APP_STATE.completion.is_null() {
            return -1;
        }
        match (*APP_STATE.completion).await_value() {
            AwaitResult::Pending => TaskResult::YIELDED,
            AwaitResult::Ready { value } => {
                *out = value;
                TaskResult::COMPLETE
            }
            AwaitResult::Failed { status } => status,
            AwaitResult::Invalid => -1,
        }
    }
}

/// Enter the C-owned application wrapper. `start` runs once from the root
/// coroutine and returns the terminal future for the app's promise chain; the
/// wrapper owns the runtime, event loop, and private reply endpoint.
///
/// Blocks until that future settles: the wrapper alternates one coroutine step
/// with one event-loop poll, parking on the reply endpoint whenever the root
/// task waits, so it never spins. Returns the chain's resolved value truncated
/// to `i32`; every failure collapses to -1, including a rejected chain, a null
/// return from `start`, and a reply endpoint or root coroutine that could not be
/// created. The wrapper state is module-static, so one call is live at a time.
pub fn run_async_app(start: extern "C" fn() -> *mut Future) -> i32 {
    unsafe {
        APP_STATE.start = Some(start);
        APP_STATE.completion = core::ptr::null_mut();
        APP_STATE.started = false;
        let config = core::ptr::addr_of_mut!(APP_CONFIG);
        (*config).resume = Some(app_resume);
        (*config).prepare = None;
        (*config).user = core::ptr::null_mut();
        wasmos_sys_wasm_async_run(config, 0, 0, 0, 0)
    }
}
