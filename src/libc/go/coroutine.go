package main

import "unsafe"

// These types are caller-owned wasm32 ABI records for the shared C coroutine
// implementation. Keep their storage in uint32 words: C accesses them through
// pointers and therefore requires four-byte alignment.
//
// The word counts are the sizeof() of the C structs on wasm32 and MUST match
// exactly: C writes through these pointers, so a count that is too small is
// heap corruption, not a compile error. Unlike the Rust and Zig bindings, which
// declare real fields and let the compiler compute the layout, these are opaque
// blobs that nothing checks at the language level -- so they are pinned by
// tests/unit/test_go_abi_sizes.c, which fails the build if a C struct moves.
type Runtime struct{ storage [6]uint32 }
type Future struct{ storage [6]uint32 }
type Promise struct{ storage [1]uint32 }
type Coroutine struct{ storage [14]uint32 }
type Continuation struct{ storage [15]uint32 }
type FutureGroup struct{ storage [14]uint32 }
type EventLoop struct{ storage [133]uint32 }
type IPCFuture struct{ storage [20]uint32 }
type FSRequest struct{ storage [20]uint32 }
type FSOperation struct{ storage [24]uint32 }

// AsyncFSOperation owns the ABI operation, continuation records, and path
// storage needed by one typed filesystem thenable.
type AsyncFSOperation struct {
	operation    FSOperation
	future       *Future
	continuation Continuation
	adopt        Continuation
	reply        IPCFutureReply
	read         []byte
	path         [256]byte
	staging      [256]byte
}
type IPCFutureReply struct {
	Type, RequestID, Arg0, Arg1, Arg2, Arg3, Source, Destination int32
}

const (
	TaskComplete int32 = 0
	TaskYielded  int32 = 1
	AwaitPending int32 = 1
)

// A task or continuation callback is a wasm function-table address. Export a
// Go callback with TinyGo and pass its table address when callback support is
// needed; Go has no portable representation for a C function pointer.
type Callback uintptr
type IPCReplyStatus Callback
type FutureSuccess func(value uintptr) (uintptr, int32)
type FutureError func(status int32) (uintptr, int32)
type FutureChain func(value uintptr) *Future

const goFutureCallbackMax = 32

var goFutureSuccess [goFutureCallbackMax]FutureSuccess
var goFutureError [goFutureCallbackMax]FutureError
var goFutureChain [goFutureCallbackMax]FutureChain
var goAsyncAppStart func() *Future
var goAsyncAppCompletion *Future
var goAsyncAppStarted bool

//go:extern wasmos_wasm_runtime_init
//go:linkname wasmRuntimeInit wasmos_wasm_runtime_init
func wasmRuntimeInit(*Runtime)

//go:extern wasmos_async_start
//go:linkname wasmAsyncStart wasmos_async_start
func wasmAsyncStart(*Runtime, *Coroutine, Callback, unsafe.Pointer) *Future

//go:extern wasmos_wasm_coroutine_run
//go:linkname wasmCoroutineRun wasmos_wasm_coroutine_run
func wasmCoroutineRun(*Runtime) int32

//go:extern wasmos_wasm_coroutine_run_budget
//go:linkname wasmCoroutineRunBudget wasmos_wasm_coroutine_run_budget
func wasmCoroutineRunBudget(*Runtime, uintptr) int32

//go:extern wasmos_wasm_coroutine_join
//go:linkname wasmCoroutineJoin wasmos_wasm_coroutine_join
func wasmCoroutineJoin(*Coroutine, *int32) int32

//go:extern wasmos_future_init
//go:linkname wasmFutureInit wasmos_future_init
func wasmFutureInit(*Future, *Promise)

//go:extern wasmos_future_poll
//go:linkname wasmFuturePoll wasmos_future_poll
func wasmFuturePoll(*Future, *int32, *uintptr) bool

//go:extern wasmos_future_await
//go:linkname wasmFutureAwait wasmos_future_await
func wasmFutureAwait(*Future, *uintptr) int32

//go:extern wasmos_promise_resolve
//go:linkname wasmPromiseResolve wasmos_promise_resolve
func wasmPromiseResolve(*Promise, uintptr) bool

//go:extern wasmos_promise_reject
//go:linkname wasmPromiseReject wasmos_promise_reject
func wasmPromiseReject(*Promise, int32) bool

//go:extern wasmos_future_then
func wasmFutureThen(*Runtime, *Future, *Continuation, Callback, Callback, unsafe.Pointer) *Future

//go:extern wasmos_future_race
func wasmFutureRace(*Runtime, *FutureGroup, unsafe.Pointer, uintptr, *Continuation) *Future

//go:extern wasmos_future_all
func wasmFutureAll(*Runtime, *FutureGroup, unsafe.Pointer, uintptr, *uintptr, *Continuation) *Future

//go:extern wasmos_sys_wasm_event_loop_init
//go:linkname wasmEventLoopInit wasmos_sys_wasm_event_loop_init
func wasmEventLoopInit(*EventLoop, int32, int32)

//go:extern wasmos_sys_wasm_event_loop_poll
//go:linkname wasmEventLoopPoll wasmos_sys_wasm_event_loop_poll
func wasmEventLoopPoll(*EventLoop, int32) int32

//go:extern wasmos_sys_wasm_ipc_future_init
func wasmIPCFutureInit(*IPCFuture, IPCReplyStatus, unsafe.Pointer)

//go:extern wasmos_sys_wasm_ipc_future_send
func wasmIPCFutureSend(*EventLoop, *IPCFuture, int32, int32, int32, int32, int32, int32, int32, *int32) *Future

//go:extern wasmos_sys_wasm_ipc_future_cancel
func wasmIPCFutureCancel(*IPCFuture, int32)

//go:extern wasmos_sys_wasm_ipc_future_reply
func wasmIPCFutureReply(*IPCFuture) *IPCFutureReply

//go:extern wasmos_sys_wasm_fs_request_init
//go:linkname wasmFSRequestInit wasmos_sys_wasm_fs_request_init
func wasmFSRequestInit(*FSRequest)

//go:extern wasmos_sys_wasm_fs_request_send
//go:linkname wasmFSRequestSend wasmos_sys_wasm_fs_request_send
func wasmFSRequestSend(*EventLoop, *FSRequest, int32, int32, int32, int32, int32, int32, int32, *int32) *Future

//go:extern wasmos_sys_wasm_fs_request_reply
//go:linkname wasmFSRequestReply wasmos_sys_wasm_fs_request_reply
func wasmFSRequestReply(*FSRequest) *IPCFutureReply

//go:extern wasmos_sys_wasm_fs_operation_init
//go:linkname wasmFSOperationInit wasmos_sys_wasm_fs_operation_init
func wasmFSOperationInit(*FSOperation)

//go:extern wasmos_sys_wasm_fs_open_async
//go:linkname wasmFSOpenAsync wasmos_sys_wasm_fs_open_async
func wasmFSOpenAsync(*EventLoop, *FSOperation, int32, int32, *byte, int32, *int32) *Future

//go:extern wasmos_sys_wasm_fs_read_async
//go:linkname wasmFSReadAsync wasmos_sys_wasm_fs_read_async
func wasmFSReadAsync(*EventLoop, *FSOperation, int32, int32, int32, *byte, int32, *int32) *Future

//go:extern wasmos_sys_wasm_fs_write_async
//go:linkname wasmFSWriteAsync wasmos_sys_wasm_fs_write_async
func wasmFSWriteAsync(*EventLoop, *FSOperation, int32, int32, int32, *byte, int32, *int32) *Future

//go:extern wasmos_sys_wasm_fs_close_async
//go:linkname wasmFSCloseAsync wasmos_sys_wasm_fs_close_async
func wasmFSCloseAsync(*EventLoop, *FSOperation, int32, int32, int32, *int32) *Future

//go:extern wasmos_sys_wasm_fs_unlink_async
//go:linkname wasmFSUnlinkAsync wasmos_sys_wasm_fs_unlink_async
func wasmFSUnlinkAsync(*EventLoop, *FSOperation, int32, int32, *byte, *int32) *Future

//go:extern wasmos_sys_wasm_fs_stat_async
//go:linkname wasmFSStatAsync wasmos_sys_wasm_fs_stat_async
func wasmFSStatAsync(*EventLoop, *FSOperation, int32, int32, *byte, *int32) *Future

//go:extern wasmos_sys_wasm_fs_operation_finish
//go:linkname wasmFSOperationFinish wasmos_sys_wasm_fs_operation_finish
func wasmFSOperationFinish(*FSOperation, *byte, int32, *IPCFutureReply) int32

//go:extern wasmos_go_coroutine_start
//go:linkname wasmGoCoroutineStart wasmos_go_coroutine_start
func wasmGoCoroutineStart(*Runtime, *Coroutine, uint32) *Future

//go:extern wasmos_go_future_then
//go:linkname wasmGoFutureThen wasmos_go_future_then
func wasmGoFutureThen(*Runtime, *Future, *Continuation, uint32) *Future

//go:extern wasmos_go_future_then_flat
//go:linkname wasmGoFutureThenFlat wasmos_go_future_then_flat
func wasmGoFutureThenFlat(*Runtime, *Future, *Continuation, *Continuation, uint32) *Future

//go:extern wasmos_go_async_app_run
//go:linkname wasmGoAsyncAppRun wasmos_go_async_app_run
func wasmGoAsyncAppRun() int32

//go:extern wasmos_sys_wasm_async_event_loop
//go:linkname wasmAsyncAppEventLoop wasmos_sys_wasm_async_event_loop
func wasmAsyncAppEventLoop() *EventLoop

//go:extern wasmos_sys_wasm_async_reply_endpoint
//go:linkname wasmAsyncAppReplyEndpoint wasmos_sys_wasm_async_reply_endpoint
func wasmAsyncAppReplyEndpoint() int32

//go:extern wasmos_sys_wasm_async_runtime
//go:linkname wasmAsyncAppRuntime wasmos_sys_wasm_async_runtime
func wasmAsyncAppRuntime() *Runtime

func (r *Runtime) Init() {
	wasmRuntimeInit(r)
}

func (r *Runtime) Run() int32 {
	return wasmCoroutineRun(r)
}

func (r *Runtime) RunBudget(budget uintptr) int32 {
	return wasmCoroutineRunBudget(r, budget)
}

func (c *Coroutine) Start(runtime *Runtime, resume Callback, user unsafe.Pointer) *Future {
	if runtime == nil || resume == 0 {
		return nil
	}
	return wasmAsyncStart(runtime, c, resume, user)
}

// StartTask uses the C trampoline and the exported Go dispatcher below. taskID
// is caller-assigned, so multiple Go coroutines can be active concurrently.
func (c *Coroutine) StartTask(runtime *Runtime, taskID uint32) *Future {
	if runtime == nil || taskID == 0 {
		return nil
	}
	return wasmGoCoroutineStart(runtime, c, taskID)
}

// RunAsyncApp enters the C-owned application wrapper.  start is called once
// from its root coroutine; it returns the terminal future for the app chain.
func RunAsyncApp(start func() *Future) int32 {
	if start == nil || goAsyncAppStart != nil {
		return -1
	}
	goAsyncAppStart, goAsyncAppCompletion, goAsyncAppStarted = start, nil, false
	return wasmGoAsyncAppRun()
}

func AsyncAppLoop() *EventLoop {
	return wasmAsyncAppEventLoop()
}

func AsyncAppReplyEndpoint() int32 {
	return wasmAsyncAppReplyEndpoint()
}

func AsyncAppRuntime() *Runtime {
	return wasmAsyncAppRuntime()
}

//go:export wasmos_go_async_app_resume
func wasmosGoAsyncAppResume(out *uintptr) int32 {
	if !goAsyncAppStarted {
		goAsyncAppStarted, goAsyncAppCompletion = true, goAsyncAppStart()
	}
	if goAsyncAppCompletion == nil {
		return -1
	}
	value, status := goAsyncAppCompletion.Await()
	if status == AwaitPending {
		return TaskYielded
	}
	*out = value
	return status
}

func (c *Coroutine) Join() (int32, int32) {
	var result int32
	status := wasmCoroutineJoin(c, &result)
	return result, status
}

func (f *Future) Init(p *Promise) {
	if f != nil && p != nil {
		wasmFutureInit(f, p)
	}
}

// Poll returns settled=false while the result is pending. A settled status of
// zero is success; a negative status is failure.
func (f *Future) Poll() (value uintptr, status int32, settled bool) {
	settled = wasmFuturePoll(f, &status, &value)
	return
}

// Await returns zero or a negative failure status when settled, or AwaitPending
// after registering the currently running stackless coroutine as a waiter.
func (f *Future) Await() (value uintptr, status int32) {
	status = wasmFutureAwait(f, &value)
	return
}

func (f *Future) Then(runtime *Runtime, continuation *Continuation, success Callback, failure Callback, user unsafe.Pointer) *Future {
	if runtime == nil || continuation == nil {
		return nil
	}
	return wasmFutureThen(runtime, f, continuation, success, failure, user)
}

// ThenGo dispatches Go callbacks through the stable C trampoline.  Callbacks
// run from the runtime queue after settlement; no application polling or task
// suspension is exposed.  The returned future carries the callback result.
func (f *Future) ThenGo(runtime *Runtime, continuation *Continuation, success FutureSuccess, failure FutureError) *Future {
	if f == nil || runtime == nil || continuation == nil || (success == nil && failure == nil) {
		return nil
	}
	for i := 0; i < goFutureCallbackMax; i++ {
		if goFutureSuccess[i] == nil && goFutureError[i] == nil {
			// TODO: reclaim this slot once its continuation settles; hello-sized
			// chains are bounded, but long-running services need reusable slots.
			goFutureSuccess[i], goFutureError[i] = success, failure
			return wasmGoFutureThen(runtime, f, continuation, uint32(i+1))
		}
	}
	return nil
}

func (f *Future) ThenFlatGo(runtime *Runtime, continuation, adopt *Continuation, next FutureChain) *Future {
	if f == nil || runtime == nil || continuation == nil || adopt == nil || next == nil {
		return nil
	}
	for i := 0; i < goFutureCallbackMax; i++ {
		if goFutureChain[i] == nil {
			goFutureChain[i] = next
			return wasmGoFutureThenFlat(runtime, f, continuation, adopt, uint32(i+1))
		}
	}
	return nil
}

//go:export wasmos_go_future_chain
func wasmosGoFutureChain(callbackID uint32, value uintptr) uintptr {
	if callbackID == 0 || callbackID > goFutureCallbackMax || goFutureChain[callbackID-1] == nil {
		return 0
	}
	return uintptr(unsafe.Pointer(goFutureChain[callbackID-1](value)))
}

//go:export wasmos_go_future_success
func wasmosGoFutureSuccess(callbackID uint32, value uintptr, out *uintptr) int32 {
	if callbackID == 0 || callbackID > goFutureCallbackMax || goFutureSuccess[callbackID-1] == nil {
		return -1
	}
	value, status := goFutureSuccess[callbackID-1](value)
	*out = value
	return status
}

//go:export wasmos_go_future_error
func wasmosGoFutureError(callbackID uint32, status int32, out *uintptr) int32 {
	if callbackID == 0 || callbackID > goFutureCallbackMax || goFutureError[callbackID-1] == nil {
		return status
	}
	value, nextStatus := goFutureError[callbackID-1](status)
	*out = value
	return nextStatus
}

func (p *Promise) Resolve(value uintptr) bool {
	return p != nil && wasmPromiseResolve(p, value)
}

func (p *Promise) Reject(status int32) bool {
	return p != nil && status < 0 && wasmPromiseReject(p, status)
}

func (loop *EventLoop) Init(receiverEndpoint, requestIDBase int32) {
	if loop != nil {
		wasmEventLoopInit(loop, receiverEndpoint, requestIDBase)
	}
}

func (loop *EventLoop) Poll(budget int32) int32 {
	if loop == nil {
		return 0
	}
	return wasmEventLoopPoll(loop, budget)
}

// Init connects this caller-owned operation to a reply-status callback. The
// callback is a wasm table address and returns zero to resolve or a negative
// status to reject.
func (op *IPCFuture) Init(replyStatus IPCReplyStatus, user unsafe.Pointer) {
	if op != nil {
		wasmIPCFutureInit(op, replyStatus, user)
	}
}

func (op *IPCFuture) Send(loop *EventLoop, destination, source, msgType, arg0, arg1, arg2, arg3 int32) (*Future, int32) {
	if op == nil || loop == nil {
		return nil, 0
	}
	var requestID int32
	return wasmIPCFutureSend(loop, op, destination, source, msgType, arg0, arg1, arg2, arg3, &requestID), requestID
}

func (op *IPCFuture) Cancel(status int32) {
	if op != nil {
		wasmIPCFutureCancel(op, status)
	}
}

func (op *IPCFuture) Reply() *IPCFutureReply {
	if op == nil {
		return nil
	}
	return wasmIPCFutureReply(op)
}

// FSRequest is a caller-owned non-blocking filesystem request. It settles
// only on FS_IPC_RESP; callers retain any transfer buffers until settlement.
func (request *FSRequest) Init() {
	if request != nil {
		wasmFSRequestInit(request)
	}
}

func (request *FSRequest) Send(loop *EventLoop, fsEndpoint, replyEndpoint, msgType, arg0, arg1, arg2, arg3 int32) (*Future, int32) {
	if request == nil || loop == nil {
		return nil, 0
	}
	var requestID int32
	return wasmFSRequestSend(loop, request, fsEndpoint, replyEndpoint, msgType, arg0, arg1, arg2, arg3, &requestID), requestID
}

func (request *FSRequest) Reply() *IPCFutureReply {
	if request == nil {
		return nil
	}
	return wasmFSRequestReply(request)
}

func (op *FSOperation) Init() {
	if op != nil {
		wasmFSOperationInit(op)
	}
}

func (op *FSOperation) Open(loop *EventLoop, endpoint, reply int32, path []byte, flags int32) (*Future, int32) {
	var id int32
	if op == nil || len(path) == 0 {
		return nil, 0
	}
	return wasmFSOpenAsync(loop, op, endpoint, reply, &path[0], flags, &id), id
}

func (op *FSOperation) Read(loop *EventLoop, endpoint, reply, fd int32, dst []byte) (*Future, int32) {
	var id int32
	if op == nil || len(dst) == 0 {
		return nil, 0
	}
	return wasmFSReadAsync(loop, op, endpoint, reply, fd, &dst[0], int32(len(dst)), &id), id
}

func (op *FSOperation) Write(loop *EventLoop, endpoint, reply, fd int32, src []byte) (*Future, int32) {
	var id int32
	if op == nil || len(src) == 0 {
		return nil, 0
	}
	return wasmFSWriteAsync(loop, op, endpoint, reply, fd, &src[0], int32(len(src)), &id), id
}

func (op *FSOperation) Close(loop *EventLoop, endpoint, reply, fd int32) (*Future, int32) {
	var id int32
	if op == nil {
		return nil, 0
	}
	return wasmFSCloseAsync(loop, op, endpoint, reply, fd, &id), id
}

func (op *FSOperation) Unlink(loop *EventLoop, endpoint, reply int32, path []byte) (*Future, int32) {
	var id int32
	if op == nil || len(path) == 0 {
		return nil, 0
	}
	return wasmFSUnlinkAsync(loop, op, endpoint, reply, &path[0], &id), id
}

func (op *FSOperation) Stat(loop *EventLoop, endpoint, reply int32, path []byte) (*Future, int32) {
	var id int32
	if op == nil || len(path) == 0 {
		return nil, 0
	}
	return wasmFSStatAsync(loop, op, endpoint, reply, &path[0], &id), id
}

func (op *FSOperation) Finish(dst []byte, reply *IPCFutureReply) int32 {
	if op == nil {
		return -1
	}
	if len(dst) == 0 {
		return wasmFSOperationFinish(op, nil, 0, reply)
	}
	return wasmFSOperationFinish(op, &dst[0], int32(len(dst)), reply)
}

func (op *AsyncFSOperation) Result() int32 {
	if op == nil {
		return -1
	}
	return op.operation.Finish(op.read, &op.reply)
}

func (op *AsyncFSOperation) Map(next func(*AsyncFSOperation) *AsyncFSOperation) *Future {
	if op == nil || op.future == nil || next == nil {
		return nil
	}
	return op.future.ThenFlatGo(AsyncAppRuntime(), &op.continuation, &op.adopt, func(_ uintptr) *Future {
		nextOp := next(op)
		if nextOp == nil {
			return nil
		}
		return nextOp.future
	})
}

// Then follows JavaScript Promise semantics: the callback returns the next
// future and this returned future adopts its eventual result.
func (op *AsyncFSOperation) Then(next func(*AsyncFSOperation) *Future) *Future {
	if op == nil || op.future == nil || next == nil {
		return nil
	}
	return op.future.ThenFlatGo(AsyncAppRuntime(), &op.continuation, &op.adopt, func(_ uintptr) *Future { return next(op) })
}

func (f *Future) CatchGo(runtime *Runtime, continuation *Continuation, failure FutureError) *Future {
	return f.ThenGo(runtime, continuation, nil, failure)
}

func asyncFSOperation() *AsyncFSOperation {
	return &AsyncFSOperation{}
}

func (fsAPI) OpenAsync(path string, flags int32) *AsyncFSOperation {
	op := asyncFSOperation()
	if len(path) == 0 || len(path) >= len(op.path) {
		return nil
	}
	copy(op.path[:], path)
	op.path[len(path)] = 0
	op.future, _ = op.operation.Open(AsyncAppLoop(), fsEndpoint(), AsyncAppReplyEndpoint(), op.path[:len(path)+1], flags)
	if op.future == nil {
		return nil
	}
	return op
}

func (fsAPI) ReadAsync(fd int32, dst []byte) *AsyncFSOperation {
	if len(dst) == 0 {
		return nil
	}
	op := asyncFSOperation()
	op.read = dst
	op.future, _ = op.operation.Read(AsyncAppLoop(), fsEndpoint(), AsyncAppReplyEndpoint(), fd, dst)
	if op.future == nil {
		return nil
	}
	return op
}

func (fsAPI) WriteAsync(fd int32, src []byte) *AsyncFSOperation {
	op := asyncFSOperation()
	// Stage the payload into the operation's own array storage before crossing
	// into C.  TinyGo delivers a null pointer for the address of a heap-slice
	// element (&src[0]) passed through a //go:linkname C function; the address of
	// an element in a fixed array field marshals correctly, so the caller's bytes
	// are copied into op.staging first.  A single async write is bounded by one
	// transfer buffer.
	// TODO: chunk writes larger than the staging array like the synchronous File.Write.
	if len(src) == 0 || len(src) > len(op.staging) {
		return nil
	}
	copy(op.staging[:], src)
	op.future, _ = op.operation.Write(AsyncAppLoop(), fsEndpoint(), AsyncAppReplyEndpoint(), fd, op.staging[:len(src)])
	if op.future == nil {
		return nil
	}
	return op
}

func (fsAPI) CloseAsync(fd int32) *AsyncFSOperation {
	op := asyncFSOperation()
	op.future, _ = op.operation.Close(AsyncAppLoop(), fsEndpoint(), AsyncAppReplyEndpoint(), fd)
	if op.future == nil {
		return nil
	}
	return op
}

func (fsAPI) UnlinkAsync(path string) *AsyncFSOperation {
	op := asyncFSOperation()
	if len(path) == 0 || len(path) >= len(op.path) {
		return nil
	}
	copy(op.path[:], path)
	op.path[len(path)] = 0
	op.future, _ = op.operation.Unlink(AsyncAppLoop(), fsEndpoint(), AsyncAppReplyEndpoint(), op.path[:len(path)+1])
	if op.future == nil {
		return nil
	}
	return op
}

func (fsAPI) StatAsync(path string) *AsyncFSOperation {
	op := asyncFSOperation()
	if len(path) == 0 || len(path) >= len(op.path) {
		return nil
	}
	copy(op.path[:], path)
	op.path[len(path)] = 0
	op.future, _ = op.operation.Stat(AsyncAppLoop(), fsEndpoint(), AsyncAppReplyEndpoint(), op.path[:len(path)+1])
	if op.future == nil {
		return nil
	}
	return op
}

// Race settles with the first input result. inputs and continuations must have
// identical non-zero lengths and remain live until every input settles.
func (g *FutureGroup) Race(runtime *Runtime, inputs []*Future, continuations []Continuation) *Future {
	if runtime == nil || len(inputs) == 0 || len(inputs) != len(continuations) {
		return nil
	}
	return wasmFutureRace(runtime, g, unsafe.Pointer(&inputs[0]), uintptr(len(inputs)), &continuations[0])
}

// All settles successfully after all inputs; values receives each result in
// input order. All three slices must have identical non-zero lengths and stay
// live until every input settles.
func (g *FutureGroup) All(runtime *Runtime, inputs []*Future, values []uintptr, continuations []Continuation) *Future {
	if runtime == nil || len(inputs) == 0 || len(inputs) != len(values) || len(inputs) != len(continuations) {
		return nil
	}
	return wasmFutureAll(runtime, g, unsafe.Pointer(&inputs[0]), uintptr(len(inputs)), &values[0], &continuations[0])
}
