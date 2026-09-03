/* test_syscall_ipc.c — host tests for the IPC syscalls (src/kernel/syscall.c).
 *
 * WASMOS_SYSCALL_IPC_CALL is the whole synchronous request/reply primitive for
 * ring-3: it allocates a per-process reply endpoint, sends, then correlates the
 * reply by request_id and checks the reply is authentic. Two of the documented
 * IPC invariants live here and nowhere else -- "reply authenticity checked at
 * IPC_CALL" and "all IPC_CALL fields are validated as 32-bit-clean".
 *
 * The only other coverage is the klog string probes syscall.c fires for the
 * ring3-smoke image, which assert that a line was printed rather than that a
 * reply was rejected. This suite drives x86_syscall_handler directly with a
 * syscall_frame_t, against the REAL ipc.c, and asserts on the returned frame.
 *
 * The service side is genuine: a yield hook plays the role of the peer, reading
 * the request off the destination endpoint and replying to req.source exactly
 * as a real service would (including, in the negative cases, replying badly).
 * That hook runs at the point the caller is parked inside
 * ipc_recv_blocking_for, which is where a service on another CPU would run.
 *
 * MODELLING NOTE (blocking). In the kernel, process_yield(PROCESS_RUN_BLOCKED)
 * does not return until a waker resumes the thread, so the reply correlation
 * that follows it in the IPC_CALL path runs post-wake. A host stub cannot
 * suspend, so process_yield returns immediately and an unanswered call observes
 * a "spurious wake" -- which IPC_CALL reports to ring 3 as IPC_EMPTY, the
 * documented retry contract. The yield hook is the stand-in waker: it fires with
 * the caller parked in the wait list and no IPC lock held, so the reply it sends
 * travels the real send-and-wake path rather than a fabricated one.
 */

#include <stdio.h>

#include "test_shuffle.h"
#include <stdlib.h>
#include <string.h>

#include "ipc.h"
#include "process.h"
#include "sched.h"
#include "sched_event.h"
#include "syscall.h"
#include "thread.h"

static int g_failures;
static int g_checks;

/* Record one assertion. A failed condition is counted and printed with its
 * source line, and the case CONTINUES -- so one case can report several failures
 * and every assertion after a failure still runs. The counters are cumulative
 * across cases; main's exit status is the only pass/fail signal. */
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ stubs */

#define POOL_MAX 4
static thread_t g_threads[POOL_MAX];
static uint32_t g_current_tid = 1;
static uint64_t g_now;
static int g_yield_calls;
static void (*g_yield_hook)(void);

/* Two processes: the caller and the peer service. */
#define CALLER_PID 1u
#define CALLER_CTX 41u
#define SERVICE_CTX 42u
static process_t g_procs[2];
static uint32_t g_current_pid = CALLER_PID;

/* The tick clock is a fixture variable: it advances only where a case assigns
 * g_now, never on its own. The kernel counts timer interrupts instead, and
 * scales milliseconds by the configured tick rate (rounding up); here one tick
 * is one millisecond. No case in this file arms a timeout. */
uint64_t timer_ticks(void) {
    return g_now;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}

/* The fake thread table: POOL_MAX slots with tid == index + 1, so a lookup is a
 * direct index. The kernel's versions take the thread-table lock and skip
 * THREAD_STATE_UNUSED slots; these are unlocked and unfiltered, which holds only
 * because the file is single-threaded and never frees a slot. Bounding
 * thread_table_at at POOL_MAX is what keeps sched_event.c's scan -- which runs
 * to THREAD_MAX_COUNT -- inside the fixture. */
thread_t* thread_table_at(uint32_t i) {
    return (i < POOL_MAX) ? &g_threads[i] : 0;
}
thread_t* thread_get(uint32_t tid) {
    if (tid == 0 || tid > POOL_MAX) {
        return 0;
    }
    return &g_threads[tid - 1u];
}
/* The kernel reads the executing CPU's current thread; the fixture's "current
 * CPU" is g_current_tid, which reset() points at tid 1 and no case moves. */
uint32_t thread_current_tid(void) {
    return g_current_tid;
}
/* Unconditional write. The kernel validates the edge against the thread state
 * machine under the table lock and drops an illegal one -- notably any attempt
 * to leave ZOMBIE -- so a transition the real kernel would refuse takes effect
 * here. */
void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    thread_t* t = thread_get(tid);
    if (t) {
        t->state = state;
        t->block_reason = reason;
    }
}
/* Mark the thread READY and nothing more. The kernel's version claims the wake
 * against the thread's completion path, drops it unless the thread is genuinely
 * BLOCKED, enqueues it on a run queue and may request a preemption; a stale wake
 * the real scheduler would discard therefore still lands here. Nothing in this
 * file asserts on wakes, only on what the syscall returned. */
void sched_wake_thread(thread_t* t) {
    if (t) {
        t->state = THREAD_STATE_READY;
    }
}

/* Returns immediately, because a host stub cannot suspend its caller; see the
 * MODELLING NOTE at the top of the file. An installed g_yield_hook -- the peer
 * service -- fires at most once per install, before the caller is unlinked from
 * the wait list, so call_with re-arms it for every call. */
void process_yield(process_run_result_t result) {
    (void)result;
    g_yield_calls++;
    thread_t* self = thread_get(g_current_tid);
    if (g_yield_hook) {
        void (*hook)(void) = g_yield_hook;
        g_yield_hook = 0; /* fires once */
        hook();
    }
    if (self) {
        sched_event_t* ev = self->wait_event;
        if (ev) {
            ksync_spinlock_lock(&ev->lock);
            if (self->wait_event == ev) {
                list_head_del(&self->event_node);
                self->wait_event = 0;
            }
            ksync_spinlock_unlock(&ev->lock);
        }
        self->state = THREAD_STATE_RUNNING;
    }
}

/* The process table is one entry deep: only CALLER_PID resolves, so the peer
 * service has a context id and endpoints but no process behind it. The kernel
 * searches its real process list, and its process_current_pid reads the
 * executing CPU's slot rather than a global. */
uint32_t process_current_pid(void) {
    return g_current_pid;
}
process_t* process_get(uint32_t pid) {
    if (pid == CALLER_PID) {
        return &g_procs[0];
    }
    return 0;
}

/* Present to satisfy the linker: syscall.c references these, but no path this
 * file drives reaches them. The klog calls in particular are guarded on the
 * caller being the "ring3-smoke" process, which the fixture is not. */
void klog_write(const char* s) {
    (void)s;
}
uint64_t paging_get_current_root_table(void) {
    return 0;
}
void process_notify_ready(process_t* p) {
    (void)p;
}
void process_set_exit_status(process_t* p, int32_t status) {
    (void)p;
    (void)status;
}
/* The uniform -1 below is not a contract any of these entry points has: the real
 * process_thread_* and process_wait return 0 for done, a packed WASMOS_ERR_*
 * code (negative) for failure, and in the join/wait cases a positive value
 * meaning the caller was parked and must retry. Nothing this file drives calls
 * them, so the value is never observed; a new case that reached one would see a
 * failure the kernel cannot produce. user_mutex_user_try_lock and
 * user_mutex_user_unlock take four arguments in the kernel (context id, user
 * address, tid, out-state) -- these two-argument stubs share only the symbol
 * name, and user_mutex.h is deliberately not included here. */
int process_thread_detach(process_t* p, uint32_t tid) {
    (void)p;
    (void)tid;
    return -1;
}
int process_thread_join(process_t* p, uint32_t tid, int32_t* out) {
    (void)p;
    (void)tid;
    (void)out;
    return -1;
}
int process_thread_spawn_user_internal(uint32_t pid, const char* name, uint64_t rip, uint64_t rsp,
                                       uint32_t* out_tid) {
    (void)pid;
    (void)name;
    (void)rip;
    (void)rsp;
    (void)out_tid;
    return -1;
}
int process_wait(process_t* p, uint32_t target, int32_t* out) {
    (void)p;
    (void)target;
    (void)out;
    return -1;
}
int user_mutex_user_try_lock(uint64_t p, uint32_t tid) {
    (void)p;
    (void)tid;
    return -1;
}
int user_mutex_user_unlock(uint64_t p, uint32_t tid) {
    (void)p;
    (void)tid;
    return -1;
}

/* --------------------------------------------------------------- fixtures */

static uint32_t g_dest_ep;    /* the peer service's endpoint */
static uint32_t g_reply_ep;   /* the caller's per-process reply endpoint */
static uint32_t g_seen_src;   /* req.source as the service saw it */
static uint32_t g_seen_reqid; /* req.request_id as the service saw it */
static uint32_t g_seen_type;
static int g_service_saw_request;

/* Per-case fixture reset: drain both endpoints, rebuild the thread pool and the
 * caller process, and clear the clock, the yield hook and the request fields the
 * peer records. g_reply_ep survives, since it names the endpoint to drain on the
 * next entry. It does NOT recreate the IPC state -- ipc_init and the peer's
 * g_dest_ep are set up once in main, and endpoints already handed out stay
 * live -- nor does it clear g_reply_mode or g_foreign_ep, which call_with and
 * the cases that need them set immediately before use. */
static void reset(void) {
    /* Drain anything a previous case left in the caller's reply endpoint. The
     * endpoint is per-PROCESS and reused across cases, so an unmatched reply
     * would be picked up by the next call's blocking receive and make that case
     * fail for a reason that has nothing to do with what it tests. */
    ipc_message_t drop;
    if (g_reply_ep != 0) {
        while (ipc_recv_for(CALLER_CTX, g_reply_ep, &drop) == IPC_OK) {
        }
    }
    /* And anything left on the peer's side. A call resolved from the pending
     * ring returns AFTER its request was already sent, so it leaves an
     * unanswered request behind -- which the next case's peer would otherwise
     * pick up instead of its own. */
    if (g_dest_ep != 0) {
        while (ipc_recv_for(SERVICE_CTX, g_dest_ep, &drop) == IPC_OK) {
        }
    }
    memset(g_threads, 0, sizeof(g_threads));
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        g_threads[i].tid = i + 1u;
        g_threads[i].state = THREAD_STATE_RUNNING;
        list_head_init(&g_threads[i].event_node);
        list_head_init(&g_threads[i].sched_node);
    }
    memset(g_procs, 0, sizeof(g_procs));
    g_procs[0].pid = CALLER_PID;
    g_procs[0].context_id = CALLER_CTX;
    g_procs[0].name = "test-caller";
    g_current_tid = 1;
    g_current_pid = CALLER_PID;
    g_now = 10;
    g_yield_calls = 0;
    g_yield_hook = 0;
    g_seen_src = 0;
    g_seen_reqid = 0;
    g_seen_type = 0;
    g_service_saw_request = 0;
}

/* Build the register frame a ring-3 WASMOS_SYSCALL_IPC_CALL arrives in: the
 * selector in RAX, the destination and message type in RDI/RSI, and arg0..arg3
 * in RDX/RCX/R8/R9. cs carries the ring-3 selector, which is what makes the
 * handler mirror the frame into the caller's thread resume context on the way
 * in, as it would for a real user trap. Arguments are taken as
 * 64-bit deliberately, so a case can plant bits above 32 that the handler must
 * refuse. */
static syscall_frame_t make_call(uint32_t dst, uint32_t type, uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3) {
    syscall_frame_t f;
    memset(&f, 0, sizeof(f));
    f.rax = WASMOS_SYSCALL_IPC_CALL;
    f.cs = 0x1Bu; /* ring 3 */
    f.rdi = dst;
    f.rsi = type;
    f.rdx = a0;
    f.rcx = a1;
    f.r8 = a2;
    f.r9 = a3;
    return f;
}

/* The peer service: take the request and answer it. `mutate` lets a test send
 * a deliberately bad reply without changing anything else about the flow. */
typedef enum {
    REPLY_GOOD = 0,
    REPLY_WRONG_REQUEST_ID,
    REPLY_NO_SOURCE,
    REPLY_FORGED_SOURCE,   /* claims the caller's own endpoint as source */
    REPLY_FOREIGN_SOURCE,  /* authentic-looking, but owned by a third context */
    REPLY_STALE_THEN_GOOD, /* an unrelated reply first, then the real one */
    REPLY_NOTHING
} reply_mode_t;

static reply_mode_t g_reply_mode;
static uint32_t g_foreign_ep; /* owned by a third context */

/* The peer, run as the one-shot yield hook while the caller is parked. It takes
 * one request off g_dest_ep, records what the request looked like from the
 * service side (source, request_id, type) and answers it according to
 * g_reply_mode -- correctly for REPLY_GOOD, not at all for REPLY_NOTHING, and
 * deliberately malformed otherwise. If no request is queued it returns without
 * recording anything, and the caller then observes a spurious wake.
 *
 * It receives with ipc_recv (kernel context), which skips the ownership check
 * the real service would pass on its own endpoint; the reply, in contrast, is
 * sent from SERVICE_CTX so it faces the same source check a real peer does. The
 * REPLY_FOREIGN_SOURCE case sends from the kernel context precisely because that
 * check would otherwise refuse the forged source outright, leaving the syscall's
 * own authenticity check untested. */
static void service_reply(void) {
    ipc_message_t req;
    if (ipc_recv(g_dest_ep, &req) != IPC_OK) {
        return;
    }
    g_service_saw_request = 1;
    g_seen_src = req.source;
    g_reply_ep = req.source;
    g_seen_reqid = req.request_id;
    g_seen_type = req.type;
    if (g_reply_mode == REPLY_NOTHING) {
        return;
    }

    ipc_message_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.type = req.type;
    rep.source = g_dest_ep;
    rep.request_id = req.request_id;
    rep.arg0 = req.arg0 ^ 0xF0F0u; /* something the caller can recognise */

    if (g_reply_mode == REPLY_STALE_THEN_GOOD) {
        ipc_message_t stale = rep;
        /* An id the caller has already retired. Deliberately NOT a future id:
         * the pending ring refuses those outright, which is
         * test_a_reply_for_a_future_id_is_refused (K15). */
        stale.request_id = req.request_id - 1u;
        stale.arg0 = 0xBADBAD00u;
        (void)ipc_send_from(SERVICE_CTX, req.source, &stale);
    }
    if (g_reply_mode == REPLY_WRONG_REQUEST_ID) {
        rep.request_id = req.request_id + 1u;
    }
    if (g_reply_mode == REPLY_NO_SOURCE) {
        rep.source = IPC_ENDPOINT_NONE;
    }
    if (g_reply_mode == REPLY_FORGED_SOURCE) {
        rep.source = req.source; /* the caller's own reply endpoint */
    }
    if (g_reply_mode == REPLY_FOREIGN_SOURCE) {
        rep.source = g_foreign_ep;
        (void)ipc_send_from(IPC_CONTEXT_KERNEL, req.source, &rep);
        return;
    }
    (void)ipc_send_from(SERVICE_CTX, req.source, &rep);
}

/* Run one IPC_CALL with the service answering in `mode`. */
static uint64_t call_with(reply_mode_t mode, syscall_frame_t* out_frame, uint32_t type,
                          uint32_t a0) {
    syscall_frame_t f = make_call(g_dest_ep, type, a0, 0, 0, 0);
    g_reply_mode = mode;
    g_yield_hook = service_reply;
    uint64_t rc = x86_syscall_handler(&f);
    if (out_frame) {
        *out_frame = f;
    }
    return rc;
}

/* ------------------------------------------------------------------ tests */

static void test_a_matching_reply_resolves_the_call(void) {
    reset();
    syscall_frame_t f;
    uint64_t rc = call_with(REPLY_GOOD, &f, 0x1234u, 0x55u);
    CHECK(rc == (uint64_t)IPC_OK, "an answered call returns IPC_OK");
    CHECK(g_service_saw_request, "the peer really received the request");
    CHECK(g_seen_type == 0x1234u, "with the message type the caller passed");
    CHECK(f.rdx == (uint64_t)(0x55u ^ 0xF0F0u), "and RDX carries the reply's arg0");
}

static void test_the_request_carries_a_nonzero_correlation_id(void) {
    reset();
    syscall_frame_t f;
    (void)call_with(REPLY_GOOD, &f, 1u, 1u);
    uint32_t first = g_seen_reqid;
    CHECK(first != 0, "a request_id is never zero — zero is the unset marker");
    (void)call_with(REPLY_GOOD, &f, 1u, 2u);
    CHECK(g_seen_reqid != first, "each call gets a fresh correlation id");
}

static void test_the_caller_gets_a_reply_endpoint_it_owns(void) {
    reset();
    syscall_frame_t f;
    (void)call_with(REPLY_GOOD, &f, 1u, 1u);
    uint32_t owner = 0xFFu;
    CHECK(g_seen_src != IPC_ENDPOINT_NONE, "the request names a reply endpoint");
    CHECK(ipc_endpoint_owner(g_seen_src, &owner) == IPC_OK && owner == CALLER_CTX,
          "owned by the calling context, so the peer's reply passes the send check");

    /* The endpoint is per-process and reused, not leaked per call. */
    uint32_t first = g_seen_src;
    (void)call_with(REPLY_GOOD, &f, 1u, 2u);
    CHECK(g_seen_src == first, "the reply endpoint is allocated once and reused");
}

/* If the reply endpoint is destroyed under the process — its context was
 * released — the next call must mint a new one rather than send into a dead
 * handle. */
static void test_a_stale_reply_endpoint_is_replaced(void) {
    reset();
    syscall_frame_t f;
    (void)call_with(REPLY_GOOD, &f, 1u, 1u);
    uint32_t first = g_seen_src;

    ipc_endpoints_release_owner(CALLER_CTX);

    uint64_t rc = call_with(REPLY_GOOD, &f, 1u, 2u);
    CHECK(rc == (uint64_t)IPC_OK, "the call still works after its reply endpoint went away");
    CHECK(g_seen_src != first, "a fresh reply endpoint was minted");
    uint32_t owner = 0xFFu;
    CHECK(ipc_endpoint_owner(g_seen_src, &owner) == IPC_OK && owner == CALLER_CTX,
          "and it is owned by the caller");
}

/* Invariant: the reply source endpoint is owned by the expected context. */
static void test_a_reply_from_the_wrong_source_is_refused(void) {
    reset();
    syscall_frame_t f;
    uint64_t rc = call_with(REPLY_FORGED_SOURCE, &f, 1u, 0x11u);
    CHECK(rc != (uint64_t)IPC_OK,
          "a reply claiming the caller's own endpoint as its source is not accepted");
    CHECK(f.rdx == 0, "and RDX is left clear rather than carrying the forged payload");

    reset();
    rc = call_with(REPLY_NO_SOURCE, &f, 1u, 0x22u);
    CHECK(rc != (uint64_t)IPC_OK, "a reply with no source endpoint is not accepted");
    CHECK(f.rdx == 0, "RDX stays clear");
}

static void test_a_reply_from_a_third_context_is_refused(void) {
    reset();
    /* An endpoint owned by neither the caller nor the peer. The reply is
     * well-formed and correctly correlated — only its owner is wrong. */
    uint32_t third_ctx = 99u;
    g_foreign_ep = 0;
    (void)ipc_endpoint_create(third_ctx, &g_foreign_ep);

    syscall_frame_t f;
    uint64_t rc = call_with(REPLY_FOREIGN_SOURCE, &f, 1u, 0x33u);
    CHECK(rc != (uint64_t)IPC_OK, "a correctly-correlated reply from a third party is refused");
    CHECK(f.rdx == 0, "RDX stays clear");
    ipc_endpoints_release_owner(third_ctx);
}

static void test_a_reply_with_the_wrong_id_does_not_resolve_the_call(void) {
    reset();
    syscall_frame_t f;
    uint64_t rc = call_with(REPLY_WRONG_REQUEST_ID, &f, 1u, 0x44u);
    CHECK(rc != (uint64_t)IPC_OK, "a reply carrying someone else's request_id does not resolve it");
    CHECK(f.rdx == 0, "RDX stays clear");
    /* No cleanup needed: the reply the peer planted for request_id+1 is refused
     * at the ring rather than retained, so it cannot leak into the next case.
     * K15 pins that. */
}

/* An unrelated reply arriving first must be kept, not dropped: it belongs to a
 * different in-flight call and the pending ring exists precisely to hold it. */
static void test_an_out_of_order_reply_is_retained(void) {
    reset();
    syscall_frame_t f;
    uint64_t rc = call_with(REPLY_STALE_THEN_GOOD, &f, 1u, 0x66u);
    CHECK(rc == (uint64_t)IPC_OK, "the matching reply still resolves the call");
    CHECK(f.rdx == (uint64_t)(0x66u ^ 0xF0F0u), "with the right payload, not the stale one");

    /* The stale one is still in the ring: a later call whose id happens to
     * match would find it. Prove it did not reach the caller as this call's
     * answer, which is the property that matters. */
    CHECK(f.rdx != 0xBADBAD00u, "the stale reply was never handed to the caller");
}

/* A peer that answers with request_id+1 is answering a call that does not
 * exist. Retaining it would let the NEXT call the process makes draw it, and
 * resolve that call with the previous call's payload without the peer having
 * answered it at all, because the authenticity check asks WHO replied and never
 * WHEN.
 *
 * Request ids come from one monotonic counter, so "at or beyond the next id to
 * be issued" is enough to tell a reply-to-nothing from a real one; the ring
 * refuses such a reply rather than retaining it. */
static void test_a_reply_for_a_future_id_is_refused(void) {
    reset();
    syscall_frame_t f;
    uint64_t rc = call_with(REPLY_WRONG_REQUEST_ID, &f, 1u, 0x99u);
    CHECK(rc != (uint64_t)IPC_OK, "the call answered with a future id is not resolved");

    /* The very next call draws exactly the id that reply carried. Nobody
     * answers it, so it must report EMPTY rather than inherit the payload. */
    g_reply_mode = REPLY_NOTHING;
    g_yield_hook = 0;
    syscall_frame_t g = make_call(g_dest_ep, 1u, 0xAAu, 0, 0, 0);
    g.rdx = 0xD00Du;
    rc = x86_syscall_handler(&g);
    CHECK(rc != (uint64_t)IPC_OK, "the next call is not resolved by the planted reply");
    CHECK(g.rdx != (uint64_t)(0x99u ^ 0xF0F0u), "and never sees the previous call's payload");
}

/* The ring's whole purpose: hold a reply whose call is not the one currently
 * blocked, so its owner finds it later. Driven through the ring directly,
 * because it is unreachable from a single-threaded caller -- an id already
 * issued is never drawn again, and one not yet issued is refused by the guard
 * above. The case it exists for is several threads sharing a reply endpoint,
 * one blocked on id 5 parking another's reply for id 6, which no host fixture
 * drives.
 *
 * Without it, a guard that rejected EVERY reply would leave the suite green. */
static void test_the_pending_ring_retains_an_issued_id(void) {
    reset();
    /* Give the slot an owner by making one ordinary call first. */
    syscall_frame_t warm;
    (void)call_with(REPLY_GOOD, &warm, 1u, 0x01u);

    const uint32_t issued = syscall_test_next_request_id() - 1u;
    ipc_message_t held;
    memset(&held, 0, sizeof(held));
    held.request_id = issued;
    held.source = g_dest_ep;
    held.arg0 = 0xFEEDu;

    CHECK(syscall_test_pending_enqueue(CALLER_PID, &held) == 0, "an issued id is retained");
    ipc_message_t taken;
    memset(&taken, 0, sizeof(taken));
    CHECK(syscall_test_pending_take(CALLER_PID, issued, &taken) == 0,
          "and its owner finds it later");
    CHECK(taken.arg0 == 0xFEEDu, "with its payload intact");
    CHECK(syscall_test_pending_take(CALLER_PID, issued, &taken) != 0, "only once");
}

/* The same guard, at the ring rather than through a call: an id that has not
 * been issued is refused outright, so it can never be there to be found. */
static void test_the_pending_ring_refuses_an_unissued_id(void) {
    reset();
    syscall_frame_t warm;
    (void)call_with(REPLY_GOOD, &warm, 1u, 0x01u);

    ipc_message_t future;
    memset(&future, 0, sizeof(future));
    future.request_id = syscall_test_next_request_id() + 4u;
    future.source = g_dest_ep;
    future.arg0 = 0xBADu;

    CHECK(syscall_test_pending_enqueue(CALLER_PID, &future) != 0, "an unissued id is refused");
    ipc_message_t taken;
    CHECK(syscall_test_pending_take(CALLER_PID, future.request_id, &taken) != 0,
          "so it is never there to answer the call that draws that id");
}

static void test_arguments_must_be_32_bit_clean(void) {
    reset();
    struct {
        const char* what;
        int field;
    } cases[] = {
        {"destination", 0},
        {"type", 1},
        {"arg0", 2},
        {"arg1", 3},
        {"arg2", 4},
        {"arg3", 5},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        syscall_frame_t f = make_call(g_dest_ep, 1u, 1u, 1u, 1u, 1u);
        uint64_t dirty = 0x100000001ull; /* a value that does not fit in 32 bits */
        switch (cases[i].field) {
        case 0:
            f.rdi = dirty;
            break;
        case 1:
            f.rsi = dirty;
            break;
        case 2:
            f.rdx = dirty;
            break;
        case 3:
            f.rcx = dirty;
            break;
        case 4:
            f.r8 = dirty;
            break;
        default:
            f.r9 = dirty;
            break;
        }
        f.rdx = (cases[i].field == 2) ? dirty : f.rdx;
        uint64_t rc = x86_syscall_handler(&f);
        CHECK(rc == (uint64_t)(int64_t)IPC_ERR_INVALID,
              "a syscall argument with bits above 32 is refused");
        CHECK(f.rdx == 0, "and RDX is cleared on the error path");
    }
    CHECK(g_yield_calls == 0, "none of the rejected calls blocked");
}

static void test_an_unknown_destination_is_refused(void) {
    reset();
    syscall_frame_t f = make_call(0x7FFFFFFFu, 1u, 1u, 0, 0, 0);
    f.rdx = 0xDEADBEEFu; /* stale register content the caller must not observe */
    uint64_t rc = x86_syscall_handler(&f);
    CHECK(rc == (uint64_t)(int64_t)IPC_ERR_NOENT,
          "an unknown destination reports NOENT, distinct from a malformed argument");
    CHECK(f.rdx == 0, "and the stale secondary return is cleared");
    CHECK(g_yield_calls == 0, "the rejection does not block");
}

/* Kernel-owned endpoints are not callable from ring 3 unless they are the one
 * endpoint explicitly opened for the smoke image. */
static void test_kernel_owned_destinations_are_refused(void) {
    reset();
    uint32_t kernel_ep = 0;
    (void)ipc_endpoint_create(IPC_CONTEXT_KERNEL, &kernel_ep);

    syscall_frame_t f = make_call(kernel_ep, 1u, 1u, 0, 0, 0);
    uint64_t rc = x86_syscall_handler(&f);
    CHECK(rc == (uint64_t)(int64_t)IPC_ERR_PERM, "a kernel-owned destination is PERM");
    CHECK(f.rdx == 0, "RDX stays clear");

    /* Opening it as the echo endpoint makes it reachable — and closing it
     * again takes the permission back. */
    syscall_set_ipc_call_echo_endpoint(kernel_ep);
    f = make_call(kernel_ep, 0x1111u, 0x77u, 0, 0, 0);
    rc = x86_syscall_handler(&f);
    CHECK(rc == (uint64_t)IPC_OK, "the designated echo endpoint answers");
    CHECK(f.rdx == 0x77u, "echoing arg0 back through RDX");

    syscall_set_ipc_call_echo_endpoint(IPC_ENDPOINT_NONE);
    f = make_call(kernel_ep, 1u, 1u, 0, 0, 0);
    rc = x86_syscall_handler(&f);
    CHECK(rc == (uint64_t)(int64_t)IPC_ERR_PERM, "and closing it restores the refusal");
}

static void test_a_denied_control_endpoint_is_refused(void) {
    reset();
    syscall_set_ipc_call_control_deny_endpoint(g_dest_ep);
    syscall_frame_t f = make_call(g_dest_ep, 1u, 1u, 0, 0, 0);
    uint64_t rc = x86_syscall_handler(&f);
    CHECK(rc == (uint64_t)(int64_t)IPC_ERR_PERM, "a denied control endpoint is refused");
    CHECK(f.rdx == 0, "RDX stays clear");
    syscall_set_ipc_call_control_deny_endpoint(IPC_ENDPOINT_NONE);

    f = make_call(g_dest_ep, 1u, 1u, 0, 0, 0);
    g_reply_mode = REPLY_GOOD;
    g_yield_hook = service_reply;
    rc = x86_syscall_handler(&f);
    CHECK(rc == (uint64_t)IPC_OK, "and lifting the denial restores it");
}

/* An unanswered call returns rather than wedging: the blocking receive reports
 * a spurious wake, which surfaces to ring 3 as IPC_EMPTY. Pinned because
 * IPC_EMPTY is a POSITIVE value -- a caller testing `rc < 0` for failure treats
 * it as success and then reads a stale RDX. */
static void test_an_unanswered_call_reports_empty_not_success(void) {
    reset();
    syscall_frame_t f;
    uint64_t rc = call_with(REPLY_NOTHING, &f, 1u, 0x88u);
    CHECK(g_service_saw_request, "the request was delivered");
    CHECK(rc != (uint64_t)IPC_OK, "an unanswered call does not report success");
    CHECK(rc == (uint64_t)IPC_EMPTY, "it reports IPC_EMPTY, the retry contract");
    CHECK(f.rdx == 0, "and leaves no payload behind");
}

/* ------------------------------------------------------------- IPC_NOTIFY */

static void test_notify_passes_the_transport_result_through(void) {
    reset();
    uint32_t note = 0;
    (void)ipc_notification_create(CALLER_CTX, &note);

    syscall_frame_t f;
    memset(&f, 0, sizeof(f));
    f.rax = WASMOS_SYSCALL_IPC_NOTIFY;
    f.cs = 0x1Bu;
    f.rdi = note;
    CHECK(x86_syscall_handler(&f) == (uint64_t)IPC_OK, "the owner may notify its endpoint");
    CHECK(ipc_wait_for(CALLER_CTX, note) == IPC_OK, "and the signal really landed");

    f.rdi = 0x7FFFFFFFu;
    CHECK(x86_syscall_handler(&f) == (uint64_t)(int64_t)IPC_ERR_NOENT,
          "an unknown endpoint reports NOENT");

    uint32_t foreign = 0;
    (void)ipc_notification_create(SERVICE_CTX, &foreign);
    f.rdi = foreign;
    CHECK(x86_syscall_handler(&f) == (uint64_t)(int64_t)IPC_ERR_PERM,
          "another context's notification endpoint is PERM");

    f.rdi = 0x100000001ull;
    CHECK(x86_syscall_handler(&f) == (uint64_t)(int64_t)IPC_ERR_INVALID,
          "an endpoint id with bits above 32 is refused");

    syscall_set_ipc_notify_control_deny_endpoint(note);
    f.rdi = note;
    CHECK(x86_syscall_handler(&f) == (uint64_t)IPC_OK,
          "the notify deny endpoint is a trace hook, not a gate");
    syscall_set_ipc_notify_control_deny_endpoint(IPC_ENDPOINT_NONE);
}

/* -------------------------------------------------------------------- main */

/* Initialise the IPC layer and the peer's endpoint once, then run every case in
 * a shuffled order. Returns 0 only when every CHECK passed and 1 otherwise; on
 * failure the shuffle seed is printed so the order can be replayed through
 * WASMOS_TEST_SEED. The endpoint table and the request-id counter carry across
 * cases, which is what reset() exists to compensate for. */
int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"K1 a matching reply resolves the call", test_a_matching_reply_resolves_the_call},
        {"K2 the request carries a nonzero id", test_the_request_carries_a_nonzero_correlation_id},
        {"K3 the caller gets a reply endpoint it owns",
         test_the_caller_gets_a_reply_endpoint_it_owns},
        {"K4 a stale reply endpoint is replaced", test_a_stale_reply_endpoint_is_replaced},
        {"K5 a reply from the wrong source is refused",
         test_a_reply_from_the_wrong_source_is_refused},
        {"K6 a reply from a third context is refused",
         test_a_reply_from_a_third_context_is_refused},
        {"K7 a wrong-id reply does not resolve",
         test_a_reply_with_the_wrong_id_does_not_resolve_the_call},
        {"K8 an out-of-order reply is retained", test_an_out_of_order_reply_is_retained},
        {"K9 arguments must be 32-bit clean", test_arguments_must_be_32_bit_clean},
        {"K15 a reply for a future id is refused", test_a_reply_for_a_future_id_is_refused},
        {"K16 the pending ring retains an issued id", test_the_pending_ring_retains_an_issued_id},
        {"K17 the pending ring refuses an unissued id",
         test_the_pending_ring_refuses_an_unissued_id},
        {"K10 an unknown destination is refused", test_an_unknown_destination_is_refused},
        {"K11 kernel-owned destinations are refused", test_kernel_owned_destinations_are_refused},
        {"K12 a denied control endpoint is refused", test_a_denied_control_endpoint_is_refused},
        {"K13 an unanswered call reports EMPTY", test_an_unanswered_call_reports_empty_not_success},
        {"K14 notify passes the transport result through",
         test_notify_passes_the_transport_result_through},
    };

    ipc_init();
    reset();
    (void)ipc_endpoint_create(SERVICE_CTX, &g_dest_ep);

    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    const int test_count = (int)(sizeof(tests) / sizeof(tests[0]));
    int order[WASMOS_TEST_MAX_CASES];
    const uint64_t seed = wasmos_test_shuffle(order, test_count);

    for (int i = 0; i < test_count; ++i) {
        int before = g_failures;
        printf("  ... %s\n", tests[order[i]].name);
        fflush(stdout);
        tests[order[i]].fn();
        if (g_failures != before) {
            printf("[fail] %s\n", tests[order[i]].name);
        }
    }
    printf("test_syscall_ipc: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}