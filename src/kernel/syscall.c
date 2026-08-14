/* syscall.c - Ring-3 int 0x80 syscall dispatch.
 * x86_syscall_handler() is called from isr_syscall_128 in cpu_isr.S with RAX
 * selecting the call.  Arguments arrive in the 64-bit registers but the ABI is
 * 32-bit (see syscall.h): syscall_arg_u32/_i32 reject a value that does not fit,
 * so a caller cannot smuggle bits through the upper half.  Entries that name a
 * kernel object (endpoint, pid, tid) resolve it against the calling process's
 * context and refuse anything it does not own; the WARP hostcall range is
 * forwarded to warp_ring3_dispatch, which does its own validation. */
#include "syscall.h"
#include "klog.h"
#include "ipc.h"
#include "memory.h"
#include "process.h"
#include "thread.h"
#include "string.h"
#include "paging.h"
#include "serial.h"
#include "user_mutex.h"
#ifdef WASMOS_WASM_RUNTIME_WARP
#include "warp_ring3.h"
#include "arch/x86_64/smp.h"
extern uint64_t warp_r3_memory_helper(uint64_t min_linmem_len, uint32_t basedata_len,
                                      uint64_t original_linmem_user_va);
#endif

static uint8_t g_ring3_syscall_logged;
static uint8_t g_ring3_stress_ok_logged;
static uint32_t g_ring3_getpid_count;
static uint8_t g_ring3_ipc_deny_logged;
static uint8_t g_ring3_ipc_arg_width_deny_logged;
static uint8_t g_ring3_ipc_ok_logged;
static uint8_t g_ring3_ipc_control_deny_logged;
static uint8_t g_ring3_ipc_call_deny_logged;
static uint8_t g_ring3_ipc_call_perm_deny_logged;
static uint8_t g_ring3_ipc_call_control_deny_logged;
static uint8_t g_ring3_ipc_call_control_endpoint_deny_logged;
static uint8_t g_ring3_ipc_call_ok_logged;
static uint8_t g_ring3_ipc_call_err_rdx_zero_logged;
static uint8_t g_ring3_ipc_call_correlation_logged;
static uint8_t g_ring3_ipc_call_out_of_order_retain_logged;
static uint8_t g_ring3_ipc_call_spoof_invalid_source_deny_logged;
static uint8_t g_ring3_ipc_call_owner_sender_stress_logged;
static uint8_t g_ring3_yield_logged;
static uint8_t g_ring3_thread_yield_logged;
static uint8_t g_ring3_thread_exit_logged;
static uint8_t g_ring3_native_abi_logged;
static uint8_t g_ring3_native_gettid_logged;
static uint8_t g_ring3_thread_create_logged;
static uint8_t g_ring3_thread_join_logged;
static uint8_t g_ring3_thread_join_self_deny_logged;
static uint8_t g_ring3_thread_join_helper_ok_logged;
static uint8_t g_ring3_thread_detach_logged;
static uint8_t g_ring3_thread_detach_invalid_deny_logged;
static uint8_t g_ring3_thread_detach_helper_ok_logged;
static uint8_t g_ring3_thread_detach_join_deny_logged;
/* Monotonic request-id source for IPC_CALL, and the anchor
 * syscall_ipc_request_id_issued compares against.
 * FIXME: incremented non-atomically, and the per-pid slot table below is walked
 * without a lock.  Two CPUs issuing IPC_CALL concurrently can hand out the same
 * request id, which the reply-correlation logic assumes is unique. */
static uint32_t g_syscall_ipc_call_next_request_id = 1;
static uint32_t g_ipc_call_echo_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_ipc_call_control_deny_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_ipc_notify_control_deny_endpoint = IPC_ENDPOINT_NONE;
#define SYSCALL_IPC_PENDING_DEPTH 8u
#define USER_CS_SELECTOR 0x1Bu
#define USER_DS_SELECTOR 0x23u

typedef struct {
    uint8_t in_use;
    uint32_t pid;
    uint32_t source_endpoint;
    ipc_message_t pending[SYSCALL_IPC_PENDING_DEPTH];
    uint32_t pending_head;
    uint32_t pending_count;
} syscall_ipc_call_slot_t;

static syscall_ipc_call_slot_t g_syscall_ipc_call_slots[PROCESS_MAX_COUNT];

static inline uintptr_t syscall_alias_ptr(uintptr_t p) {
#if defined(WASMOS_HOST_TEST_SMP)
    /* Host harness: there is no higher-half alias of the kernel image, so the
     * translation below would turn a valid host address into a non-canonical
     * one. Same reasoning as cpu_local()'s host spelling in sched_thread.c --
     * the arch mechanism has no hosted equivalent, and the identity here is
     * what the kernel mapping provides at run time anyway. */
    return p;
#else
    if ((uint64_t)p < KERNEL_HIGHER_HALF_BASE) {
        p = (uintptr_t)((uint64_t)p + KERNEL_HIGHER_HALF_BASE);
    }
    return p;
#endif
}

static inline syscall_ipc_call_slot_t* syscall_ipc_call_slots_ptr(void) {
    return (syscall_ipc_call_slot_t*)(void*)syscall_alias_ptr(
        (uintptr_t)&g_syscall_ipc_call_slots[0]);
}

static int name_eq(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

static int syscall_arg_u32(uint64_t raw, uint32_t* out) {
    if (!out) {
        return -1;
    }
    if ((raw >> 32) != 0) {
        return -1;
    }
    *out = (uint32_t)raw;
    return 0;
}

static int syscall_arg_i32(uint64_t raw, int32_t* out) {
    if (!out) {
        return -1;
    }
    uint64_t sign_extended = (uint64_t)(int64_t)(int32_t)raw;
    if (sign_extended != raw) {
        return -1;
    }
    *out = (int32_t)raw;
    return 0;
}

/* Ring-3 self-test wiring.  Each installs one distinguished endpoint id that
 * IPC_CALL / IPC_NOTIFY special-case, so a guest can exercise a path that has no
 * other trigger.  IPC_ENDPOINT_NONE disarms; all three default to it, and the
 * special cases below are written so a disarmed value can never match a real
 * endpoint.  Set once during boot before the probe guest runs — they are plain
 * globals with no synchronisation. */

/* Endpoint whose IPC_CALLs are answered by the kernel itself instead of being
 * delivered.  Also the one kernel-owned destination a guest is allowed to call:
 * syscall_ipc_call_kernel_endpoint_allowed permits exactly this id.  Message
 * types 0x9ABC and 0x9ABD additionally inject stale, out-of-order, forged and
 * invalid-source replies to drive the reply-correlation and authenticity checks;
 * every other type is echoed straight back in RDX. */
void syscall_set_ipc_call_echo_endpoint(uint32_t endpoint) {
    g_ipc_call_echo_endpoint = endpoint;
}

uint32_t syscall_ipc_call_echo_endpoint(void) {
    return g_ipc_call_echo_endpoint;
}

/* Endpoints that IPC_CALL / IPC_NOTIFY refuse with IPC_ERR_PERM regardless of
 * ownership, so the control-plane deny paths can be exercised against an
 * otherwise valid endpoint. */
void syscall_set_ipc_call_control_deny_endpoint(uint32_t endpoint) {
    g_ipc_call_control_deny_endpoint = endpoint;
}

void syscall_set_ipc_notify_control_deny_endpoint(uint32_t endpoint) {
    g_ipc_notify_control_deny_endpoint = endpoint;
}

static syscall_ipc_call_slot_t* syscall_ipc_call_slot_for_pid(uint32_t pid) {
    syscall_ipc_call_slot_t* slots = syscall_ipc_call_slots_ptr();
    syscall_ipc_call_slot_t* empty = 0;
    if (pid == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        syscall_ipc_call_slot_t* slot = &slots[i];
        if (slot->in_use && !process_get(slot->pid)) {
            slot->in_use = 0;
            slot->pid = 0;
            slot->source_endpoint = IPC_ENDPOINT_NONE;
            slot->pending_head = 0;
            slot->pending_count = 0;
        }
        if (slot->in_use && slot->pid == pid) {
            return slot;
        }
        if (!empty && !slot->in_use) {
            empty = slot;
        }
    }
    if (!empty) {
        return 0;
    }
    empty->in_use = 1;
    empty->pid = pid;
    empty->source_endpoint = IPC_ENDPOINT_NONE;
    empty->pending_head = 0;
    empty->pending_count = 0;
    return empty;
}

/*
 * True while `request_id` is one this kernel has already handed out.
 *
 * Request ids come from a single monotonic counter, so a reply carrying an id
 * at or beyond the next one to be issued cannot be answering any call that
 * exists -- it was minted for a call that has not happened yet.  Compared as a
 * signed difference so the answer stays right across the counter's wrap.
 */
static int syscall_ipc_request_id_issued(uint32_t request_id) {
    return (int32_t)(request_id - g_syscall_ipc_call_next_request_id) < 0;
}

static int syscall_ipc_pending_enqueue(syscall_ipc_call_slot_t* slot, const ipc_message_t* msg) {
    if (!slot || !msg) {
        return -1;
    }
    /*
     * Refuse a reply for an id that has not been issued.  Retaining one lets a
     * peer answer a call the caller has not made: the reply sits in the ring
     * until the id comes round, and then resolves that future call with this
     * payload, without the peer having answered it at all.  The authenticity
     * check downstream asks WHO replied, never WHEN, so the two are otherwise
     * indistinguishable.  Only the legitimate destination can do it, which makes
     * it a confused-deputy hazard rather than an escalation -- but a reply to a
     * call that does not exist is never legitimate.
     */
    if (!syscall_ipc_request_id_issued(msg->request_id)) {
        return -1;
    }
    /* Explicit drop policy: bounded queue, drop oldest on overflow. */
    if (slot->pending_count >= SYSCALL_IPC_PENDING_DEPTH) {
        slot->pending_head = (slot->pending_head + 1u) % SYSCALL_IPC_PENDING_DEPTH;
        slot->pending_count--;
    }
    uint32_t tail = (slot->pending_head + slot->pending_count) % SYSCALL_IPC_PENDING_DEPTH;
    slot->pending[tail] = *msg;
    slot->pending_count++;
    return 0;
}

static int syscall_ipc_pending_take_request(syscall_ipc_call_slot_t* slot, uint32_t request_id,
                                            ipc_message_t* out) {
    if (!slot || !out || slot->pending_count == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < slot->pending_count; ++i) {
        uint32_t idx = (slot->pending_head + i) % SYSCALL_IPC_PENDING_DEPTH;
        if (slot->pending[idx].request_id != request_id) {
            continue;
        }
        *out = slot->pending[idx];
        for (uint32_t j = i; j + 1u < slot->pending_count; ++j) {
            uint32_t from = (slot->pending_head + j + 1u) % SYSCALL_IPC_PENDING_DEPTH;
            uint32_t to = (slot->pending_head + j) % SYSCALL_IPC_PENDING_DEPTH;
            slot->pending[to] = slot->pending[from];
        }
        slot->pending_count--;
        return 0;
    }
    return -1;
}

#ifdef WASMOS_SYSCALL_TEST_SEAMS
/*
 * The pending ring, exposed for tests.
 *
 * Its retention is otherwise unobservable from a single-threaded caller: an id
 * already issued is never drawn again, and one not yet issued is refused by
 * syscall_ipc_request_id_issued.  The ring earns its keep when several threads
 * share a reply endpoint -- one blocked on id 5 parks another's reply for id 6
 * -- which no host fixture drives.  These let a test exercise the ring itself
 * rather than infer it.
 */
/* Parks a reply in `pid`'s pending ring.  0 on success; -1 if no slot can be
 * allocated for the pid, or if the message's request_id has not been issued yet
 * (the retention refusal the ring enforces for real replies too).  Allocates the
 * slot on first use, so a pid with no IPC_CALL history is still usable. */
int syscall_test_pending_enqueue(uint32_t pid, const ipc_message_t* msg) {
    syscall_ipc_call_slot_t* slot = syscall_ipc_call_slot_for_pid(pid);
    return slot ? syscall_ipc_pending_enqueue(slot, msg) : -1;
}

/* Removes the parked reply matching `request_id`, preserving the order of the
 * rest.  0 with *out written, -1 if no slot exists or nothing matches. */
int syscall_test_pending_take(uint32_t pid, uint32_t request_id, ipc_message_t* out) {
    syscall_ipc_call_slot_t* slot = syscall_ipc_call_slot_for_pid(pid);
    return slot ? syscall_ipc_pending_take_request(slot, request_id, out) : -1;
}

/* The id the NEXT IPC_CALL will consume — i.e. one past the highest issued.  It
 * is the boundary syscall_ipc_request_id_issued compares against, so a test
 * builds an "already issued" id as this minus one and a "never issued" id as
 * this or beyond. */
uint32_t syscall_test_next_request_id(void) {
    return g_syscall_ipc_call_next_request_id;
}
#endif

static int syscall_ipc_reply_authentic(const ipc_message_t* resp, uint32_t expected_source_endpoint,
                                       uint32_t expected_owner_context) {
    uint32_t reply_owner_context = 0;
    if (!resp || resp->source == IPC_ENDPOINT_NONE) {
        return 0;
    }
    if (expected_source_endpoint != IPC_ENDPOINT_NONE && resp->source != expected_source_endpoint) {
        return 0;
    }
    if (ipc_endpoint_owner(resp->source, &reply_owner_context) != IPC_OK) {
        return 0;
    }
    return reply_owner_context == expected_owner_context;
}

static int syscall_ipc_call_source_endpoint(process_t* proc, uint32_t* out_endpoint) {
    syscall_ipc_call_slot_t* slot = 0;
    uint32_t endpoint = IPC_ENDPOINT_NONE;
    uint32_t owner_context = 0;

    if (!proc || !out_endpoint) {
        return IPC_ERR_INVALID;
    }
    slot = syscall_ipc_call_slot_for_pid(proc->pid);
    if (!slot) {
        return IPC_ERR_FULL;
    }
    if (slot->source_endpoint != IPC_ENDPOINT_NONE &&
        (ipc_endpoint_owner(slot->source_endpoint, &owner_context) != IPC_OK ||
         owner_context != proc->context_id)) {
        slot->source_endpoint = IPC_ENDPOINT_NONE;
    }
    if (slot->source_endpoint == IPC_ENDPOINT_NONE) {
        if (ipc_endpoint_create(proc->context_id, &endpoint) != IPC_OK) {
            return IPC_ERR_FULL;
        }
        slot->source_endpoint = endpoint;
    }
    *out_endpoint = slot->source_endpoint;
    return IPC_OK;
}

static int syscall_ipc_call_kernel_endpoint_allowed(uint32_t destination) {
    return destination == g_ipc_call_echo_endpoint;
}

static void syscall_trace_ring3_once(syscall_frame_t* frame) {
    if (!frame || g_ring3_syscall_logged) {
        return;
    }
    if ((frame->cs & 0x3u) != 0x3u) {
        return;
    }
    if ((uint32_t)frame->rax != WASMOS_SYSCALL_GETPID) {
        return;
    }
    process_t* proc = process_get(process_current_pid());
    if (!proc || !name_eq(proc->name, "ring3-smoke")) {
        return;
    }
    g_ring3_syscall_logged = 1;
    klog_write("[test] ring3 syscall ok\n");
}

static void syscall_trace_ring3_stress(syscall_frame_t* frame) {
    if (!frame || g_ring3_stress_ok_logged) {
        return;
    }
    if ((frame->cs & 0x3u) != 0x3u) {
        return;
    }
    process_t* proc = process_get(process_current_pid());
    if (!proc || !name_eq(proc->name, "ring3-smoke")) {
        return;
    }
    if ((uint32_t)frame->rax == WASMOS_SYSCALL_GETPID) {
        if (g_ring3_getpid_count < 0xFFFFFFFFu) {
            g_ring3_getpid_count++;
        }
        return;
    }
    if ((uint32_t)frame->rax == WASMOS_SYSCALL_EXIT && g_ring3_getpid_count >= 4096u) {
        g_ring3_stress_ok_logged = 1;
        klog_write("[test] ring3 preempt stress ok\n");
    }
}

/* The int 0x80 dispatch body, called from isr_syscall_128 with `frame` pointing
 * at the live saved-register block on the interrupt stack.
 *
 * The returned value is stored over the frame's saved RAX before POP_REGS, so it
 * IS the guest's RAX on return.  Fields written into `frame` are likewise
 * restored into the guest — which is how the secondary return works: THREAD_JOIN
 * delivers the joined status and IPC_CALL the reply's arg0 in frame->rdx, both
 * cleared up front so a failure never leaves a stale value there.
 *
 * `frame` is borrowed and must not be retained.  A NULL frame and an unknown
 * call number both answer (uint64_t)-1; per-call errors are the packed
 * abi/errors.yaml or IPC_ERR_* codes, sign-extended into RAX, so a guest reads
 * them back as negative int32.
 *
 * Blocking calls (WAIT, THREAD_JOIN, and IPC_CALL awaiting its reply) park the
 * caller inside this handler and resume here, so the handler can span many
 * timeslices.  Their retry loops are what make a spurious wake harmless.  The
 * ring-3 register snapshot at the top exists for exactly that: a blocking call
 * must resume at the post-syscall RIP, not re-enter the trap.
 *
 * Under WARP three ranges are intercepted before the switch — the return
 * trampoline, the memory helper, and RAX in [WARP_HC_SYSCALL_BASE,
 * +WARP_HC_MAX), which is forwarded to warp_ring3_dispatch for its own
 * validation. */
uint64_t x86_syscall_handler(syscall_frame_t* frame) {
    thread_t* current_thread = 0;
    if (!frame) {
        return (uint64_t)-1;
    }
    current_thread = thread_get(thread_current_tid());
    /* Keep thread-resume context aligned with the latest syscall trap frame so
     * any blocking/yielding syscall resumes at the correct post-syscall RIP. */
    if (current_thread && (frame->cs & 0x3u) == 0x3u) {
        current_thread->ctx.rax = frame->rax;
        current_thread->ctx.rbx = frame->rbx;
        current_thread->ctx.rcx = frame->rcx;
        current_thread->ctx.rdx = frame->rdx;
        current_thread->ctx.rbp = frame->rbp;
        current_thread->ctx.rsi = frame->rsi;
        current_thread->ctx.rdi = frame->rdi;
        current_thread->ctx.r8 = frame->r8;
        current_thread->ctx.r9 = frame->r9;
        current_thread->ctx.r10 = frame->r10;
        current_thread->ctx.r11 = frame->r11;
        current_thread->ctx.r12 = frame->r12;
        current_thread->ctx.r13 = frame->r13;
        current_thread->ctx.r14 = frame->r14;
        current_thread->ctx.r15 = frame->r15;
        current_thread->ctx.cs = frame->cs;
        current_thread->ctx.rip = frame->rip;
        current_thread->ctx.rflags = frame->rflags;
        current_thread->ctx.root_table = paging_get_current_root_table();
    }
    syscall_trace_ring3_stress(frame);
    syscall_trace_ring3_once(frame);

#ifdef WASMOS_WASM_RUNTIME_WARP
    /* WARP ring-3 return: the return trampoline fires this after the JIT
     * wrapper executes its final `ret`. RDI contains the raw RAX that the
     * JIT wrapper had on exit (trap code or return value). */
    if ((uint32_t)frame->rax == WASMOS_SYSCALL_WARP_RETURN) {
        thread_t* r3t = cpu_local()->current_thread;
        if (r3t && r3t->warp_r3_active) {
            r3t->warp_r3_active = 0;
            __builtin_longjmp(r3t->warp_r3_jbuf, 1);
        }
        frame->rax = (uint64_t)-1;
        return 0;
    }
    if ((uint32_t)frame->rax == WASMOS_SYSCALL_WARP_MEMORY_HELPER) {
        uint32_t basedata_len = 0;
        if (syscall_arg_u32(frame->rsi, &basedata_len) != 0) {
            frame->rax = 0;
            return 0;
        }
        frame->rax = warp_r3_memory_helper(frame->rdi, basedata_len, frame->rdx);
        return frame->rax;
    }
    /* WARP ring-3 hostcall: stubs fire int 0x80 with RAX = 0x100 + hc_id. */
    if (frame->rax >= WARP_HC_SYSCALL_BASE && frame->rax < WARP_HC_SYSCALL_BASE + WARP_HC_MAX) {
        uint32_t hc_id = (uint32_t)(frame->rax - WARP_HC_SYSCALL_BASE);
        frame->rax = (uint64_t)warp_ring3_dispatch(hc_id, (void*)frame);
        return frame->rax;
    }
#endif

    switch ((uint32_t)frame->rax) {
    case WASMOS_SYSCALL_NOP:
        return 0;
    case WASMOS_SYSCALL_GETPID:
        if (!g_ring3_native_abi_logged && (frame->cs & 0x3u) == 0x3u) {
            process_t* proc = process_get(process_current_pid());
            if (proc && name_eq(proc->name, "ring3-native")) {
                g_ring3_native_abi_logged = 1;
                klog_write("[test] ring3 native abi ok\n");
            }
        }
        return process_current_pid();
    case WASMOS_SYSCALL_GETTID:
        if (!g_ring3_native_gettid_logged && (frame->cs & 0x3u) == 0x3u) {
            process_t* proc = process_get(process_current_pid());
            if (proc && name_eq(proc->name, "ring3-native")) {
                g_ring3_native_gettid_logged = 1;
                klog_write("[test] ring3 native gettid ok\n");
            }
        }
        return thread_current_tid();
    case WASMOS_SYSCALL_EXIT: {
        process_t* proc = process_get(process_current_pid());
        int32_t exit_status = 0;
        if (!proc) {
            return (uint64_t)-1;
        }
        if (syscall_arg_i32(frame->rdi, &exit_status) != 0) {
            return (uint64_t)-1;
        }
        process_set_exit_status(proc, exit_status);
        process_yield(PROCESS_RUN_EXITED);
        return 0;
    }
    case WASMOS_SYSCALL_YIELD:
        if (!g_ring3_yield_logged) {
            process_t* proc = process_get(process_current_pid());
            if (proc && name_eq(proc->name, "ring3-smoke") && (frame->cs & 0x3u) == 0x3u) {
                g_ring3_yield_logged = 1;
                klog_write("[test] ring3 yield syscall ok\n");
            }
        }
        process_yield(PROCESS_RUN_YIELDED);
        return 0;
    case WASMOS_SYSCALL_THREAD_YIELD:
        if (!g_ring3_thread_yield_logged) {
            process_t* proc = process_get(process_current_pid());
            if (proc && name_eq(proc->name, "ring3-native") && (frame->cs & 0x3u) == 0x3u) {
                g_ring3_thread_yield_logged = 1;
                klog_write("[test] ring3 thread yield syscall ok\n");
            }
        }
        process_yield(PROCESS_RUN_YIELDED);
        return 0;
    case WASMOS_SYSCALL_THREAD_EXIT: {
        process_t* proc = process_get(process_current_pid());
        int32_t exit_status = 0;
        if (!proc) {
            return (uint64_t)-1;
        }
        if (syscall_arg_i32(frame->rdi, &exit_status) != 0) {
            return (uint64_t)-1;
        }
        if (!g_ring3_thread_exit_logged && (frame->cs & 0x3u) == 0x3u &&
            name_eq(proc->name, "ring3-native")) {
            g_ring3_thread_exit_logged = 1;
            klog_write("[test] ring3 thread exit syscall ok\n");
        }
        process_set_exit_status(proc, exit_status);
        process_yield(PROCESS_RUN_THREAD_EXITED);
        return 0;
    }
    case WASMOS_SYSCALL_THREAD_CREATE: {
        process_t* proc = process_get(process_current_pid());
        uint32_t tid = 0;
        uint64_t entry_rip = frame->rdi;
        uint64_t user_stack_top = frame->rsi;
        if (!proc) {
            return (uint64_t)-1;
        }
        if (!g_ring3_thread_create_logged && (frame->cs & 0x3u) == 0x3u &&
            name_eq(proc->name, "ring3-native")) {
            g_ring3_thread_create_logged = 1;
            klog_write("[test] ring3 thread create syscall ok\n");
        }
        if (process_thread_spawn_user_internal(proc->pid, "user-thread", entry_rip, user_stack_top,
                                               &tid) != 0) {
            return (uint64_t)-1;
        }
        return tid;
    }
    case WASMOS_SYSCALL_THREAD_JOIN: {
        process_t* proc = process_get(process_current_pid());
        uint32_t target_tid = 0;
        int join_rc = -1;
        int32_t exit_status = 0;
        uint32_t self_tid = thread_current_tid();
        if (!proc) {
            return (uint64_t)(int64_t)WASMOS_ERR_KERNEL_NO_CALLER;
        }
        if (!g_ring3_thread_join_logged && (frame->cs & 0x3u) == 0x3u &&
            name_eq(proc->name, "ring3-native")) {
            g_ring3_thread_join_logged = 1;
            klog_write("[test] ring3 thread join syscall ok\n");
        }
        if (syscall_arg_u32(frame->rdi, &target_tid) != 0) {
            return (uint64_t)(int64_t)WASMOS_INVAL;
        }
        /* RDX carries the joined thread's exit status, RAX the outcome.  The
         * status is guest-chosen and spans the whole 32-bit range, so it cannot
         * share RAX with the outcome: a thread exiting -1 would be
         * indistinguishable from a failed join.  Cleared up front so an error
         * return never leaves a stale status in RDX. */
        frame->rdx = 0;
        for (;;) {
            join_rc = process_thread_join(proc, target_tid, &exit_status);
            if (join_rc == 0) {
                if (!g_ring3_thread_join_helper_ok_logged && (frame->cs & 0x3u) == 0x3u &&
                    !name_eq(proc->name, "ring3-native") && target_tid != self_tid) {
                    g_ring3_thread_join_helper_ok_logged = 1;
                    klog_write("[test] ring3 thread join helper ok\n");
                }
                frame->rdx = (uint64_t)(uint32_t)exit_status;
                return 0;
            }
            if (join_rc < 0) {
                if (!g_ring3_thread_join_self_deny_logged && (frame->cs & 0x3u) == 0x3u &&
                    name_eq(proc->name, "ring3-native") && target_tid == self_tid) {
                    g_ring3_thread_join_self_deny_logged = 1;
                    klog_write("[test] ring3 thread join self deny ok\n");
                }
                if (!g_ring3_thread_detach_join_deny_logged && (frame->cs & 0x3u) == 0x3u &&
                    name_eq(proc->name, "ring3-threading") && target_tid != self_tid) {
                    g_ring3_thread_detach_join_deny_logged = 1;
                    klog_write("[test] ring3 thread detach join deny ok\n");
                }
                return (uint64_t)(int64_t)join_rc; /* already a packed code */
            }
            process_yield(PROCESS_RUN_BLOCKED);
        }
    }
    case WASMOS_SYSCALL_THREAD_DETACH: {
        process_t* proc = process_get(process_current_pid());
        uint32_t target_tid = 0;
        int detach_rc = -1;
        if (!proc) {
            return (uint64_t)-1;
        }
        if (!g_ring3_thread_detach_logged && (frame->cs & 0x3u) == 0x3u &&
            name_eq(proc->name, "ring3-native")) {
            g_ring3_thread_detach_logged = 1;
            klog_write("[test] ring3 thread detach syscall ok\n");
        }
        if (syscall_arg_u32(frame->rdi, &target_tid) != 0) {
            return (uint64_t)-1;
        }
        detach_rc = process_thread_detach(proc, target_tid);
        if (detach_rc < 0) {
            if (!g_ring3_thread_detach_invalid_deny_logged && (frame->cs & 0x3u) == 0x3u &&
                name_eq(proc->name, "ring3-native") && target_tid == 0) {
                g_ring3_thread_detach_invalid_deny_logged = 1;
                klog_write("[test] ring3 thread detach invalid deny ok\n");
            }
            return (uint64_t)-1;
        }
        if (!g_ring3_thread_detach_helper_ok_logged && (frame->cs & 0x3u) == 0x3u &&
            !name_eq(proc->name, "ring3-native") && target_tid != thread_current_tid()) {
            g_ring3_thread_detach_helper_ok_logged = 1;
            klog_write("[test] ring3 thread detach helper ok\n");
        }
        if (!g_ring3_thread_detach_join_deny_logged && (frame->cs & 0x3u) == 0x3u &&
            name_eq(proc->name, "ring3-native")) {
            if (process_thread_join(proc, target_tid, 0) < 0) {
                g_ring3_thread_detach_join_deny_logged = 1;
                klog_write("[test] ring3 thread detach join deny ok\n");
            }
        }
        return 0;
    }
    case WASMOS_SYSCALL_MUTEX_TRY_LOCK: {
        process_t* proc = process_get(process_current_pid());
        if (!proc) {
            return (uint64_t)-1;
        }
        return (uint64_t)(int64_t)user_mutex_user_try_lock(proc->context_id, frame->rdi,
                                                           thread_current_tid(), 0);
    }
    case WASMOS_SYSCALL_MUTEX_UNLOCK: {
        process_t* proc = process_get(process_current_pid());
        if (!proc) {
            return (uint64_t)-1;
        }
        return (uint64_t)(int64_t)user_mutex_user_unlock(proc->context_id, frame->rdi,
                                                         thread_current_tid(), 0);
    }
    case WASMOS_SYSCALL_WAIT: {
        process_t* proc = process_get(process_current_pid());
        uint32_t target_pid = 0;
        int32_t exit_status = 0;
        int wait_rc = 0;
        if (!proc) {
            return (uint64_t)-1;
        }
        if (syscall_arg_u32(frame->rdi, &target_pid) != 0) {
            return (uint64_t)-1;
        }
        for (;;) {
            wait_rc = process_wait(proc, target_pid, &exit_status);
            if (wait_rc == 0) {
                return (uint64_t)(int64_t)exit_status;
            }
            if (wait_rc < 0) {
                return (uint64_t)-1;
            }
            process_yield(PROCESS_RUN_BLOCKED);
        }
    }
    case WASMOS_SYSCALL_NOTIFY_READY: {
        process_t* proc = process_get(process_current_pid());
        if (proc) {
            process_notify_ready(proc);
        }
        return 0;
    }
    case WASMOS_SYSCALL_IPC_NOTIFY: {
        process_t* proc = process_get(process_current_pid());
        uint32_t endpoint = 0;
        int rc = IPC_ERR_INVALID;
        if (!proc) {
            return (uint64_t)-1;
        }
        if (syscall_arg_u32(frame->rdi, &endpoint) != 0) {
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_arg_width_deny_logged) {
                g_ring3_ipc_arg_width_deny_logged = 1;
                klog_write("[test] ring3 ipc syscall arg width deny ok\n");
            }
            return (uint64_t)(int64_t)IPC_ERR_INVALID;
        }
        rc = ipc_notify_from(proc->context_id, endpoint);
        if (name_eq(proc->name, "ring3-smoke")) {
            if (!g_ring3_ipc_deny_logged && endpoint == 0xFFFFFFFFu && rc == IPC_ERR_NOENT) {
                g_ring3_ipc_deny_logged = 1;
                klog_write("[test] ring3 ipc syscall deny ok\n");
            }
            if (!g_ring3_ipc_ok_logged && rc == IPC_OK) {
                g_ring3_ipc_ok_logged = 1;
                klog_write("[test] ring3 ipc syscall ok\n");
            }
            if (!g_ring3_ipc_control_deny_logged &&
                endpoint == g_ipc_notify_control_deny_endpoint &&
                g_ipc_notify_control_deny_endpoint != IPC_ENDPOINT_NONE && rc == IPC_ERR_PERM) {
                g_ring3_ipc_control_deny_logged = 1;
                klog_write("[test] ring3 ipc syscall control deny ok\n");
            }
        }
        return (uint64_t)(int64_t)rc;
    }
    case WASMOS_SYSCALL_IPC_CALL: {
        process_t* proc = process_get(process_current_pid());
        uint32_t destination = 0;
        uint32_t msg_type = 0;
        uint32_t arg0 = 0;
        uint32_t arg1 = 0;
        uint32_t arg2 = 0;
        uint32_t arg3 = 0;
        uint32_t source_endpoint = IPC_ENDPOINT_NONE;
        uint32_t request_id = g_syscall_ipc_call_next_request_id++;
        uint32_t owner_context = 0;
        uint32_t expected_reply_source = IPC_ENDPOINT_NONE;
        uint32_t expected_reply_owner_context = 0;
        uint32_t injected_out_of_order_request_id = 0;
        uint32_t dropped_inauth_replies = 0;
        uint8_t injected_invalid_source_spoof = 0;
        int rc = IPC_ERR_INVALID;
        ipc_message_t req;
        ipc_message_t resp;
        syscall_ipc_call_slot_t* slot = 0;
        if (!proc) {
            return (uint64_t)-1;
        }
        /* RDX is both an input (arg0) and the secondary return, so latch the
         * input before clearing it.
         *
         * IPC_CALL returns a secondary value in RDX only on success, and it is
         * cleared up-front so callers never observe stale register contents on
         * an error path.  That has to happen BEFORE the argument-width check
         * below, not after: that check is the one rejection where RDX still
         * holds caller-supplied input, which a caller reading RDX after a
         * failed call would take for a reply payload. */
        uint64_t raw_arg0 = frame->rdx;
        frame->rdx = 0;
        if (syscall_arg_u32(frame->rdi, &destination) != 0 ||
            syscall_arg_u32(frame->rsi, &msg_type) != 0 || syscall_arg_u32(raw_arg0, &arg0) != 0 ||
            syscall_arg_u32(frame->rcx, &arg1) != 0 || syscall_arg_u32(frame->r8, &arg2) != 0 ||
            syscall_arg_u32(frame->r9, &arg3) != 0) {
            return (uint64_t)(int64_t)IPC_ERR_INVALID;
        }
        if (request_id == 0) {
            request_id = g_syscall_ipc_call_next_request_id++;
        }
        slot = syscall_ipc_call_slot_for_pid(proc->pid);
        if (!slot) {
            return (uint64_t)(int64_t)IPC_ERR_FULL;
        }
        rc = syscall_ipc_call_source_endpoint(proc, &source_endpoint);
        if (rc != IPC_OK) {
            return (uint64_t)(int64_t)rc;
        }
        if (ipc_endpoint_owner(destination, &owner_context) != IPC_OK) {
            rc = IPC_ERR_NOENT; /* no such destination endpoint */
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_deny_logged &&
                destination == 0xFFFFFFFFu) {
                g_ring3_ipc_call_deny_logged = 1;
                klog_write("[test] ring3 ipc call deny ok\n");
                if (!g_ring3_ipc_call_err_rdx_zero_logged && frame->rdx == 0) {
                    g_ring3_ipc_call_err_rdx_zero_logged = 1;
                    klog_write("[test] ring3 ipc call err rdx zero ok\n");
                }
            }
            return (uint64_t)(int64_t)rc;
        }
        if (owner_context == IPC_CONTEXT_KERNEL &&
            !syscall_ipc_call_kernel_endpoint_allowed(destination)) {
            rc = IPC_ERR_PERM;
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_perm_deny_logged) {
                g_ring3_ipc_call_perm_deny_logged = 1;
                klog_write("[test] ring3 ipc call perm deny ok\n");
            }
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_control_deny_logged) {
                g_ring3_ipc_call_control_deny_logged = 1;
                klog_write("[test] ring3 ipc call control deny ok\n");
            }
            return (uint64_t)(int64_t)rc;
        }
        if (destination == g_ipc_call_control_deny_endpoint &&
            g_ipc_call_control_deny_endpoint != IPC_ENDPOINT_NONE) {
            rc = IPC_ERR_PERM;
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_control_deny_logged) {
                g_ring3_ipc_call_control_deny_logged = 1;
                klog_write("[test] ring3 ipc call control deny ok\n");
            }
            if (name_eq(proc->name, "ring3-smoke") &&
                !g_ring3_ipc_call_control_endpoint_deny_logged) {
                g_ring3_ipc_call_control_endpoint_deny_logged = 1;
                klog_write("[test] ring3 ipc call control endpoint deny ok\n");
            }
            return (uint64_t)(int64_t)rc;
        }
        req.type = msg_type;
        req.source = source_endpoint;
        req.destination = destination;
        req.request_id = request_id;
        req.arg0 = arg0;
        req.arg1 = arg1;
        req.arg2 = arg2;
        req.arg3 = arg3;
        expected_reply_source = destination;
        expected_reply_owner_context = owner_context;
        if (destination == g_ipc_call_echo_endpoint &&
            g_ipc_call_echo_endpoint != IPC_ENDPOINT_NONE && msg_type == 0x00009ABCu) {
            ipc_message_t stale;
            ipc_message_t synthetic;
            stale.type = req.type;
            stale.source = req.source;
            stale.destination = req.source;
            stale.request_id = req.request_id + 1u; /* stale/future replay probe */
            stale.arg0 = req.arg0 ^ 0xFFFFFFFFu;
            stale.arg1 = req.arg1;
            stale.arg2 = req.arg2;
            stale.arg3 = req.arg3;
            synthetic.type = req.type;
            synthetic.source = req.source;
            synthetic.destination = req.source;
            synthetic.request_id = req.request_id;
            synthetic.arg0 = req.arg0;
            synthetic.arg1 = req.arg1;
            synthetic.arg2 = req.arg2;
            synthetic.arg3 = req.arg3;
            expected_reply_source = req.source;
            expected_reply_owner_context = proc->context_id;
            (void)syscall_ipc_pending_enqueue(slot, &stale);
            (void)syscall_ipc_pending_enqueue(slot, &synthetic);
        }
        if (destination == g_ipc_call_echo_endpoint &&
            g_ipc_call_echo_endpoint != IPC_ENDPOINT_NONE && msg_type == 0x00009ABDu) {
            ipc_message_t out_of_order;
            ipc_message_t invalid_source;
            ipc_message_t forged;
            ipc_message_t synthetic;
            out_of_order.type = req.type;
            out_of_order.source = destination;
            out_of_order.destination = req.source;
            out_of_order.request_id = req.request_id + 1u; /* unrelated reply */
            out_of_order.arg0 = req.arg0 ^ 0x01010101u;
            out_of_order.arg1 = req.arg1;
            out_of_order.arg2 = req.arg2;
            out_of_order.arg3 = req.arg3;
            invalid_source = out_of_order;
            invalid_source.request_id = req.request_id;
            invalid_source.source = IPC_ENDPOINT_NONE; /* invalid spoof */
            invalid_source.arg0 = req.arg0 ^ 0xA5A5A5A5u;
            forged.type = req.type;
            forged.source = req.source; /* wrong source: caller endpoint */
            forged.destination = req.source;
            forged.request_id = req.request_id;
            forged.arg0 = req.arg0;
            forged.arg1 = req.arg1;
            forged.arg2 = req.arg2;
            forged.arg3 = req.arg3;
            synthetic = forged;
            synthetic.source = destination; /* expected source */
            injected_out_of_order_request_id = out_of_order.request_id;
            (void)syscall_ipc_pending_enqueue(slot, &out_of_order);
            (void)syscall_ipc_pending_enqueue(slot, &invalid_source);
            injected_invalid_source_spoof = 1;
            (void)syscall_ipc_pending_enqueue(slot, &forged);
            (void)syscall_ipc_pending_enqueue(slot, &synthetic);
        }
        if (destination == g_ipc_call_echo_endpoint &&
            g_ipc_call_echo_endpoint != IPC_ENDPOINT_NONE && msg_type != 0x00009ABCu &&
            msg_type != 0x00009ABDu) {
            frame->rdx = (uint64_t)req.arg0;
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_ok_logged &&
                (uint32_t)frame->rdx == req.arg0) {
                g_ring3_ipc_call_ok_logged = 1;
                klog_write("[test] ring3 ipc call ok\n");
            }
            return 0;
        }
        if (!(destination == g_ipc_call_echo_endpoint &&
              g_ipc_call_echo_endpoint != IPC_ENDPOINT_NONE &&
              (msg_type == 0x00009ABCu || msg_type == 0x00009ABDu))) {
            rc = ipc_send_from(proc->context_id, destination, &req);
            if (rc != IPC_OK) {
                return (uint64_t)(int64_t)rc;
            }
        }
        while (syscall_ipc_pending_take_request(slot, request_id, &resp) == 0) {
            if (!syscall_ipc_reply_authentic(&resp, expected_reply_source,
                                             expected_reply_owner_context)) {
                dropped_inauth_replies++;
                continue;
            }
            frame->rdx = (uint64_t)resp.arg0;
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_ok_logged &&
                (uint32_t)frame->rdx == req.arg0) {
                g_ring3_ipc_call_ok_logged = 1;
                klog_write("[test] ring3 ipc call ok\n");
            }
            if (name_eq(proc->name, "ring3-smoke") && msg_type == 0x00009ABCu &&
                !g_ring3_ipc_call_correlation_logged && (uint32_t)frame->rdx == req.arg0) {
                g_ring3_ipc_call_correlation_logged = 1;
                klog_write("[test] ring3 ipc call correlate ok\n");
                klog_write("[test] ring3 ipc call stale id deny ok\n");
            }
            if (name_eq(proc->name, "ring3-smoke") && msg_type == 0x00009ABDu &&
                (uint32_t)frame->rdx == req.arg0) {
                klog_write("[test] ring3 ipc call source auth ok\n");
                if (!g_ring3_ipc_call_spoof_invalid_source_deny_logged &&
                    injected_invalid_source_spoof) {
                    g_ring3_ipc_call_spoof_invalid_source_deny_logged = 1;
                    klog_write("[test] ring3 ipc call spoof invalid source deny ok\n");
                }
                if (!g_ring3_ipc_call_out_of_order_retain_logged &&
                    injected_out_of_order_request_id != 0) {
                    ipc_message_t retained;
                    if (syscall_ipc_pending_take_request(slot, injected_out_of_order_request_id,
                                                         &retained) == 0) {
                        g_ring3_ipc_call_out_of_order_retain_logged = 1;
                        klog_write("[test] ring3 ipc call out-of-order retain ok\n");
                    }
                }
                if (!g_ring3_ipc_call_owner_sender_stress_logged && dropped_inauth_replies >= 2u) {
                    g_ring3_ipc_call_owner_sender_stress_logged = 1;
                    klog_write("[test] ring3 ipc owner+sender stress ok\n");
                }
            }
            return 0;
        }
        for (;;) {
            rc = ipc_recv_blocking_for(proc->context_id, source_endpoint, &resp);
            if (rc != IPC_OK) {
                return (uint64_t)(int64_t)rc;
            }
            if (resp.request_id != request_id) {
                (void)syscall_ipc_pending_enqueue(slot, &resp);
                continue;
            }
            if (!syscall_ipc_reply_authentic(&resp, expected_reply_source,
                                             expected_reply_owner_context)) {
                dropped_inauth_replies++;
                continue;
            }
            frame->rdx = (uint64_t)resp.arg0;
            if (name_eq(proc->name, "ring3-smoke") && !g_ring3_ipc_call_ok_logged &&
                (uint32_t)frame->rdx == req.arg0) {
                g_ring3_ipc_call_ok_logged = 1;
                klog_write("[test] ring3 ipc call ok\n");
            }
            return 0;
        }
    }
    default:
        return (uint64_t)-1;
    }
}
