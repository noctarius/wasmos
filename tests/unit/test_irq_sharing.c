/* Host unit test for the per-line IRQ sharer bookkeeping (src/kernel/irq_sharing.c).
 *
 * The rules under test exist because PCI INTx is wire-OR'd — several devices can
 * assert one line — and because the kernel masks a line on dispatch and reopens
 * it only when every sharer has reported (line 0, the timer, is never masked).
 * Getting that accounting wrong either livelocks the machine (reopening while a
 * device still asserts) or kills a device permanently (never reopening). See
 * docs/architecture/05-x86-cpu-architecture.md §Interrupt Controller and IRQ
 * Routing.
 *
 * src/kernel/irq_sharing.c is the only source linked in: it holds no state and
 * hides no platform dependency, so the line table and every environmental effect
 * come from this file. What the fakes below stand in for on target
 * (src/kernel/arch/x86_64/irq_x86_64.c) is noted at each one; the differences
 * that matter are that time only advances when a case advances it, and that the
 * real caller runs dispatch/tick under g_irq_lines_lock while this module itself
 * takes no lock. */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "irq_sharing.h"

/* Table geometry. TEST_LINES matches the kernel's IRQ_COUNT (the 16 legacy PIC
 * lines), so a line index used here is one the real table would also accept. */
#define TEST_LINES 16u
#define LINE 11u /* the shared virtio-net/virtio-rng line in QEMU */
#define TIMER_LINE 0u

static int g_failures;
static int g_checks;

/* Record one assertion: counts and CONTINUES, so a failing case runs to its end
 * and every later assertion in it still reports. Nothing here returns a marker;
 * main() reports g_failures as the process exit status. */
static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

/* ---- fake ops -------------------------------------------------------------- */

/* Call log for the injected ops. Each array keeps only its first 64 entries
 * while the matching counter keeps counting, so a count stays exact past 64 but
 * the recorded argument list goes stale — IRQ_DISPATCH_BUDGET_PER_TICK is 64, so
 * the budget case sits exactly on that boundary. g_now is the fake tick counter
 * in the same unit as IRQ_ACK_DEADLINE_TICKS; it starts at 100 (any non-zero
 * base) and only moves when a case moves it. */
static uint32_t g_mask_calls[64];
static uint32_t g_mask_count;
static uint32_t g_unmask_calls[64];
static uint32_t g_unmask_count;
static uint32_t g_delivered_to[64];
static uint32_t g_deliver_count;
static uint32_t g_throttle_logged_line;
static uint32_t g_throttle_log_count;
static uint64_t g_now;
/* Endpoint whose delivery fails, modelling a full endpoint queue. 0 = none. */
static uint32_t g_deliver_fail_endpoint;

/* Stand in for x86_irq_mask/x86_irq_unmask, which program the PIC or IO-APIC.
 * These only record, so a case observes the mask decision without any controller
 * state: nothing here prevents a mask that the hardware would reject. */
static void fake_mask(uint32_t line) {
    if (g_mask_count < 64) {
        g_mask_calls[g_mask_count] = line;
    }
    g_mask_count++;
}

static void fake_unmask(uint32_t line) {
    if (g_unmask_count < 64) {
        g_unmask_calls[g_unmask_count] = line;
    }
    g_unmask_count++;
}

/* Stand in for irq_ops_deliver, which posts an IPC_IRQ_EVENT_TYPE message with
 * ipc_send_from and returns -1 whenever that is not IPC_OK. Failure is modelled
 * by endpoint id rather than by queue depth, so it is deterministic and a
 * "queue" here never drains: an endpoint set in g_deliver_fail_endpoint refuses
 * every delivery for the rest of the case. Returns 0 when queued, matching the
 * ops contract in irq_sharing.h. */
static int fake_deliver(uint32_t endpoint, uint32_t line) {
    (void)line;
    if (g_deliver_fail_endpoint != 0 && endpoint == g_deliver_fail_endpoint) {
        return -1;
    }
    if (g_deliver_count < 64) {
        g_delivered_to[g_deliver_count] = endpoint;
    }
    g_deliver_count++;
    return 0;
}

/* Stand in for timer_ticks(), which the timer interrupt advances. Time is
 * therefore inert unless a case writes g_now, which is what lets the ack
 * deadline be crossed exactly rather than waited out. */
static uint64_t fake_now(void) {
    return g_now;
}

/* Stand in for irq_ops_log_throttle, which writes a diagnostic to the serial
 * port. Captures the line and the call count so the once-per-throttle
 * rate-limiting is observable. */
static void fake_log_throttle(uint32_t line) {
    g_throttle_logged_line = line;
    g_throttle_log_count++;
}

static const irq_sharing_ops_t OPS = {
    fake_mask, fake_unmask, fake_deliver, fake_now, fake_log_throttle,
};

static irq_line_t g_lines[TEST_LINES];

/* Fixture reset: an empty line table plus a cleared call log, called first in
 * every case because the cases run in a shuffled order and share both. */
static void reset(void) {
    memset(g_lines, 0, sizeof(g_lines));
    irq_sharing_init(g_lines, TEST_LINES);
    g_mask_count = 0;
    g_unmask_count = 0;
    g_deliver_count = 0;
    g_throttle_log_count = 0;
    g_throttle_logged_line = 0;
    g_deliver_fail_endpoint = 0;
    g_now = 100;
}

/* 1 when `endpoint` appears in the recorded deliveries, which says nothing about
 * how many times or in what order; both helpers see only the first 64 calls. */
static int delivered_to(uint32_t endpoint) {
    for (uint32_t i = 0; i < g_deliver_count && i < 64; ++i) {
        if (g_delivered_to[i] == endpoint) {
            return 1;
        }
    }
    return 0;
}

/* How many recorded unmasks name `line`. Cases zero g_unmask_count first so the
 * answer covers only the step under test rather than the whole setup. */
static uint32_t unmasks_of(uint32_t line) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_unmask_count && i < 64; ++i) {
        if (g_unmask_calls[i] == line) {
            n++;
        }
    }
    return n;
}

/* ---- tests ----------------------------------------------------------------- */

/* Registering a second context must ADD a sharer: overwriting the first would
 * silently steal the line and stop that driver's interrupts. */
static void test_register_adds_not_replaces(void) {
    reset();
    expect(irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS) == 0, "first register ok");
    expect(irq_sharing_register(g_lines, LINE, 20, 0x200, &OPS) == 0, "second register ok");
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(g_deliver_count == 2, "dispatch reaches both sharers");
    expect(delivered_to(0x100) && delivered_to(0x200), "both endpoints addressed");
}

/* Re-registering the same context updates its endpoint rather than consuming a
 * second slot. */
static void test_reregister_same_context_updates(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    (void)irq_sharing_register(g_lines, LINE, 10, 0x1FF, &OPS);
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(g_deliver_count == 1, "still one sharer");
    expect(delivered_to(0x1FF), "endpoint was updated");
}

static void test_register_full_line_rejected(void) {
    reset();
    for (uint32_t i = 0; i < IRQ_SHARERS_MAX; ++i) {
        expect(irq_sharing_register(g_lines, LINE, 10 + i, 0x100 + i, &OPS) == 0,
               "register within capacity");
    }
    expect(irq_sharing_register(g_lines, LINE, 99, 0x999, &OPS) == WASMOS_ERR_IRQ_LINE_FULL,
           "register beyond IRQ_SHARERS_MAX returns LINE_FULL");
}

/* The core rule: the line must stay masked until the LAST sharer acks. Reopening
 * on the first ack re-fires the still-asserted line. */
static void test_line_reopens_only_after_last_ack(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    (void)irq_sharing_register(g_lines, LINE, 20, 0x200, &OPS);
    g_unmask_count = 0;

    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(g_mask_count >= 1, "dispatch masks the line");

    expect(irq_sharing_ack(g_lines, LINE, 10, &OPS) == 0, "first ack accepted");
    expect(unmasks_of(LINE) == 0, "line still masked after first of two acks");

    expect(irq_sharing_ack(g_lines, LINE, 20, &OPS) == 0, "second ack accepted");
    expect(unmasks_of(LINE) == 1, "line reopens after the last ack");
}

static void test_ack_from_non_sharer_and_duplicate(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(irq_sharing_ack(g_lines, LINE, 77, &OPS) == WASMOS_ERR_IRQ_NOT_A_SHARER,
           "ack from non-sharer returns NOT_A_SHARER");
    expect(irq_sharing_ack(g_lines, LINE, 10, &OPS) == 0, "ack accepted");
    g_unmask_count = 0;
    /* Drivers may ack defensively; a second ack must be a successful no-op and
     * must not reopen the line again. */
    expect(irq_sharing_ack(g_lines, LINE, 10, &OPS) == 0, "duplicate ack is a no-op");
    expect(unmasks_of(LINE) == 0, "duplicate ack does not unmask again");
}

/* An unreachable sharer (full endpoint queue) must not make the line wait for an
 * ack that will never arrive. */
static void test_failed_delivery_not_counted(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    (void)irq_sharing_register(g_lines, LINE, 20, 0x200, &OPS);
    g_deliver_fail_endpoint = 0x200;
    g_unmask_count = 0;

    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(irq_sharing_ack(g_lines, LINE, 10, &OPS) == 0, "reachable sharer acks");
    expect(unmasks_of(LINE) == 1, "line reopens without the unreachable sharer");
}

/* If nobody could be reached at all the line must be reopened immediately rather
 * than left masked forever. */
static void test_no_reachable_sharer_reopens_line(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    g_deliver_fail_endpoint = 0x100;
    g_unmask_count = 0;
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(unmasks_of(LINE) == 1, "line reopened when no delivery succeeded");
}

/* One wedged driver must not disable a shared device for its co-sharers. */
static void test_ack_deadline_force_completes(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    (void)irq_sharing_register(g_lines, LINE, 20, 0x200, &OPS);
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    (void)irq_sharing_ack(g_lines, LINE, 10, &OPS); /* sharer 20 never acks */
    g_unmask_count = 0;

    g_now += IRQ_ACK_DEADLINE_TICKS - 1;
    irq_sharing_tick(g_lines, TEST_LINES, &OPS);
    expect(unmasks_of(LINE) == 0, "line still masked before the deadline");

    g_now += 2;
    irq_sharing_tick(g_lines, TEST_LINES, &OPS);
    expect(unmasks_of(LINE) == 1, "deadline force-completes the missing ack");

    /* The forced completion must leave the line usable, and a late ack from the
     * wedged sharer must not reopen it a second time. */
    g_unmask_count = 0;
    expect(irq_sharing_ack(g_lines, LINE, 20, &OPS) == 0, "late ack tolerated");
    expect(unmasks_of(LINE) == 0, "late ack does not double-unmask");
}

/* A device asserting a line no sharer clears re-fires on every unmask. The
 * budget bounds it instead of letting it livelock the machine. */
static void test_dispatch_budget_throttles_and_recovers(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);

    /* Ack each dispatch immediately, as a driver that finds nothing to do would. */
    for (uint32_t i = 0; i < IRQ_DISPATCH_BUDGET_PER_TICK; ++i) {
        irq_sharing_dispatch(g_lines, LINE, &OPS);
        (void)irq_sharing_ack(g_lines, LINE, 10, &OPS);
    }
    expect(g_throttle_log_count == 0, "no throttle within budget");
    uint32_t delivered_before = g_deliver_count;

    irq_sharing_dispatch(g_lines, LINE, &OPS); /* one over budget */
    expect(g_throttle_log_count == 1, "throttle logged once past budget");
    expect(g_throttle_logged_line == LINE, "throttle names the offending line");
    expect(g_deliver_count == delivered_before, "throttled dispatch delivers nothing");

    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(g_throttle_log_count == 1, "throttle diagnostic is not repeated");

    /* The next tick restores the budget and reopens the line. */
    g_now++;
    g_unmask_count = 0;
    irq_sharing_tick(g_lines, TEST_LINES, &OPS);
    expect(unmasks_of(LINE) == 1, "tick reopens the throttled line");
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(g_deliver_count == delivered_before + 1, "dispatch resumes after the tick");
}

/* A departing or reaped sharer must forgive the ack it owed, else the line stays
 * masked for the survivors. */
static void test_unregister_forgives_owed_ack(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    (void)irq_sharing_register(g_lines, LINE, 20, 0x200, &OPS);
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    (void)irq_sharing_ack(g_lines, LINE, 10, &OPS);
    g_unmask_count = 0;

    expect(irq_sharing_unregister(g_lines, LINE, 20, &OPS) == 0, "unregister ok");
    expect(unmasks_of(LINE) == 1, "line reopens once the owed ack is forgiven");
    expect(irq_sharing_has_sharers(g_lines, LINE) == 1, "surviving sharer kept");
}

static void test_last_sharer_leaving_masks_line(void) {
    reset();
    (void)irq_sharing_register(g_lines, LINE, 10, 0x100, &OPS);
    g_mask_count = 0;
    expect(irq_sharing_unregister(g_lines, LINE, 10, &OPS) == 0, "unregister ok");
    expect(g_mask_count == 1, "line masked when the last sharer leaves");
    expect(irq_sharing_has_sharers(g_lines, LINE) == 0, "line has no sharers");
    expect(irq_sharing_unregister(g_lines, LINE, 10, &OPS) == WASMOS_ERR_IRQ_NOT_A_SHARER,
           "second unregister returns NOT_A_SHARER");
}

/* A reaped driver must not leave a slot pointing at a dead endpoint: any ack it
 * still owed would mask the line forever. */
static void test_release_context_clears_all_lines(void) {
    reset();
    (void)irq_sharing_register(g_lines, 4, 10, 0x100, &OPS);
    (void)irq_sharing_register(g_lines, LINE, 10, 0x101, &OPS);
    (void)irq_sharing_register(g_lines, LINE, 20, 0x200, &OPS);
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    g_unmask_count = 0;

    irq_sharing_release_context(g_lines, TEST_LINES, 10, &OPS);
    expect(irq_sharing_has_sharers(g_lines, 4) == 0, "context dropped from line 4");
    expect(irq_sharing_has_sharers(g_lines, LINE) == 1, "co-sharer on line 11 survives");
    /* Sharer 20 still owes its ack, so the line must not have reopened yet. */
    expect(unmasks_of(LINE) == 0, "line stays masked while the survivor owes an ack");
    expect(irq_sharing_ack(g_lines, LINE, 20, &OPS) == 0, "survivor acks");
    expect(unmasks_of(LINE) == 1, "line reopens after the survivor acks");
}

/* The timer line drives scheduler accounting and is never masked. */
static void test_timer_line_never_masked(void) {
    reset();
    (void)irq_sharing_register(g_lines, TIMER_LINE, 10, 0x100, &OPS);
    g_mask_count = 0;
    g_unmask_count = 0;
    irq_sharing_dispatch(g_lines, TIMER_LINE, &OPS);
    expect(g_mask_count == 0, "timer line is not masked on dispatch");
    (void)irq_sharing_ack(g_lines, TIMER_LINE, 10, &OPS);
    expect(unmasks_of(TIMER_LINE) == 0, "timer line is not unmasked either");
}

static void test_dispatch_without_sharers_is_inert(void) {
    reset();
    irq_sharing_dispatch(g_lines, LINE, &OPS);
    expect(g_mask_count == 0 && g_deliver_count == 0, "unrouted line is not touched");
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_register_adds_not_replaces),
        WASMOS_TEST_CASE(test_reregister_same_context_updates),
        WASMOS_TEST_CASE(test_register_full_line_rejected),
        WASMOS_TEST_CASE(test_line_reopens_only_after_last_ack),
        WASMOS_TEST_CASE(test_ack_from_non_sharer_and_duplicate),
        WASMOS_TEST_CASE(test_failed_delivery_not_counted),
        WASMOS_TEST_CASE(test_no_reachable_sharer_reopens_line),
        WASMOS_TEST_CASE(test_ack_deadline_force_completes),
        WASMOS_TEST_CASE(test_dispatch_budget_throttles_and_recovers),
        WASMOS_TEST_CASE(test_unregister_forgives_owed_ack),
        WASMOS_TEST_CASE(test_last_sharer_leaving_masks_line),
        WASMOS_TEST_CASE(test_release_context_clears_all_lines),
        WASMOS_TEST_CASE(test_timer_line_never_masked),
        WASMOS_TEST_CASE(test_dispatch_without_sharers_is_inert),
    };
    const uint64_t seed = wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_irq_sharing: %d passed, %d failed\n", g_checks - g_failures, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
