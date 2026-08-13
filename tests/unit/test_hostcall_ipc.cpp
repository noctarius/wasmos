/* test_hostcall_ipc.cpp — the IPC host-call shims, both runtimes, side by side.
 *
 * These are the last layer before a guest: they validate the guest's arguments,
 * translate handles, and call ipc.c. Nothing tested them, and nothing checked
 * that the two runtimes agree — which matters because an app is supposed to
 * behave identically under wasm3 and WARP, and the shims are where that
 * promise is actually kept or broken.
 *
 * Both are driven from ONE binary so every scenario is run through both and the
 * results compared directly. wasm3's ABI is pure stack marshalling (arguments
 * and the return value are slots in _sp), so calling one needs a stack array and
 * nothing else; WARP's take an unused per-call context pointer. Neither needs a
 * live engine, module or instance.
 *
 * The kernel side is real: the tests link the actual ipc.c, poll.c and
 * sched_event.c, so a shim's return value is produced by the transport it
 * really talks to. Only the process/thread environment is stubbed.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "test_shuffle.h"

extern "C" {
#include "ipc.h"
#include "process.h"
#include "sched.h"
#include "sched_event.h"
#include "thread.h"
}

#include "warp/link_ipc.h"
#include "wasm3/link_ipc.h"

static int g_failures;
static int g_checks;

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
#define CALLER_PID 1u
#define CALLER_CTX 77u
#define OTHER_CTX 88u

static thread_t g_threads[POOL_MAX];
static process_t g_proc;
static uint32_t g_current_tid = 1;
static int g_have_process = 1;
static int g_yield_calls;

extern "C" {

uint64_t timer_ticks(void) {
    return 0;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}
thread_t* thread_table_at(uint32_t i) {
    return (i < POOL_MAX) ? &g_threads[i] : nullptr;
}
thread_t* thread_get(uint32_t tid) {
    return (tid == 0 || tid > POOL_MAX) ? nullptr : &g_threads[tid - 1u];
}
uint32_t thread_current_tid(void) {
    return g_current_tid;
}
void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    thread_t* t = thread_get(tid);
    if (t) {
        t->state = state;
        t->block_reason = reason;
    }
}
void sched_wake_thread(thread_t* t) {
    if (t) {
        t->state = THREAD_STATE_READY;
    }
}

/* A blocking shim would spin forever on an endpoint nobody feeds, so the stub
 * models the spurious wake the API documents: unlink and resume. Tests that
 * exercise a blocking shim queue the message first. */
void process_yield(process_run_result_t result) {
    (void)result;
    g_yield_calls++;
    thread_t* self = thread_get(g_current_tid);
    if (self && self->wait_event) {
        sched_event_t* ev = self->wait_event;
        ksync_spinlock_lock(&ev->lock);
        if (self->wait_event == ev) {
            list_head_del(&self->event_node);
            self->wait_event = nullptr;
        }
        ksync_spinlock_unlock(&ev->lock);
    }
    if (self) {
        self->state = THREAD_STATE_RUNNING;
    }
}

uint32_t process_current_pid(void) {
    return g_have_process ? CALLER_PID : 0u;
}
process_t* process_get(uint32_t pid) {
    return (g_have_process && pid == CALLER_PID) ? &g_proc : nullptr;
}
void preempt_safepoint(void) {}
void klog_write(const char* s) {
    (void)s;
}
void serial_write_hex64(uint64_t v) {
    (void)v;
}

/* wasm3 needs this symbol for m3ApiReturn's success path; its value is never
 * inspected here, only the slot the shim wrote. */
const char* const m3Err_none = nullptr;

/* The per-pid side tables. In the kernel these live in link.c/link.cpp; a
 * single-process test needs one slot each, and both runtimes get their own so
 * neither can observe the other's state. */
static wasm_ipc_last_slot_t g_w3_slot;
static wasm_fs_peer_slot_t g_w3_peer;
static WarpIpcLastSlot g_warp_slot;
static WarpFsPeerSlot g_warp_peer;

wasm_ipc_last_slot_t* wasm_ipc_slot_for_pid(uint32_t pid) {
    return pid ? &g_w3_slot : nullptr;
}
wasm_fs_peer_slot_t* wasm_fs_peer_slot_for_pid(uint32_t pid) {
    return pid ? &g_w3_peer : nullptr;
}
WarpIpcLastSlot* warp_ipc_slot_for_pid(uint32_t pid) {
    return pid ? &g_warp_slot : nullptr;
}
WarpFsPeerSlot* warp_fs_peer_slot_for_pid(uint32_t pid) {
    return pid ? &g_warp_peer : nullptr;
}
uint8_t warp_dbg_ipc_trace_process(process_t* proc) {
    (void)proc;
    return 0;
}

int current_process_context(uint32_t* out) {
    if (!g_have_process || !out) {
        return -1;
    }
    *out = g_proc.context_id;
    return 0;
}
int warp_current_context_id(uint32_t* out) {
    return current_process_context(out);
}

} /* extern "C" */

/* ------------------------------------------- wasm3 raw-call invocation */

/* m3ApiReturnType takes _sp[0] as the return slot and m3ApiGetArg walks
 * forward from _sp[1], so a call is just a stack array. */
static int32_t w3_call(M3RawCall fn, std::initializer_list<int32_t> args) {
    uint64_t sp[16];
    memset(sp, 0, sizeof(sp));
    unsigned i = 1;
    for (int32_t a : args) {
        sp[i++] = (uint64_t)(uint32_t)a;
    }
    (void)fn(nullptr, nullptr, sp, nullptr);
    return (int32_t)(uint32_t)sp[0];
}

/* --------------------------------------------------------------- fixture */

struct Env {
    uint32_t msg_ep;
    uint32_t note_ep;
    uint32_t foreign_ep;
    uint32_t ctx;
};
static Env g_env;

static void reset(void) {
    memset(g_threads, 0, sizeof(g_threads));
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        g_threads[i].tid = i + 1u;
        g_threads[i].state = THREAD_STATE_RUNNING;
        list_head_init(&g_threads[i].event_node);
        list_head_init(&g_threads[i].sched_node);
    }
    memset(&g_proc, 0, sizeof(g_proc));
    g_proc.pid = CALLER_PID;
    g_proc.context_id = CALLER_CTX;
    g_proc.name = "hostcall-test";
    g_current_tid = 1;
    g_have_process = 1;
    g_yield_calls = 0;
    memset(&g_w3_slot, 0, sizeof(g_w3_slot));
    memset(&g_w3_peer, 0, sizeof(g_w3_peer));
    memset(&g_warp_slot, 0, sizeof(g_warp_slot));
    memset(&g_warp_peer, 0, sizeof(g_warp_peer));

    if (g_env.ctx) {
        ipc_endpoints_release_owner(g_env.ctx);
        ipc_endpoints_release_owner(OTHER_CTX);
    }
    g_env.ctx = CALLER_CTX;
    g_env.msg_ep = g_env.note_ep = g_env.foreign_ep = 0;
    (void)ipc_endpoint_create(CALLER_CTX, &g_env.msg_ep);
    (void)ipc_notification_create(CALLER_CTX, &g_env.note_ep);
    (void)ipc_endpoint_create(OTHER_CTX, &g_env.foreign_ep);
}

static int ksend(uint32_t ep, uint32_t arg0) {
    ipc_message_t m;
    memset(&m, 0, sizeof(m));
    m.type = 0x1234u;
    m.source = IPC_ENDPOINT_NONE;
    m.request_id = 0x99u;
    m.arg0 = arg0;
    return ipc_send(ep, &m);
}

/* ------------------------------------------------- per-runtime adapters */

/* One scenario, run through both runtimes. Each adapter returns whatever the
 * shim gave back, so the comparison is of guest-observable values only. */
struct Shims {
    const char* name;
    int32_t (*create_endpoint)(void);
    int32_t (*endpoint_owner)(int32_t ep);
    int32_t (*send)(int32_t dst, int32_t src, int32_t type, int32_t rid, int32_t a0);
    int32_t (*drain)(int32_t ep);
    int32_t (*notify)(int32_t ep);
    int32_t (*last_field)(int32_t field);
    int32_t (*select_create)(void);
    int32_t (*select_add)(int32_t sel, int32_t ep);
    int32_t (*select_wait_timeout)(int32_t sel, int32_t ms);
    int32_t (*select_destroy)(int32_t sel);
};

static int32_t w3_create_endpoint(void) {
    return w3_call(wasmos_ipc_create_endpoint, {});
}
static int32_t w3_endpoint_owner(int32_t ep) {
    return w3_call(wasmos_ipc_endpoint_owner, {ep});
}
static int32_t w3_send(int32_t d, int32_t s, int32_t t, int32_t r, int32_t a0) {
    return w3_call(wasmos_ipc_send, {d, s, t, r, a0, 0, 0, 0});
}
static int32_t w3_drain(int32_t ep) {
    return w3_call(wasmos_ipc_drain, {ep});
}
static int32_t w3_notify(int32_t ep) {
    return w3_call(wasmos_ipc_notify, {ep});
}
static int32_t w3_last_field(int32_t f) {
    return w3_call(wasmos_ipc_last_field, {f});
}
static int32_t w3_select_create(void) {
    return w3_call(wasmos_sys_select_create, {});
}
static int32_t w3_select_add(int32_t sel, int32_t ep) {
    return w3_call(wasmos_sys_select_add, {sel, ep});
}
static int32_t w3_select_wait_timeout(int32_t sel, int32_t ms) {
    return w3_call(wasmos_sys_select_wait_timeout, {sel, ms});
}
static int32_t w3_select_destroy(int32_t sel) {
    return w3_call(wasmos_sys_select_destroy, {sel});
}

static int32_t wp_create_endpoint(void) {
    return (int32_t)warp_ipc_create_endpoint(nullptr);
}
static int32_t wp_endpoint_owner(int32_t ep) {
    return (int32_t)warp_ipc_endpoint_owner((uint32_t)ep, nullptr);
}
static int32_t wp_send(int32_t d, int32_t s, int32_t t, int32_t r, int32_t a0) {
    return (int32_t)warp_ipc_send((uint32_t)d, (uint32_t)s, (uint32_t)t, (uint32_t)r, (uint32_t)a0,
                                  0, 0, 0, nullptr);
}
static int32_t wp_drain(int32_t ep) {
    return (int32_t)warp_ipc_drain((uint32_t)ep, nullptr);
}
static int32_t wp_notify(int32_t ep) {
    return (int32_t)warp_ipc_notify((uint32_t)ep, nullptr);
}
static int32_t wp_last_field(int32_t f) {
    return (int32_t)warp_ipc_last_field((uint32_t)f, nullptr);
}
static int32_t wp_select_create(void) {
    return (int32_t)warp_ipc_select_create(nullptr);
}
static int32_t wp_select_add(int32_t sel, int32_t ep) {
    return (int32_t)warp_ipc_select_add((uint32_t)sel, (uint32_t)ep, nullptr);
}
static int32_t wp_select_wait_timeout(int32_t sel, int32_t ms) {
    return (int32_t)warp_ipc_select_wait_timeout((uint32_t)sel, (uint32_t)ms, nullptr);
}
static int32_t wp_select_destroy(int32_t sel) {
    return (int32_t)warp_ipc_select_destroy((uint32_t)sel, nullptr);
}

static const Shims k_wasm3 = {
    "wasm3",          w3_create_endpoint, w3_endpoint_owner, w3_send,       w3_drain,
    w3_notify,        w3_last_field,      w3_select_create,  w3_select_add, w3_select_wait_timeout,
    w3_select_destroy};
static const Shims k_warp = {
    "WARP",           wp_create_endpoint, wp_endpoint_owner, wp_send,       wp_drain,
    wp_notify,        wp_last_field,      wp_select_create,  wp_select_add, wp_select_wait_timeout,
    wp_select_destroy};

/* ------------------------------------------------------- scenario table */

/* A scenario returns one guest-observable value. Run through both runtimes,
 * asserted against the expectation AND against each other. */
/* Per-runtime expectations. Where they differ, the two runtimes genuinely
 * disagree about what a guest observes -- recorded here rather than hidden
 * behind a single loose assertion, with `divergent` marking it deliberate.
 *
 * A divergent row is checked BOTH ways: the values must still differ. Fixing
 * one therefore fails this test and forces the row to be updated, so the list
 * cannot silently rot into a description of an already-fixed problem. */
struct Scenario {
    const char* what;
    int32_t expect_wasm3;
    int32_t expect_warp;
    bool divergent;
    int32_t (*run)(const Shims& s);
    const char* note;
};

static int32_t s_create_ok(const Shims& s) {
    return s.create_endpoint() > 0 ? 1 : 0;
}
static int32_t s_create_no_process(const Shims& s) {
    g_have_process = 0;
    int32_t rc = s.create_endpoint();
    g_have_process = 1;
    return rc;
}
static int32_t s_owner_ok(const Shims& s) {
    return s.endpoint_owner((int32_t)g_env.msg_ep);
}
static int32_t s_owner_negative(const Shims& s) {
    return s.endpoint_owner(-1);
}
static int32_t s_owner_unknown(const Shims& s) {
    return s.endpoint_owner(0x7FFFFFFF);
}
static int32_t s_send_ok(const Shims& s) {
    return s.send((int32_t)g_env.msg_ep, (int32_t)g_env.msg_ep, 7, 1, 42);
}
static int32_t s_send_negative_dst(const Shims& s) {
    return s.send(-1, (int32_t)g_env.msg_ep, 7, 1, 42);
}
static int32_t s_send_negative_src(const Shims& s) {
    return s.send((int32_t)g_env.msg_ep, -1, 7, 1, 42);
}
static int32_t s_send_unknown_dst(const Shims& s) {
    return s.send(0x7FFFFFFF, (int32_t)g_env.msg_ep, 7, 1, 42);
}
static int32_t s_send_foreign_src(const Shims& s) {
    return s.send((int32_t)g_env.msg_ep, (int32_t)g_env.foreign_ep, 7, 1, 42);
}
static int32_t s_send_to_notification(const Shims& s) {
    return s.send((int32_t)g_env.note_ep, (int32_t)g_env.msg_ep, 7, 1, 42);
}
/* The documented drain contract: 0 on empty, never -1. */
static int32_t s_drain_empty(const Shims& s) {
    return s.drain((int32_t)g_env.msg_ep);
}
static int32_t s_drain_has_message(const Shims& s) {
    (void)ksend(g_env.msg_ep, 5u);
    return s.drain((int32_t)g_env.msg_ep);
}
static int32_t s_drain_negative(const Shims& s) {
    return s.drain(-1);
}
static int32_t s_drain_unknown(const Shims& s) {
    return s.drain(0x7FFFFFFF);
}
static int32_t s_drain_notification(const Shims& s) {
    return s.drain((int32_t)g_env.note_ep);
}
static int32_t s_last_field_before_receive(const Shims& s) {
    return s.last_field(0);
}
static int32_t s_last_field_after_receive(const Shims& s) {
    (void)ksend(g_env.msg_ep, 0xABu);
    (void)s.drain((int32_t)g_env.msg_ep);
    return s.last_field(2); /* WASMOS_IPC_FIELD_ARG0 */
}
static int32_t s_last_field_out_of_range(const Shims& s) {
    (void)ksend(g_env.msg_ep, 1u);
    (void)s.drain((int32_t)g_env.msg_ep);
    return s.last_field(99);
}
static int32_t s_notify_ok(const Shims& s) {
    return s.notify((int32_t)g_env.note_ep);
}
static int32_t s_notify_negative(const Shims& s) {
    return s.notify(-1);
}
static int32_t s_notify_message_ep(const Shims& s) {
    return s.notify((int32_t)g_env.msg_ep);
}
static int32_t s_notify_foreign(const Shims& s) {
    uint32_t foreign_note = 0;
    (void)ipc_notification_create(OTHER_CTX, &foreign_note);
    return s.notify((int32_t)foreign_note);
}
static int32_t s_select_create_ok(const Shims& s) {
    int32_t sel = s.select_create();
    if (sel > 0) {
        (void)s.select_destroy(sel);
    }
    return sel > 0 ? 1 : 0;
}
static int32_t s_select_add_ok(const Shims& s) {
    int32_t sel = s.select_create();
    int32_t rc = s.select_add(sel, (int32_t)g_env.msg_ep);
    (void)s.select_destroy(sel);
    return rc;
}
static int32_t s_select_add_bad_set(const Shims& s) {
    return s.select_add(0x7FFF, (int32_t)g_env.msg_ep);
}
static int32_t s_select_add_negative_set(const Shims& s) {
    return s.select_add(-1, (int32_t)g_env.msg_ep);
}
static int32_t s_select_add_negative_ep(const Shims& s) {
    int32_t sel = s.select_create();
    int32_t rc = s.select_add(sel, -1);
    (void)s.select_destroy(sel);
    return rc;
}
/* The timed wait separates "nothing was ready" from "the call failed", which
 * the untimed one cannot. */
static int32_t s_select_wait_timeout_expires(const Shims& s) {
    int32_t sel = s.select_create();
    (void)s.select_add(sel, (int32_t)g_env.msg_ep);
    int32_t rc = s.select_wait_timeout(sel, 1);
    (void)s.select_destroy(sel);
    return rc;
}
static int32_t s_select_wait_timeout_ready(const Shims& s) {
    int32_t sel = s.select_create();
    (void)s.select_add(sel, (int32_t)g_env.msg_ep);
    (void)ksend(g_env.msg_ep, 1u);
    int32_t rc = s.select_wait_timeout(sel, 1);
    (void)s.select_destroy(sel);
    return rc == (int32_t)g_env.msg_ep ? 1 : rc;
}
static int32_t s_select_wait_timeout_bad_set(const Shims& s) {
    return s.select_wait_timeout(0x7FFF, 1);
}
static int32_t s_select_destroy_bad_set(const Shims& s) {
    return s.select_destroy(0x7FFF);
}

static const Scenario k_scenarios[] = {
    {"create_endpoint(valid) returns a handle", 1, 1, false, s_create_ok, nullptr},
    {"create_endpoint(no current process)", -4, -4, false, s_create_no_process, nullptr},
    {"endpoint_owner(owned)", (int32_t)CALLER_CTX, (int32_t)CALLER_CTX, false, s_owner_ok, nullptr},
    {"endpoint_owner(negative handle)", -1, -1, false, s_owner_negative, nullptr},
    {"endpoint_owner(unknown)", -4, -4, false, s_owner_unknown, nullptr},

    {"send(valid)", 0, 0, false, s_send_ok, nullptr},
    {"send(negative destination)", -1, -1, false, s_send_negative_dst,
     "a malformed handle is INVALID in both. WARP used to cast it to "
     "IPC_ENDPOINT_NONE first and report whatever the transport then said"},
    {"send(negative source)", -1, -1, false, s_send_negative_src,
     "same cause: WARP's cast turns the guest's -1 into a source it does not own, "
     "so the answer is DENIED rather than a rejected argument"},
    {"send(unknown destination)", -4, -4, false, s_send_unknown_dst,
     "the transport code reaches the guest: NOENT is distinguishable from a "
     "malformed argument"},
    {"send(source owned by another context)", -2, -2, false, s_send_foreign_src, nullptr},
    {"send(destination is a notification endpoint)", -7, -7, false, s_send_to_notification,
     nullptr},

    {"drain(empty) is 0, not an error", 0, 0, false, s_drain_empty, nullptr},
    {"drain(has message)", 1, 1, false, s_drain_has_message, nullptr},
    {"drain(negative handle)", -1, -1, false, s_drain_negative, nullptr},
    {"drain(unknown endpoint)", -4, -4, false, s_drain_unknown, nullptr},
    {"drain(notification endpoint)", -7, -7, false, s_drain_notification, nullptr},

    {"last_field before any receive", -4, -4, false, s_last_field_before_receive, nullptr},
    {"last_field(arg0) after receive", 0xAB, 0xAB, false, s_last_field_after_receive, nullptr},
    {"last_field(out of range)", -1, -1, false, s_last_field_out_of_range, nullptr},

    {"notify(valid)", 0, 0, false, s_notify_ok, nullptr},
    {"notify(negative handle)", -1, -1, false, s_notify_negative,
     "a malformed handle, not a transport failure"},
    {"notify(message endpoint)", -7, -7, false, s_notify_message_ep,
     "UNSUPPORTED: both runtimes now say WHY, so a guest can tell a type mismatch "
     "from a bad handle"},
    {"notify(another context's endpoint)", -2, -2, false, s_notify_foreign,
     "DENIED, distinct from both of the above"},

    {"select_create(valid) returns a handle", 1, 1, false, s_select_create_ok, nullptr},
    {"select_add(valid)", 0, 0, false, s_select_add_ok, nullptr},
    {"select_add(unknown set)", -4, -4, false, s_select_add_bad_set, nullptr},
    {"select_add(negative set)", -1, -1, false, s_select_add_negative_set, nullptr},
    {"select_add(negative endpoint)", -1, -1, false, s_select_add_negative_ep,
     "was the one divergence that was a bug rather than a reporting difference: "
     "WARP's cast made it IPC_ENDPOINT_NONE and ipc_select_add recorded it without "
     "resolving, so the set silently never signalled on that slot while the guest "
     "was told it was watched. ipc_select_add now refuses an endpoint it cannot "
     "resolve, which converges both runtimes on a rejection"},

    {"select_wait_timeout(expires)", -5, -5, false, s_select_wait_timeout_expires,
     "WASMOS_TIMEOUT from the generated axis, not a private -1: a guest can now tell "
     "'the window elapsed' from 'you passed a bad argument'"},
    {"select_wait_timeout(ready) names the endpoint", 1, 1, false, s_select_wait_timeout_ready,
     nullptr},
    {"select_wait_timeout(unknown set)", -4, -4, false, s_select_wait_timeout_bad_set,
     "the transport code, like every other call -- this was a private -2"},
    {"select_destroy(unknown set)", 0, 0, false, s_select_destroy_bad_set,
     "destroy returns void from the transport, so nothing can be reported"},
};

/* -------------------------------------------------------------------- main */

int main(void) {
    ipc_init();
    reset();

    int divergences = 0;
    /* Randomized order: these scenarios share the fake IPC fabric through
     * reset(), so one leaving state behind must not be able to make the next
     * pass. Replay a failure with WASMOS_TEST_SEED. */
    const int scenario_count = (int)(sizeof(k_scenarios) / sizeof(k_scenarios[0]));
    int order[WASMOS_TEST_MAX_CASES];
    const uint64_t seed = wasmos_test_shuffle(order, scenario_count);

    for (int i = 0; i < scenario_count; ++i) {
        const Scenario& sc = k_scenarios[order[i]];
        reset();
        int32_t w3 = sc.run(k_wasm3);
        reset();
        int32_t wp = sc.run(k_warp);

        g_checks += 2;
        if (w3 != sc.expect_wasm3) {
            g_failures++;
            printf("  [FAIL] wasm3 %s: expected %d, got %d\n", sc.what, sc.expect_wasm3, w3);
        }
        if (wp != sc.expect_warp) {
            g_failures++;
            printf("  [FAIL] WARP  %s: expected %d, got %d\n", sc.what, sc.expect_warp, wp);
        }

        g_checks++;
        if (sc.divergent) {
            divergences++;
            /* Asserted to STILL differ, so unifying them fails here and forces
               this row to be reclassified rather than left describing history. */
            if (w3 == wp) {
                g_failures++;
                printf("  [FAIL] %s no longer diverges (both %d) -- update the table\n", sc.what,
                       w3);
            }
        } else if (w3 != wp) {
            g_failures++;
            printf("  [FAIL] parity %s: wasm3=%d WARP=%d\n", sc.what, w3, wp);
        }
    }
    printf("  ... %d of %zu scenarios differ between the runtimes\n", divergences,
           sizeof(k_scenarios) / sizeof(k_scenarios[0]));

    printf("test_hostcall_ipc: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
