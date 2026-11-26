/* syscall.h - Ring-3 to kernel syscall ABI.
 *
 * The architecture's syscall entry (int 0x80 on x86_64: isr_syscall_128) saves
 * the register frame as a syscall_frame_t and calls syscall_dispatch().
 * All pointer and numeric arguments from user space are validated before use.
 * See the detailed per-syscall contract in the block comment below. */
#ifndef WASMOS_SYSCALL_H
#define WASMOS_SYSCALL_H

#include <stdint.h>
#include "arch/arch_context.h"

typedef enum {
    WASMOS_SYSCALL_NOP = 0,
    WASMOS_SYSCALL_GETPID = 1,
    WASMOS_SYSCALL_EXIT = 2,
    WASMOS_SYSCALL_YIELD = 3,
    WASMOS_SYSCALL_WAIT = 4,
    WASMOS_SYSCALL_IPC_NOTIFY = 5,
    WASMOS_SYSCALL_IPC_CALL = 6,
    WASMOS_SYSCALL_GETTID = 7,
    WASMOS_SYSCALL_THREAD_YIELD = 8,
    WASMOS_SYSCALL_THREAD_EXIT = 9,
    WASMOS_SYSCALL_THREAD_CREATE = 10,
    WASMOS_SYSCALL_THREAD_JOIN = 11,
    WASMOS_SYSCALL_THREAD_DETACH = 12,
    WASMOS_SYSCALL_NOTIFY_READY = 13,
    WASMOS_SYSCALL_MUTEX_TRY_LOCK = 14,
    WASMOS_SYSCALL_MUTEX_UNLOCK = 15,
    /* Ring-3 WARP execution returns here via the return trampoline. */
    WASMOS_SYSCALL_WARP_RETURN = 16,
    /* Ring-3 WARP memory helper trampoline enters here. */
    WASMOS_SYSCALL_WARP_MEMORY_HELPER = 17
} wasmos_syscall_id_t;

/* Syscall ABI (current minimal contract), in architecture-neutral slots. The
 * architecture maps them onto registers (x86_64 int 0x80: see
 * include/arch/x86_64/arch_context.h) and the handler reads them through
 * arch_syscall_number / arch_syscall_arg / arch_syscall_set_result /
 * arch_syscall_set_result2.
 * - number: the syscall id (see wasmos_syscall_id_t)
 * - arg0..arg5: syscall-specific arguments
 * - return:
 *   - result: primary return value (0 or positive success value, negative error)
 *   - result2: secondary value for the calls that need one, because a full-range
 *     32-bit result cannot share result with a negative error code (IPC_CALL reply
 *     arg0, THREAD_JOIN exit status). Cleared before every such call, so an
 *     error return never leaves a stale value there.
 *
 * Current syscall argument/return contract:
 * - NOP:            result=0
 * - GETPID:         result=pid
 * - EXIT:           arg0=exit_status; does not return to caller path
 * - YIELD:          result=0
 * - WAIT:           arg0=child_pid; result=child_exit_status or -1
 * - IPC_NOTIFY:     arg0=endpoint(32-bit clean); result=ipc_result_t
 * - IPC_CALL:       arg0=endpoint, arg1=type, arg2..arg5=message arg0..arg3
 *                   all IPC_CALL fields are validated as 32-bit-clean
 *                   result=ipc_result_t (0 on success), result2=reply arg0 on success
 *                   result2=0 on error
 *                   current behavior blocks by yielding until a reply with a
 *                   matching request_id is received
 *                   current reply ABI returns arg0 only (arg1..arg3 ignored)
 * - GETTID:         result=tid
 * - THREAD_YIELD:   result=0
 * - THREAD_EXIT:    arg0=exit_status; exits the current thread. If it is the
 *                   final live thread in the process, process exit follows.
 * - THREAD_CREATE:  arg0=entry_pc, arg1=user_stack_top; result=tid on success.
 *                   New thread is created in the caller process with user-mode
 *                   user entry/stack/mode/root-table context initialized per-thread.
 * - THREAD_JOIN:    arg0=target_tid; result=0 and result2=target_exit_status on success,
 *                   result=a negative packed WASMOS_ERR_* / WASMOS_INVAL on error
 *                   with result2=0. The exit status is guest-chosen and spans the
 *                   full 32-bit range, so it cannot share result with the outcome:
 *                   a thread exiting -1 would be indistinguishable from a failed
 *                   join. Calling thread blocks until target exits.
 * - THREAD_DETACH:  arg0=target_tid; result=0 on success, -1 on error. Detached
 *                   threads are not joinable and are reaped automatically.
 * - NOTIFY_READY:   no args; result=0. Marks the calling process as ready so that
 *                   a parent blocked in PROC_IPC_SPAWN_SYNC is unblocked.
 *                   If no parent is waiting the flag is stored and checked on
 *                   the next pm_poll_spawn iteration.
 * - MUTEX_TRY_LOCK: arg0=user_mutex_ptr; result=0 on acquire, 1 if busy, -1 on
 *                   invalid pointer/state. The mutex object is process-local
 *                   and reentrant for the current thread ID.
 * - MUTEX_UNLOCK:   arg0=user_mutex_ptr; result=0 on success, -1 on invalid
 *                   pointer or non-owner unlock.
 */
/* Dispatch one syscall. Returns the value the arch entry stub hands back as
 * `result` (the per-syscall contract above), or (uint64_t)-1 for a NULL frame. Blocks for as
 * long as the requested operation does -- WAIT, IPC_CALL and THREAD_JOIN all
 * park the calling thread -- and EXIT/THREAD_EXIT may not return through the
 * caller's path at all. On entry from ring 3 it first snapshots the frame into
 * the current thread's saved context so a blocking syscall resumes at the right
 * post-syscall instruction, possibly on another CPU. */
uint64_t syscall_dispatch(syscall_frame_t* frame);
/* Test-only wiring for the ring-3 syscall suites, honoured by the IPC_CALL and
 * IPC_NOTIFY paths and unset (IPC_ENDPOINT_NONE) in a normal boot.
 *
 * The echo endpoint makes IPC_CALL answer certain message types from inside the
 * kernel instead of forwarding them, so a ring-3 probe can exercise the call
 * path with no peer process. The two deny endpoints name an endpoint whose
 * refusal should emit the one-shot "control deny ok" marker, which is how the
 * suite observes that an ownership check fired. Global, not per-process. */
void syscall_set_ipc_call_echo_endpoint(uint32_t endpoint);
uint32_t syscall_ipc_call_echo_endpoint(void);
void syscall_set_ipc_call_control_deny_endpoint(uint32_t endpoint);
void syscall_set_ipc_notify_control_deny_endpoint(uint32_t endpoint);

#ifdef WASMOS_SYSCALL_TEST_SEAMS
/* The per-process pending reply ring, so a test can exercise its retention
 * directly; see the definitions in syscall.c for why that is not reachable
 * through syscall_dispatch alone. */
#include "ipc.h"
/* Park a reply in pid's ring. Returns 0, or -1 when no slot could be obtained
 * or msg->request_id has not been issued yet -- the ring deliberately refuses
 * to hold a reply to a call that does not exist. Overflow drops the oldest. */
int syscall_test_pending_enqueue(uint32_t pid, const ipc_message_t* msg);
/* Remove the reply matching request_id. Returns 0 with *out filled, or -1 if
 * the ring holds no such reply. */
int syscall_test_pending_take(uint32_t pid, uint32_t request_id, ipc_message_t* out);
/* The next request id to be issued, so a test can construct ids that are and
 * are not yet valid. */
uint32_t syscall_test_next_request_id(void);
/* Issue one request id through the path IPC_CALL uses. Never 0. */
uint32_t syscall_test_issue_request_id(void);
#endif

#endif
