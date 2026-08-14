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
// What each record is:
//
//	Runtime      the cooperative scheduler: a queue of runnable coroutines and a
//	             queue of continuations whose source future has settled; nothing
//	             runs until Run or RunBudget is called
//	Future       the observing half of a one-shot result, settled exactly once by
//	             the Promise bound to it through Init
//	Promise      the settling half of a future; whoever holds it decides the
//	             outcome, once
//	Coroutine    one stackless task plus its completion future; Start overwrites
//	             the whole record, so it is reusable once the run is dead
//	Continuation the registration record for one Then callback, carrying the
//	             child future that callback settles; it must stay live until the
//	             callback runs and cannot hold two live registrations
//	FutureGroup  several futures combined into one; see Race and All
//	EventLoop    the IPC demultiplexer: one receive endpoint, a table of
//	             in-flight requests keyed by request id, and a table of
//	             per-message-type handlers
//	IPCFuture    one in-flight request as a future; Init re-arms it, Send refuses
//	             while the previous round trip is live, and the reply is copied
//	             into the record before the future settles
//	FSRequest    an IPCFuture pre-armed for the filesystem protocol: it resolves
//	             only on an FS_IPC_RESP reply and rejects anything else, error
//	             replies included, so the backend's packed status is not surfaced
//	FSOperation  one typed filesystem request plus the transfer buffer the C
//	             helpers acquire, borrow to the FS manager, and release in Finish
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
//
// IPCFutureReply, declared just below it, is one IPC message in the layout the C
// event loop uses (wasmos_ipc_message_t); its field order differs from IPCReply
// in wasmos.go, where Source and Destination come before the argument words.
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

// Task return values and the await sentinel.
//
// TaskComplete publishes the value written to the resume out-parameter as the
// coroutine's result. TaskYielded asks to be resumed again, either from the
// ready queue or when the future the task parked on settles; any other return is
// taken as a failure status and rejects the completion future, so it must be
// negative. AwaitPending is what Future.Await returns after parking the caller,
// and deliberately shares TaskYielded's value so a resume function can return it
// straight through.
const (
	TaskComplete int32 = 0
	TaskYielded  int32 = 1
	AwaitPending int32 = 1
)

// A task or continuation callback is a wasm function-table address. Export a
// Go callback with TinyGo and pass its table address when callback support is
// needed; Go has no portable representation for a C function pointer.
//
// IPCReplyStatus is such an address for a reply validator: it returns 0 to
// resolve the IPCFuture or a negative status to reject it, and runs while the
// loop dispatches the reply, before the future settles.
//
// The three func types below are ordinary Go closures, dispatched through the C
// trampoline by ThenGo/ThenFlatGo rather than passed as addresses:
//
//	FutureSuccess runs for a resolved future and returns the child's value plus
//	              a status -- 0 resolves the child, a negative status rejects it
//	FutureError   runs for a rejected future and returns the value plus the
//	              status to propagate; returning 0 resolves the child instead
//	FutureChain   runs for a resolved future and returns the next future, whose
//	              eventual result the child adopts; nil rejects it with -1
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

// A bodyless func needs //go:linkname as well as //go:extern: //go:extern alone
// binds a VARIABLE of that name, which leaves the call site's symbol undefined
// at link time.
//
// FIXME(go-extern-linkname): wasmFutureThen and the wasmIPCFuture* declarations
// below carry //go:extern only, so Future.Then and the IPCFuture methods hit
// that same undefined-symbol failure the moment a Go guest links them.
//
//go:extern wasmos_future_race
//go:linkname wasmFutureRace wasmos_future_race
func wasmFutureRace(*Runtime, *FutureGroup, unsafe.Pointer, uintptr, *Continuation) *Future

//go:extern wasmos_future_all
//go:linkname wasmFutureAll wasmos_future_all
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

// Init zeroes the runtime. Any coroutine still queued on it is forgotten, so
// only init a runtime that is not being driven.
func (r *Runtime) Init() {
	wasmRuntimeInit(r)
}

// Run resumes ready coroutines and dispatches settled continuations until both
// queues are empty, returning the number of coroutine resumptions, or -1 when
// called re-entrantly from inside a task or continuation. Coroutines parked on
// futures nothing settles are left parked.
func (r *Runtime) Run() int32 {
	return wasmCoroutineRun(r)
}

// RunBudget is Run with at most budget coroutine resumptions; queued
// continuations are still drained to exhaustion afterwards, and a budget of 0
// dispatches continuations only.
func (r *Runtime) RunBudget(budget uintptr) int32 {
	return wasmCoroutineRunBudget(r, budget)
}

// Start schedules a C task function on runtime and returns its completion
// future, or nil when the arguments are unusable or the coroutine is neither new
// nor dead. resume is a wasm table address; a Go task uses StartTask instead.
// user is borrowed and must outlive the task.
func (c *Coroutine) Start(runtime *Runtime, resume Callback, user unsafe.Pointer) *Future {
	if runtime == nil || resume == 0 {
		return nil
	}
	return wasmAsyncStart(runtime, c, resume, user)
}

// StartTask uses the C trampoline and the exported Go dispatcher below. taskID
// is caller-assigned, so multiple Go coroutines can be active concurrently.
//
// The dispatcher is not part of this file: the application must itself export
// wasmos_go_coroutine_resume(taskID uint32, out *uintptr) int32 and route taskID
// to its own task registry. It returns the completion future, or nil for a nil
// runtime or a zero taskID.
func (c *Coroutine) StartTask(runtime *Runtime, taskID uint32) *Future {
	if runtime == nil || taskID == 0 {
		return nil
	}
	return wasmGoCoroutineStart(runtime, c, taskID)
}

// RunAsyncApp enters the C-owned application wrapper.  start is called once
// from its root coroutine; it returns the terminal future for the app chain.
//
// It blocks until that future settles: the wrapper alternates one coroutine step
// with one event-loop poll, parking on its reply endpoint whenever the root task
// waits, so it never spins. The return is the chain's resolved value truncated
// to int32; every failure collapses to -1, including a rejected chain, a nil
// start, a start that returns nil, and a second concurrent call (the wrapper
// state is package-level and is never cleared, so RunAsyncApp succeeds once per
// process).
func RunAsyncApp(start func() *Future) int32 {
	if start == nil || goAsyncAppStart != nil {
		return -1
	}
	goAsyncAppStart, goAsyncAppCompletion, goAsyncAppStarted = start, nil, false
	return wasmGoAsyncAppRun()
}

// AsyncAppLoop returns the event loop the wrapper owns, or nil outside a live
// RunAsyncApp.
func AsyncAppLoop() *EventLoop {
	return wasmAsyncAppEventLoop()
}

// AsyncAppReplyEndpoint returns the private endpoint the wrapper created for
// replies, or -1 outside a live RunAsyncApp.
func AsyncAppReplyEndpoint() int32 {
	return wasmAsyncAppReplyEndpoint()
}

// AsyncAppRuntime returns the coroutine runtime the wrapper owns, or nil outside
// a live RunAsyncApp.
func AsyncAppRuntime() *Runtime {
	return wasmAsyncAppRuntime()
}

// wasmosGoAsyncAppResume is the root task the C wrapper resumes. It calls the
// RunAsyncApp start function once, then awaits the chain's terminal future,
// yielding while it is pending. Not called by application code.
//
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

// Join awaits this coroutine's completion from inside another task and returns
// (result, status): status 0 means the coroutine finished and result is its
// value, AwaitPending means the caller was parked and must return TaskYielded,
// and a negative status is the coroutine's failure. Not a blocking join --
// called outside a running coroutine it returns -1.
func (c *Coroutine) Join() (int32, int32) {
	var result int32
	status := wasmCoroutineJoin(c, &result)
	return result, status
}

// Init resets the future to pending and makes p the only handle that can settle
// it. Any coroutine still parked on the previous incarnation is dropped and
// never woken. Nil arguments are ignored.
func (f *Future) Init(p *Promise) {
	if f != nil && p != nil {
		wasmFutureInit(f, p)
	}
}

// Poll returns settled=false while the result is pending. A settled status of
// zero is success; a negative status is failure.
// It neither parks nor consumes the result: a settled future keeps answering.
func (f *Future) Poll() (value uintptr, status int32, settled bool) {
	settled = wasmFuturePoll(f, &status, &value)
	return
}

// Await returns zero or a negative failure status when settled, or AwaitPending
// after registering the currently running stackless coroutine as a waiter.
//
// On AwaitPending the task MUST return TaskYielded straight away without using
// the value: it is resumed from the top when the future settles, so its own
// state machine has to re-enter at the same point. A -1 without parking is the
// runtime refusing the await -- no coroutine is running, or the future belongs
// to another runtime.
func (f *Future) Await() (value uintptr, status int32) {
	status = wasmFutureAwait(f, &value)
	return
}

// Then registers C callbacks (wasm table addresses) on this future and returns
// the child future that settles with their result, or nil for nil arguments.
// The callbacks never run inline: they are queued on the runtime -- at once if
// the future has already settled -- and dispatched from Run/RunBudget. A Go
// callback goes through ThenGo instead.
//
// FIXME(go-extern-linkname): the wasmos_future_then declaration this calls
// carries //go:extern without //go:linkname, so linking a guest that uses Then
// fails on an undefined symbol.
func (f *Future) Then(runtime *Runtime, continuation *Continuation, success Callback, failure Callback, user unsafe.Pointer) *Future {
	if runtime == nil || continuation == nil {
		return nil
	}
	return wasmFutureThen(runtime, f, continuation, success, failure, user)
}

// ThenGo dispatches Go callbacks through the stable C trampoline.  Callbacks
// run from the runtime queue after settlement; no application polling or task
// suspension is exposed.  The returned future carries the callback result.
//
// Exactly one of success/failure runs, chosen by how the source settled, and it
// runs at most once; the registration slot is released as soon as it fires.
// Returns nil for nil arguments, for two nil callbacks, or when all 32 callback
// slots are in use. continuation must stay live until the callback has run.
func (f *Future) ThenGo(runtime *Runtime, continuation *Continuation, success FutureSuccess, failure FutureError) *Future {
	if f == nil || runtime == nil || continuation == nil || (success == nil && failure == nil) {
		return nil
	}
	for i := 0; i < goFutureCallbackMax; i++ {
		if goFutureSuccess[i] == nil && goFutureError[i] == nil && goFutureChain[i] == nil {
			goFutureSuccess[i], goFutureError[i] = success, failure
			return wasmGoFutureThen(runtime, f, continuation, uint32(i+1))
		}
	}
	return nil
}

// ThenFlatGo is ThenGo for a chaining callback: next returns the future whose
// eventual result the returned child future adopts, using adopt as the storage
// for that second registration. A rejected source skips next and forwards the
// status. Both continuation records must stay live until the chain settles;
// returns nil for nil arguments or an exhausted callback table.
func (f *Future) ThenFlatGo(runtime *Runtime, continuation, adopt *Continuation, next FutureChain) *Future {
	if f == nil || runtime == nil || continuation == nil || adopt == nil || next == nil {
		return nil
	}
	for i := 0; i < goFutureCallbackMax; i++ {
		if goFutureSuccess[i] == nil && goFutureError[i] == nil && goFutureChain[i] == nil {
			goFutureChain[i] = next
			return wasmGoFutureThenFlat(runtime, f, continuation, adopt, uint32(i+1))
		}
	}
	return nil
}

// releaseGoCallback frees a registration once its callback has run, so the
// fixed goFutureCallbackMax table cannot fill up permanently and start refusing
// every further ThenGo/ThenFlatGo in a long-running service.
//
// The C runtime dispatches a continuation at most once -- continuation_dispatch
// clears active and future before invoking -- so the slot is dead as soon as any
// of the three entry points below fires. Both the plain and the flat variant are
// covered: wasmos_go_future_then_flat passes ONE id as both the chain and the
// error callback, so a rejected flat chain releases through the error path.
func releaseGoCallback(callbackID uint32) {
	if callbackID == 0 || callbackID > goFutureCallbackMax {
		return
	}
	i := callbackID - 1
	goFutureSuccess[i] = nil
	goFutureError[i] = nil
	goFutureChain[i] = nil
}

// wasmosGoFutureChain is the C trampoline's entry point for a registered
// FutureChain; it returns the next future's address, or 0 when the id is stale.
// Not called by application code.
//
//go:export wasmos_go_future_chain
func wasmosGoFutureChain(callbackID uint32, value uintptr) uintptr {
	if callbackID == 0 || callbackID > goFutureCallbackMax || goFutureChain[callbackID-1] == nil {
		return 0
	}
	next := goFutureChain[callbackID-1](value)
	releaseGoCallback(callbackID)
	return uintptr(unsafe.Pointer(next))
}

// wasmosGoFutureSuccess is the C trampoline's entry point for a registered
// FutureSuccess; a stale id returns -1, rejecting the child. Not called by
// application code.
//
//go:export wasmos_go_future_success
func wasmosGoFutureSuccess(callbackID uint32, value uintptr, out *uintptr) int32 {
	if callbackID == 0 || callbackID > goFutureCallbackMax || goFutureSuccess[callbackID-1] == nil {
		return -1
	}
	value, status := goFutureSuccess[callbackID-1](value)
	releaseGoCallback(callbackID)
	*out = value
	return status
}

// wasmosGoFutureError is the C trampoline's entry point for a registered
// FutureError, and also the error path of a flat chain, where it passes the
// status through. Not called by application code.
//
//go:export wasmos_go_future_error
func wasmosGoFutureError(callbackID uint32, status int32, out *uintptr) int32 {
	if callbackID == 0 || callbackID > goFutureCallbackMax {
		return status
	}
	if goFutureError[callbackID-1] == nil {
		// A flat chain that rejected: the id is registered as a chain callback
		// only, and its error path is this pass-through. Still release it.
		releaseGoCallback(callbackID)
		return status
	}
	value, nextStatus := goFutureError[callbackID-1](status)
	releaseGoCallback(callbackID)
	*out = value
	return nextStatus
}

// Resolve settles the bound future as ready with value, waking every parked
// waiter and queueing every registered continuation. It returns false when there
// is no bound future or it has already settled.
func (p *Promise) Resolve(value uintptr) bool {
	return p != nil && wasmPromiseResolve(p, value)
}

// Reject settles the bound future as failed with status, which must be negative:
// a zero or positive status is refused and returns false, as does an
// already-settled future.
func (p *Promise) Reject(status int32) bool {
	return p != nil && status < 0 && wasmPromiseReject(p, status)
}

// Init binds the loop to receiverEndpoint and seeds its request ids at
// requestIDBase, clearing both tables. It also creates a select set over the
// endpoint so Poll can park; without one, Poll degrades to a non-blocking drain.
// Bases must not collide between loops in one process, since the request id is
// what routes a reply to its intent.
func (loop *EventLoop) Init(receiverEndpoint, requestIDBase int32) {
	if loop != nil {
		wasmEventLoopInit(loop, receiverEndpoint, requestIDBase)
	}
}

// Poll dispatches up to budget messages (0 means 1) and returns how many were
// handled. A message matching an in-flight request id resolves that intent,
// otherwise a type handler runs, otherwise the default handler; anything
// unclaimed is dropped. With nothing queued the first iteration parks on the
// select set instead of spinning.
func (loop *EventLoop) Poll(budget int32) int32 {
	if loop == nil {
		return 0
	}
	return wasmEventLoopPoll(loop, budget)
}

// Init connects this caller-owned operation to a reply-status callback. The
// callback is a wasm table address and returns zero to resolve or a negative
// status to reject.
//
// It also zeroes the record and re-arms its future, so the same IPCFuture can
// serve another round trip. A zero replyStatus accepts any reply.
//
// FIXME(go-extern-linkname): the wasmos_sys_wasm_ipc_future_* declarations this
// and the other IPCFuture methods call carry //go:extern without //go:linkname,
// so linking a guest that uses them fails on an undefined symbol.
func (op *IPCFuture) Init(replyStatus IPCReplyStatus, user unsafe.Pointer) {
	if op != nil {
		wasmIPCFutureInit(op, replyStatus, user)
	}
}

// Send sends the request through loop and returns the future that settles when
// the reply is dispatched, plus the allocated request id.
//
// The future is nil when the record is already in flight or was not re-armed. A
// send that fails still returns the future, already rejected, so a caller that
// chained onto it observes the failure through the same path as a rejecting
// reply. The future resolves with the address of the stored reply.
func (op *IPCFuture) Send(loop *EventLoop, destination, source, msgType, arg0, arg1, arg2, arg3 int32) (*Future, int32) {
	if op == nil || loop == nil {
		return nil, 0
	}
	var requestID int32
	return wasmIPCFutureSend(loop, op, destination, source, msgType, arg0, arg1, arg2, arg3, &requestID), requestID
}

// Cancel stops tracking an in-flight request and rejects its future with status
// (a non-negative status becomes -1). Only local tracking stops: a late reply
// from the peer is then dispatched as an ordinary message.
func (op *IPCFuture) Cancel(status int32) {
	if op != nil {
		wasmIPCFutureCancel(op, status)
	}
}

// Reply returns the stored reply record. It is meaningful only after the future
// settles -- it starts zeroed -- and is overwritten by the next Send.
func (op *IPCFuture) Reply() *IPCFutureReply {
	if op == nil {
		return nil
	}
	return wasmIPCFutureReply(op)
}

// Init re-arms the request with the FS reply validator installed. It settles
// only on FS_IPC_RESP; callers retain any transfer buffers until settlement.
func (request *FSRequest) Init() {
	if request != nil {
		wasmFSRequestInit(request)
	}
}

// Send sends one FS protocol message and returns its future plus the allocated
// request id; see IPCFuture.Send. The future is nil when either endpoint is
// negative or the record is still in flight.
func (request *FSRequest) Send(loop *EventLoop, fsEndpoint, replyEndpoint, msgType, arg0, arg1, arg2, arg3 int32) (*Future, int32) {
	if request == nil || loop == nil {
		return nil, 0
	}
	var requestID int32
	return wasmFSRequestSend(loop, request, fsEndpoint, replyEndpoint, msgType, arg0, arg1, arg2, arg3, &requestID), requestID
}

// Reply returns the stored FS reply; Arg0 carries the FS status or byte count.
// Meaningful only after the future settles.
func (request *FSRequest) Reply() *IPCFutureReply {
	if request == nil {
		return nil
	}
	return wasmFSRequestReply(request)
}

// Init resets the operation and its embedded request. The starters below
// re-initialise the record themselves, so this is only needed to abandon one
// that never ran; an operation still holding a transfer buffer must reach Finish
// first.
func (op *FSOperation) Init() {
	if op != nil {
		wasmFSOperationInit(op)
	}
}

// Open submits an open request for the NUL-terminated path and returns its
// future plus the request id. path must include its terminator and stays
// caller-owned; the bytes are copied into a transfer buffer during this call.
// A nil future means the operation could not be started (record busy, no
// transfer buffer, send refused).
func (op *FSOperation) Open(loop *EventLoop, endpoint, reply int32, path []byte, flags int32) (*Future, int32) {
	var id int32
	if op == nil || len(path) == 0 {
		return nil, 0
	}
	return wasmFSOpenAsync(loop, op, endpoint, reply, &path[0], flags, &id), id
}

// Read submits a read of up to len(dst) bytes for fd. dst is not written here:
// the payload is copied out by Finish once the future has settled, so dst must
// stay live until then. One request, so the byte count may be short.
func (op *FSOperation) Read(loop *EventLoop, endpoint, reply, fd int32, dst []byte) (*Future, int32) {
	var id int32
	if op == nil || len(dst) == 0 {
		return nil, 0
	}
	return wasmFSReadAsync(loop, op, endpoint, reply, fd, &dst[0], int32(len(dst)), &id), id
}

// Write submits a write of src to fd. src is copied into the transfer buffer
// during this call, so it need only be live here; it must be backed by array
// storage rather than a heap slice (see WriteAsync). Not chunked: one buffer is
// acquired for the whole payload.
func (op *FSOperation) Write(loop *EventLoop, endpoint, reply, fd int32, src []byte) (*Future, int32) {
	var id int32
	if op == nil || len(src) == 0 {
		return nil, 0
	}
	return wasmFSWriteAsync(loop, op, endpoint, reply, fd, &src[0], int32(len(src)), &id), id
}

// Close submits a close of fd. It uses no transfer buffer; the reply's status
// comes back through Finish.
func (op *FSOperation) Close(loop *EventLoop, endpoint, reply, fd int32) (*Future, int32) {
	var id int32
	if op == nil {
		return nil, 0
	}
	return wasmFSCloseAsync(loop, op, endpoint, reply, fd, &id), id
}

// Unlink submits a remove of the NUL-terminated path; see Open for the path
// contract. A refusal by the backend rejects the future rather than returning a
// status.
func (op *FSOperation) Unlink(loop *EventLoop, endpoint, reply int32, path []byte) (*Future, int32) {
	var id int32
	if op == nil || len(path) == 0 {
		return nil, 0
	}
	return wasmFSUnlinkAsync(loop, op, endpoint, reply, &path[0], &id), id
}

// Stat submits a stat of the NUL-terminated path; see Open for the path
// contract. On success Finish returns the file size and the reply's Arg1 carries
// the mode bits.
func (op *FSOperation) Stat(loop *EventLoop, endpoint, reply int32, path []byte) (*Future, int32) {
	var id int32
	if op == nil || len(path) == 0 {
		return nil, 0
	}
	return wasmFSStatAsync(loop, op, endpoint, reply, &path[0], &id), id
}

// Finish copies a settled read payload into dst, copies the reply into reply
// when non-nil, releases the operation's transfer buffer (idempotently) and
// returns the reply's Arg0 -- an fd for open, a byte count for read/write, a
// size for stat -- or -1 when the payload could not be copied out.
//
// Call it only after the future has settled: the stored reply starts zeroed, so
// an early call reports 0 rather than a real status. An empty dst skips the copy
// and only reports the status.
func (op *FSOperation) Finish(dst []byte, reply *IPCFutureReply) int32 {
	if op == nil {
		return -1
	}
	if len(dst) == 0 {
		return wasmFSOperationFinish(op, nil, 0, reply)
	}
	return wasmFSOperationFinish(op, &dst[0], int32(len(dst)), reply)
}

// Result copies out a settled read payload and returns the operation's status
// or byte count; see FSOperation.Finish for the exact value and the
// call-after-settlement rule. The reply stays available through the operation's
// own record.
func (op *AsyncFSOperation) Result() int32 {
	if op == nil {
		return -1
	}
	return op.operation.Finish(op.read, &op.reply)
}

// Map chains a step that starts another operation: next receives this settled
// operation and returns the next one, whose future the returned future adopts.
// A nil next result rejects the chain with -1. The callback runs from the
// runtime's continuation queue, never inline, and only when this operation
// resolved -- a rejection forwards the status instead.
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
//
// The callback runs from the runtime's continuation queue after this operation
// resolves, never inline, and is skipped on rejection. Returns nil for a nil
// receiver, an operation that never started, or a nil callback. The operation
// holds one continuation pair, so only one Then or Map registration can be live
// on it at a time.
func (op *AsyncFSOperation) Then(next func(*AsyncFSOperation) *Future) *Future {
	if op == nil || op.future == nil || next == nil {
		return nil
	}
	return op.future.ThenFlatGo(AsyncAppRuntime(), &op.continuation, &op.adopt, func(_ uintptr) *Future { return next(op) })
}

// CatchGo registers only an error callback: a resolved source passes its value
// through to the child untouched, and a rejected one reaches failure, which may
// return 0 to convert the rejection into a resolved child. Shorthand for ThenGo
// with a nil success callback.
func (f *Future) CatchGo(runtime *Runtime, continuation *Continuation, failure FutureError) *Future {
	return f.ThenGo(runtime, continuation, nil, failure)
}

func asyncFSOperation() *AsyncFSOperation {
	return &AsyncFSOperation{}
}

// OpenAsync submits an open of path with flags through the RunAsyncApp wrapper's
// event loop and reply endpoint, returning a chainable operation or nil.
//
// The six *Async starters below allocate a fresh AsyncFSOperation per call and
// are only usable inside a live RunAsyncApp: outside it the wrapper has no loop
// or reply endpoint and the request cannot be sent. None of them blocks; the
// outcome is taken with Result once the operation settles, and a backend refusal
// arrives as an FS error message that rejects the future with -1, losing the
// packed WASMOS_ERR_FS_* code. path is copied into the operation and must be
// non-empty and under 256 bytes.
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

// ReadAsync submits a read of up to len(dst) bytes for fd. dst is retained by
// the operation and written by Result, so it must stay live until then; nil for
// an empty dst. One request, so the count may be short of len(dst).
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

// WriteAsync submits a write of src to fd, copying it into the operation's own
// staging array first. src need only be live for this call; nil when it is empty
// or longer than the 256-byte staging array.
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

// CloseAsync submits a close of fd; Result returns the reply's status.
func (fsAPI) CloseAsync(fd int32) *AsyncFSOperation {
	op := asyncFSOperation()
	op.future, _ = op.operation.Close(AsyncAppLoop(), fsEndpoint(), AsyncAppReplyEndpoint(), fd)
	if op.future == nil {
		return nil
	}
	return op
}

// UnlinkAsync submits a remove of path; a refusal by the backend rejects the
// operation's future.
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

// StatAsync submits a stat of path; the operation rejects when the path does not
// exist, and on success Result returns the file size.
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

// Race settles with the first input result, success or failure. inputs and
// continuations must have identical non-zero lengths; the group storage,
// including this slice pair, must stay live until the returned future settles,
// at which point the runtime releases the continuations still registered on the
// losing inputs (see wasmos_future_group in coroutine_wasm.h).
func (g *FutureGroup) Race(runtime *Runtime, inputs []*Future, continuations []Continuation) *Future {
	if runtime == nil || len(inputs) == 0 || len(inputs) != len(continuations) {
		return nil
	}
	return wasmFutureRace(runtime, g, unsafe.Pointer(&inputs[0]), uintptr(len(inputs)), &continuations[0])
}

// All settles successfully once every input succeeds, with values receiving each
// result in input order, or rejects on the first failure. All three slices must
// have identical non-zero lengths and stay live until the returned future
// settles; a rejection releases the continuations still registered on the
// inputs that had not settled yet.
func (g *FutureGroup) All(runtime *Runtime, inputs []*Future, values []uintptr, continuations []Continuation) *Future {
	if runtime == nil || len(inputs) == 0 || len(inputs) != len(values) || len(inputs) != len(continuations) {
		return nil
	}
	return wasmFutureAll(runtime, g, unsafe.Pointer(&inputs[0]), uintptr(len(inputs)), &values[0], &continuations[0])
}
