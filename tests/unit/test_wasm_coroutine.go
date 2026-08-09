// test_wasm_coroutine.go - the Go binding for the shared C coroutine runtime.
//
// This is the one binding that could not be tested the way the others are. The
// Rust and Zig suites run on the HOST, linking coroutine_wasm.c directly, which
// works because their #[repr(C)] / extern struct layouts are computed per
// target. Go cannot express the C layout at all: it models every record as an
// opaque [N]uint32 blob sized for wasm32, so a host test would exercise 64-bit
// pointers and a different ABI entirely. It has to be built for wasm32 and run
// there, which is what tests/unit/go/run_go_test.mjs does.
//
// Go also carries an FFI layer the others do not, and it is the part most
// likely to break:
//
//   - Callback is a raw uintptr, because "Go has no portable representation for
//     a C function pointer", so coroutines route through the C trampoline in
//     go_coroutine_trampoline.c with a caller-assigned task id.
//   - Go closures reach the runtime through a fixed 32-slot registry keyed by
//     callback id.
//
// Neither had any coverage. The scenarios below mirror
// tests/unit/test_wasm_coroutine.c and add the two FFI-specific ones.
//
// Each case returns 0 or a marker; runTests returns the first non-zero.

package main

import "unsafe"

// wasmos.go owns main() and dispatches to Main(); the test entry point is the
// exported runTests below.
func Main(args []string) int32 { return 0 }

// ---------------------------------------------------------------- FFI smoke

// The blob sizes are pinned at compile time by tests/unit/test_go_abi_sizes.c.
// This checks the other half: that values written by C through those pointers
// are read back correctly from Go, i.e. the field OFFSETS agree too. A size can
// be right while the layout is skewed.
func testFFISmoke() int32 {
	var rt Runtime
	rt.Init()

	var future Future
	var promise Promise
	future.Init(&promise)

	// Pending: C reports not-settled and leaves the outputs alone.
	if _, _, settled := future.Poll(); settled {
		return 110
	}
	// A value written by C is read back intact through the blob.
	if !promise.Resolve(0xABCD1234) {
		return 111
	}
	value, status, settled := future.Poll()
	if !settled || status != 0 || value != 0xABCD1234 {
		return 112
	}
	// A settled future refuses to settle again, so the state field is real.
	if promise.Resolve(1) {
		return 113
	}
	// Rejection carries a negative status through the same path.
	var rejected Future
	var rejectedPromise Promise
	rejected.Init(&rejectedPromise)
	if rejectedPromise.Reject(0) {
		return 114
	}
	if !rejectedPromise.Reject(-77) {
		return 115
	}
	if _, status, settled = rejected.Poll(); !settled || status != -77 {
		return 116
	}
	// An empty runtime drains to zero rather than trapping.
	if rt.Run() != 0 {
		return 117
	}
	return 0
}

// ------------------------------------------------------- trampoline tasks

// Coroutines reach Go through wasmos_go_coroutine_resume, dispatched on a
// caller-assigned task id. The registry below is the test's own; the binding
// only carries the id.
type goTask struct {
	pc     int32
	events *[]uint32
	future *Future
	status int32
	value  uintptr
	result uintptr
	mode   int32
}

const (
	modeYield = iota
	modeWait
	modeResolve
	modeStress
)

var goTasks [64]*goTask
var goResolvePromise *Promise
var goStressCompleted int32

//go:export wasmos_go_coroutine_resume
func wasmosGoCoroutineResume(taskID uint32, out *uintptr) int32 {
	if taskID == 0 || int(taskID) > len(goTasks) || goTasks[taskID-1] == nil {
		return -1
	}
	t := goTasks[taskID-1]
	switch t.mode {
	case modeYield:
		if t.pc == 0 {
			*t.events = append(*t.events, 1)
			t.pc = 1
			return TaskYielded
		}
		*t.events = append(*t.events, 3)
		*out = 7
		return TaskComplete
	case modeWait:
		t.value, t.status = t.future.Await()
		if t.status == AwaitPending {
			return TaskYielded
		}
		*out = t.value
		return t.status
	case modeResolve:
		if !goResolvePromise.Resolve(42) {
			return -1
		}
		*out = 42
		return TaskComplete
	case modeStress:
		if t.pc > 0 {
			t.pc--
			return TaskYielded
		}
		goStressCompleted++
		*out = 0
		return TaskComplete
	}
	return -1
}

func registerTask(id uint32, t *goTask) { goTasks[id-1] = t }

// ------------------------------------------------------------------ battery

func testYieldAwaitAndJoin() int32 {
	for i := range goTasks {
		goTasks[i] = nil
	}
	var rt Runtime
	rt.Init()
	var source Future
	var promise Promise
	source.Init(&promise)
	goResolvePromise = &promise

	events := make([]uint32, 0, 4)
	yielder := &goTask{mode: modeYield, events: &events}
	waiter := &goTask{mode: modeWait, future: &source}
	resolver := &goTask{mode: modeResolve}
	registerTask(1, yielder)
	registerTask(2, waiter)
	registerTask(3, resolver)

	var first, waiterCo, resolverCo Coroutine
	if first.StartTask(&rt, 1) == nil {
		return 210
	}
	if waiterCo.StartTask(&rt, 2) == nil {
		return 211
	}
	if resolverCo.StartTask(&rt, 3) == nil {
		return 212
	}
	// Five resumes: yielder twice, waiter twice, resolver once.
	if rt.Run() != 5 {
		return 213
	}
	if len(events) != 2 || events[0] != 1 || events[1] != 3 {
		return 214
	}
	if waiter.status != 0 || waiter.value != 42 {
		return 215
	}
	result, status := first.Join()
	if status != 0 || result != 7 {
		return 216
	}
	if promise.Resolve(9) {
		return 217
	}
	return 0
}

func testMultipleWaitersAndJoiners() int32 {
	for i := range goTasks {
		goTasks[i] = nil
	}
	var rt Runtime
	rt.Init()
	var source Future
	var promise Promise
	source.Init(&promise)

	const waiters = 4
	tasks := make([]*goTask, waiters)
	cos := make([]Coroutine, waiters)
	for i := 0; i < waiters; i++ {
		tasks[i] = &goTask{mode: modeWait, future: &source}
		registerTask(uint32(i+1), tasks[i])
		if cos[i].StartTask(&rt, uint32(i+1)) == nil {
			return 310
		}
	}
	// All four park on the first drain.
	if rt.Run() != waiters {
		return 311
	}
	if !promise.Resolve(77) {
		return 312
	}
	// Settling wakes every one of them, not just the head of the wait list.
	if rt.Run() != waiters {
		return 313
	}
	for i := 0; i < waiters; i++ {
		if tasks[i].status != 0 || tasks[i].value != 77 {
			return 314
		}
	}
	return 0
}

func testSchedulerStress() int32 {
	for i := range goTasks {
		goTasks[i] = nil
	}
	var rt Runtime
	rt.Init()
	const count = 8
	const yields = 4
	goStressCompleted = 0
	cos := make([]Coroutine, count)
	for i := 0; i < count; i++ {
		registerTask(uint32(i+1), &goTask{mode: modeStress, pc: yields})
		if cos[i].StartTask(&rt, uint32(i+1)) == nil {
			return 410
		}
	}
	// The exact resume count is the schedule.
	if rt.Run() != count*(yields+1) {
		return 411
	}
	if goStressCompleted != count {
		return 412
	}
	return 0
}

func testChainsAndDeferral() int32 {
	var rt Runtime
	rt.Init()
	calls := 0

	var source Future
	var promise Promise
	source.Init(&promise)
	var plusOne Continuation
	child := source.ThenGo(&rt, &plusOne, func(v uintptr) (uintptr, int32) {
		calls++
		return v + 1, 0
	}, nil)
	if child == nil {
		return 510
	}
	if !promise.Resolve(20) {
		return 511
	}
	// Deferred: resolving does not run the callback, the runtime does.
	if calls != 0 {
		return 512
	}
	if rt.Run() != 0 || calls != 1 {
		return 513
	}
	if v, s, ok := child.Poll(); !ok || s != 0 || v != 21 {
		return 514
	}

	// Error recovery through the Go error dispatcher.
	var rejected Future
	var rejectedPromise Promise
	rejected.Init(&rejectedPromise)
	var recover Continuation
	child = rejected.CatchGo(&rt, &recover, func(status int32) (uintptr, int32) {
		if status >= 0 {
			return 0, -1
		}
		return 55, 0
	})
	if child == nil {
		return 520
	}
	if !rejectedPromise.Reject(-23) || rejectedPromise.Reject(-24) {
		return 521
	}
	if rt.Run() != 0 {
		return 522
	}
	if v, s, ok := child.Poll(); !ok || s != 0 || v != 55 {
		return 523
	}

	// Registering on an ALREADY-SETTLED future still defers.
	var settled Future
	var settledPromise Promise
	settled.Init(&settledPromise)
	if !settledPromise.Resolve(70) {
		return 530
	}
	late := 0
	var lateContinuation Continuation
	child = settled.ThenGo(&rt, &lateContinuation, func(v uintptr) (uintptr, int32) {
		late++
		return v + 1, 0
	}, nil)
	if child == nil || late != 0 {
		return 531
	}
	if rt.Run() != 0 || late != 1 {
		return 532
	}
	if v, _, ok := child.Poll(); !ok || v != 71 {
		return 533
	}
	return 0
}

// Race with three candidates that all settle, once per winning position, with
// the losers required to be discarded rather than merely outvoted.
func raceWinnerCase(winner int, base int32) int32 {
	var rt Runtime
	rt.Init()
	futures := make([]Future, 3)
	promises := make([]Promise, 3)
	inputs := make([]*Future, 3)
	continuations := make([]Continuation, 3)
	for i := 0; i < 3; i++ {
		futures[i].Init(&promises[i])
		inputs[i] = &futures[i]
	}
	var group FutureGroup
	result := group.Race(&rt, inputs, continuations)
	if result == nil {
		return base + 0
	}
	expected := uintptr(100 + winner)
	if !promises[winner].Resolve(expected) {
		return base + 1
	}
	for i := 2; i >= 0; i-- {
		if i == winner {
			continue
		}
		if !promises[i].Resolve(uintptr(900 + i)) {
			return base + 2
		}
	}
	if rt.Run() != 0 {
		return base + 3
	}
	v, s, ok := result.Poll()
	if !ok || s != 0 || v != expected {
		return base + 4
	}
	// Losers settling later must not disturb the result.
	if rt.Run() != 0 {
		return base + 5
	}
	if v, _, ok = result.Poll(); !ok || v != expected {
		return base + 6
	}
	return 0
}

func testRaceEveryWinner() int32 {
	if rc := raceWinnerCase(0, 600); rc != 0 {
		return rc
	}
	if rc := raceWinnerCase(1, 700); rc != 0 {
		return rc
	}
	return raceWinnerCase(2, 800)
}

func testAll() int32 {
	var rt Runtime
	rt.Init()
	futures := make([]Future, 3)
	promises := make([]Promise, 3)
	inputs := make([]*Future, 3)
	continuations := make([]Continuation, 3)
	values := make([]uintptr, 3)
	for i := 0; i < 3; i++ {
		futures[i].Init(&promises[i])
		inputs[i] = &futures[i]
	}
	var group FutureGroup
	result := group.All(&rt, inputs, values, continuations)
	if result == nil {
		return 910
	}
	// Settled out of order; values must land in INPUT order.
	if !promises[2].Resolve(3) || !promises[0].Resolve(1) || !promises[1].Resolve(2) {
		return 911
	}
	if rt.Run() != 0 {
		return 912
	}
	v, s, ok := result.Poll()
	if !ok || s != 0 || v != uintptr(unsafe.Pointer(&values[0])) {
		return 913
	}
	if values[0] != 1 || values[1] != 2 || values[2] != 3 {
		return 914
	}
	return 0
}

// The 32-slot Go callback registry. Every ThenGo takes a slot; before slots
// were reclaimed on dispatch the table filled permanently and ThenGo returned
// nil forever after, so a long-running service stopped chaining after its 32nd
// callback with no diagnostic.
func testCallbackRegistryReclaim() int32 {
	var rt Runtime
	rt.Init()
	const rounds = 32*3 + 5
	for round := 0; round < rounds; round++ {
		var source Future
		var promise Promise
		source.Init(&promise)
		var continuation Continuation
		fired := false
		child := source.ThenGo(&rt, &continuation, func(v uintptr) (uintptr, int32) {
			fired = true
			return v + 1, 0
		}, nil)
		if child == nil {
			// Exactly the failure the reclaim exists to prevent.
			return 1000 + int32(round)
		}
		if !promise.Resolve(uintptr(round)) {
			return 1100 + int32(round)
		}
		if rt.Run() != 0 || !fired {
			return 1200 + int32(round)
		}
		if v, s, ok := child.Poll(); !ok || s != 0 || v != uintptr(round+1) {
			return 1300 + int32(round)
		}
	}
	// A rejected chain releases through the error path, not only the success one.
	for round := 0; round < rounds; round++ {
		var source Future
		var promise Promise
		source.Init(&promise)
		var continuation Continuation
		child := source.CatchGo(&rt, &continuation, func(status int32) (uintptr, int32) {
			return 5, 0
		})
		if child == nil {
			return 1400 + int32(round)
		}
		if !promise.Reject(-3) {
			return 1500 + int32(round)
		}
		if rt.Run() != 0 {
			return 1600 + int32(round)
		}
		if v, s, ok := child.Poll(); !ok || s != 0 || v != 5 {
			return 1700 + int32(round)
		}
	}
	return 0
}

//go:wasmexport runTests
func runTests() int32 {
	if rc := testFFISmoke(); rc != 0 {
		return rc
	}
	if rc := testYieldAwaitAndJoin(); rc != 0 {
		return rc
	}
	if rc := testMultipleWaitersAndJoiners(); rc != 0 {
		return rc
	}
	if rc := testSchedulerStress(); rc != 0 {
		return rc
	}
	if rc := testChainsAndDeferral(); rc != 0 {
		return rc
	}
	if rc := testRaceEveryWinner(); rc != 0 {
		return rc
	}
	if rc := testAll(); rc != 0 {
		return rc
	}
	return testCallbackRegistryReclaim()
}
