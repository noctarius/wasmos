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
    continuations: *mut Continuation,
    values: *mut usize,
    count: usize,
    completed: usize,
    kind: FutureGroupKind,
    settled: bool,
    active: bool,
}

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

pub type IpcReplyStatus = unsafe extern "C" fn(*mut c_void, *const IpcMessage) -> i32;

#[repr(C)]
pub struct EventLoop {
    receiver_endpoint: i32,
    select_id: i32,
    next_request_id: i32,
    default_on_message: Option<unsafe extern "C" fn(*mut c_void, *const IpcMessage)>,
    default_user: *mut c_void,
    intents: [u32; 64],
    handlers: [u32; 64],
}

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
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }

    pub fn init(&mut self) {
        unsafe { wasmos_wasm_runtime_init(self) }
    }

    pub fn run(&mut self) -> Result<i32, ()> {
        let count = unsafe { wasmos_wasm_coroutine_run(self) };
        if count < 0 {
            Err(())
        } else {
            Ok(count)
        }
    }

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
        let child =
            unsafe { wasmos_future_then(runtime, self, continuation, success, error, user) };
        unsafe { child.as_mut() }
    }
}

impl Promise {
    pub const fn new() -> Self {
        Self {
            future: core::ptr::null_mut(),
        }
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
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
    pub fn init(&mut self, receiver_endpoint: i32, request_id_base: i32) {
        unsafe { wasmos_sys_event_loop_init(self, receiver_endpoint, request_id_base) }
    }
    pub fn poll(&mut self, budget: i32) -> i32 {
        unsafe { wasmos_sys_event_loop_poll(self, budget) }
    }
}

impl IpcFuture {
    pub const fn new() -> Self {
        unsafe { core::mem::zeroed() }
    }
    pub fn init(&mut self, reply_status: Option<IpcReplyStatus>, user: *mut c_void) {
        unsafe { wasmos_sys_wasm_ipc_future_init(self, reply_status, user) }
    }
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
    pub fn cancel(&mut self, status: i32) {
        unsafe { wasmos_sys_wasm_ipc_future_cancel(self, status) }
    }
    pub fn reply(&self) -> &IpcMessage {
        unsafe { &*wasmos_sys_wasm_ipc_future_reply(self) }
    }
}

impl FsRequest {
    pub const fn new() -> Self {
        Self(unsafe { core::mem::zeroed() })
    }
    pub fn init(&mut self) {
        unsafe { wasmos_sys_wasm_fs_request_init(self) }
    }
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

pub type ChainCallback = extern "C" fn(&mut AsyncFsOp) -> *mut Future;
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
    pub fn result(&mut self) -> i32 {
        let op = self as *mut AsyncFsOp;
        unsafe {
            let dst = if (*op).read_ptr.is_null() {
                core::ptr::null_mut()
            } else {
                (*op).read_ptr as *mut c_void
            };
            wasmos_sys_wasm_fs_operation_finish(&mut (*op).operation, dst, (*op).read_len, &mut (*op).reply)
        }
    }

    /// JavaScript-Promise `then`: the callback returns the next future and the
    /// returned future adopts its eventual result.
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

unsafe extern "C" fn async_fs_chain_success(user: *mut c_void, _value: usize, out: *mut usize) -> i32 {
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

/// Open `path` with `flags`; returns a promise-chainable operation.
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
/// until the operation settles and `result` copies the payload into it.
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
/// transfer buffer, so it need only be live for this call.
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

/// Close `fd`.
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

/// Unlink `path`.
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

/// Stat `path`; rejects when the path does not exist.
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
