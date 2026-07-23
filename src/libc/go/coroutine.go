package main

import "unsafe"

// These types are caller-owned wasm32 ABI records for the shared C coroutine
// implementation. Keep their storage in uint32 words: C accesses them through
// pointers and therefore requires four-byte alignment.
type Runtime struct{ storage [6]uint32 }
type Future struct{ storage [6]uint32 }
type Promise struct{ storage [1]uint32 }
type Coroutine struct{ storage [13]uint32 }
type Continuation struct{ storage [15]uint32 }
type FutureGroup struct{ storage [14]uint32 }
type EventLoop struct{ storage [133]uint32 }
type IPCFuture struct{ storage [20]uint32 }
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

//go:extern wasmos_wasm_runtime_init
func wasmRuntimeInit(*Runtime)

//go:extern wasmos_async_start
func wasmAsyncStart(*Runtime, *Coroutine, Callback, unsafe.Pointer) *Future

//go:extern wasmos_wasm_coroutine_run
func wasmCoroutineRun(*Runtime) int32

//go:extern wasmos_wasm_coroutine_run_budget
func wasmCoroutineRunBudget(*Runtime, uintptr) int32

//go:extern wasmos_wasm_coroutine_join
func wasmCoroutineJoin(*Coroutine, *int32) int32

//go:extern wasmos_future_init
func wasmFutureInit(*Future, *Promise)

//go:extern wasmos_future_poll
func wasmFuturePoll(*Future, *int32, *uintptr) bool

//go:extern wasmos_future_await
func wasmFutureAwait(*Future, *uintptr) int32

//go:extern wasmos_promise_resolve
func wasmPromiseResolve(*Promise, uintptr) bool

//go:extern wasmos_promise_reject
func wasmPromiseReject(*Promise, int32) bool

//go:extern wasmos_future_then
func wasmFutureThen(*Runtime, *Future, *Continuation, Callback, Callback, unsafe.Pointer) *Future

//go:extern wasmos_future_race
func wasmFutureRace(*Runtime, *FutureGroup, unsafe.Pointer, uintptr, *Continuation) *Future

//go:extern wasmos_future_all
func wasmFutureAll(*Runtime, *FutureGroup, unsafe.Pointer, uintptr, *uintptr, *Continuation) *Future

//go:extern wasmos_sys_event_loop_init
func wasmEventLoopInit(*EventLoop, int32, int32)

//go:extern wasmos_sys_event_loop_poll
func wasmEventLoopPoll(*EventLoop, int32) int32

//go:extern wasmos_sys_wasm_ipc_future_init
func wasmIPCFutureInit(*IPCFuture, IPCReplyStatus, unsafe.Pointer)

//go:extern wasmos_sys_wasm_ipc_future_send
func wasmIPCFutureSend(*EventLoop, *IPCFuture, int32, int32, int32, int32, int32, int32, int32, *int32) *Future

//go:extern wasmos_sys_wasm_ipc_future_cancel
func wasmIPCFutureCancel(*IPCFuture, int32)

//go:extern wasmos_sys_wasm_ipc_future_reply
func wasmIPCFutureReply(*IPCFuture) *IPCFutureReply

func (r *Runtime) Init()                          { wasmRuntimeInit(r) }
func (r *Runtime) Run() int32                     { return wasmCoroutineRun(r) }
func (r *Runtime) RunBudget(budget uintptr) int32 { return wasmCoroutineRunBudget(r, budget) }

func (c *Coroutine) Start(runtime *Runtime, resume Callback, user unsafe.Pointer) *Future {
	if runtime == nil || resume == 0 {
		return nil
	}
	return wasmAsyncStart(runtime, c, resume, user)
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

func (p *Promise) Resolve(value uintptr) bool { return p != nil && wasmPromiseResolve(p, value) }
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
