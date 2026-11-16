/* test_kmsg.c — the kernel log ring (src/kernel/kmsg.c).
 *
 * dmesg pages the ring with kmsg_read while the kernel keeps appending. The
 * contract that makes that safe is the sequence number: a reader resumes where
 * it stopped, and when the bytes it wanted were overwritten it is moved to the
 * oldest retained byte and can see by how much. These cases pin that, the
 * offset-from-oldest copy the framebuffer and COM2 replays use, and that the
 * ring stores text exactly as written (LF line ends, no CR). The subscriber
 * protocol is pinned too, against a fake kernel: one subscription per context,
 * a full table of live contexts reported by its own code, appends coalesced
 * into one notify, exited subscribers dropped (also when the table is full), a
 * notify that could not be queued retried, and liveness checks and sends made
 * with the lock released.
 *
 * src/kernel/kmsg.c is the only source linked in; it has no lock or allocator
 * behind it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "kmsg.h"
#include "wasmos_status.h"

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

static kmsg_t g_k;

/* Each case starts from an empty ring; the cases run in a shuffled order. */
static void reset(void) {
    memset(&g_k, 0, sizeof(g_k));
}

/* Appends `n` bytes cycling through 'a'..'z', newline-free. */
static void fill(uint32_t n) {
    char chunk[257];
    uint32_t done = 0;
    while (done < n) {
        uint32_t c = n - done < 256 ? n - done : 256;
        for (uint32_t i = 0; i < c; ++i) {
            chunk[i] = (char)('a' + (done + i) % 26);
        }
        chunk[c] = '\0';
        kmsg_append(&g_k, chunk);
        done += c;
    }
}

static void test_text_is_stored_as_written(void) {
    uint8_t out[32];
    uint64_t seq = 0;

    reset();
    kmsg_append(&g_k, "[kernel] one\n");
    kmsg_append(&g_k, "two\n");
    CHECK(kmsg_size(&g_k) == 17, "size counts the bytes written, no CR inserted");
    int32_t n = kmsg_read(&g_k, &seq, out, sizeof(out));
    CHECK(n == 17, "read returns everything retained");
    CHECK(seq == 0, "from sequence 0");
    CHECK(memcmp(out, "[kernel] one\ntwo\n", 17) == 0, "bytes round-trip");
}

static void test_reader_resumes_where_it_stopped(void) {
    uint8_t out[8];
    uint64_t seq = 0;

    reset();
    kmsg_append(&g_k, "abcdefghij");
    CHECK(kmsg_read(&g_k, &seq, out, 4) == 4, "first page");
    CHECK(seq == 0 && memcmp(out, "abcd", 4) == 0, "first page content");
    seq += 4;
    CHECK(kmsg_read(&g_k, &seq, out, 8) == 6, "second page is the rest");
    CHECK(seq == 4 && memcmp(out, "efghij", 6) == 0, "second page content");
    seq += 6;
    CHECK(kmsg_read(&g_k, &seq, out, 8) == 0, "at the end nothing is copied");
    CHECK(seq == 10, "and the position is kept");
    kmsg_append(&g_k, "kl");
    CHECK(kmsg_read(&g_k, &seq, out, 8) == 2, "a later append is picked up");
    CHECK(memcmp(out, "kl", 2) == 0, "appended content");
}

static void test_overwritten_reader_learns_what_it_lost(void) {
    uint8_t out[4];
    uint64_t seq = 10;

    reset();
    fill(KMSG_SIZE + 100);
    CHECK(kmsg_size(&g_k) == KMSG_SIZE, "size saturates at the ring size");
    CHECK(kmsg_first(&g_k) == 100, "the oldest retained byte is 100");
    CHECK(kmsg_read(&g_k, &seq, out, sizeof(out)) == 4, "a stale reader still reads");
    CHECK(seq == 100, "moved to the oldest retained byte: 90 bytes lost");
    CHECK(out[0] == (uint8_t)('a' + 100 % 26), "and it reads the byte at that sequence");
}

static void test_read_across_the_wrap_is_contiguous(void) {
    uint8_t out[16];
    uint64_t seq = KMSG_SIZE - 4;

    reset();
    fill(KMSG_SIZE + 8);
    CHECK(kmsg_read(&g_k, &seq, out, sizeof(out)) == 12, "the last 12 bytes");
    CHECK(seq == KMSG_SIZE - 4, "start is retained, so it is kept");
    for (uint32_t i = 0; i < 12; ++i) {
        CHECK(out[i] == (uint8_t)('a' + (KMSG_SIZE - 4 + i) % 26), "byte across the wrap");
    }
}

static void test_copy_offset_counts_from_the_oldest(void) {
    uint8_t out[4];

    reset();
    fill(KMSG_SIZE + 30);
    kmsg_copy(&g_k, out, 0, 2);
    CHECK(out[0] == (uint8_t)('a' + 30 % 26), "offset 0 is the oldest retained byte");
    memset(out, 0, sizeof(out));
    kmsg_copy(&g_k, out, KMSG_SIZE - 1, 4);
    CHECK(out[0] == (uint8_t)('a' + (KMSG_SIZE + 29) % 26), "the newest byte");
    CHECK(out[1] == 0, "len is clamped to what is retained");
    kmsg_copy(&g_k, out, KMSG_SIZE, 1);
    CHECK(out[1] == 0, "an offset past the end copies nothing");
}

static void test_future_sequence_is_refused(void) {
    uint8_t out[4];
    uint64_t seq = 6;

    reset();
    kmsg_append(&g_k, "abc");
    CHECK(kmsg_read(&g_k, &seq, out, sizeof(out)) == WASMOS_INVAL,
          "a sequence the log never reached is invalid");
    CHECK(kmsg_read(&g_k, NULL, out, sizeof(out)) == WASMOS_INVAL, "NULL seq");
}

static kmsg_notify_t g_n;

/* A fake kernel: a lock that must be held around table access and never across
 * a callback, a set of (context, endpoint) pairs still alive, a per-endpoint
 * queue that can be full, and a log of what was sent. */
typedef struct {
    int locked;
    int callback_under_lock;
    uint32_t dead_context[8];
    uint32_t dead_count;
    uint32_t full_endpoint; /* sends to this endpoint fail */
    uint32_t sent[32];
    uint32_t sent_count;
} fake_kernel_t;

static fake_kernel_t g_k2;

static void fake_lock(void* user) {
    ((fake_kernel_t*)user)->locked = 1;
}

static void fake_unlock(void* user) {
    ((fake_kernel_t*)user)->locked = 0;
}

static int fake_alive(void* user, uint32_t context_id, uint32_t endpoint) {
    fake_kernel_t* k = (fake_kernel_t*)user;
    (void)endpoint;
    if (k->locked) {
        k->callback_under_lock = 1;
    }
    for (uint32_t i = 0; i < k->dead_count; ++i) {
        if (k->dead_context[i] == context_id) {
            return 0;
        }
    }
    return 1;
}

static int fake_send(void* user, uint32_t endpoint) {
    fake_kernel_t* k = (fake_kernel_t*)user;
    if (k->locked) {
        k->callback_under_lock = 1;
    }
    if (endpoint == k->full_endpoint) {
        return 1;
    }
    if (k->sent_count < 32) {
        k->sent[k->sent_count++] = endpoint;
    }
    return 0;
}

static const kmsg_notify_env_t g_env = {fake_lock, fake_unlock, fake_alive, fake_send, &g_k2};

static void reset_notify(void) {
    memset(&g_n, 0, sizeof(g_n));
    memset(&g_k2, 0, sizeof(g_k2));
}

static uint32_t sends_to(uint32_t endpoint) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_k2.sent_count; ++i) {
        count += g_k2.sent[i] == endpoint;
    }
    return count;
}

static void test_a_context_holds_one_subscription(void) {
    reset_notify();
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 7, 100) == 0, "subscribe");
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 7, 101) == 0, "subscribing again succeeds");
    kmsg_notify_mark(&g_n);
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(g_k2.sent_count == 1 && g_k2.sent[0] == 101,
          "it replaced the endpoint rather than adding one");
}

static void test_a_full_table_of_live_contexts_is_reported(void) {
    reset_notify();
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        CHECK(kmsg_notify_subscribe(&g_n, &g_env, 10 + i, 200 + i) == 0, "fill");
    }
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 99, 299) == WASMOS_ERR_KERNEL_NO_SUBSCRIBER_SLOT,
          "a further context is refused with its own code");
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 10, 250) == 0,
          "a subscribed context still re-subscribes");
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 0, 1) == WASMOS_INVAL, "context 0 is invalid");
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 1, 0) == WASMOS_INVAL, "endpoint 0 is invalid");
}

static void test_appends_coalesce_into_one_notify(void) {
    reset_notify();
    kmsg_notify_mark(&g_n);
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(g_k2.sent_count == 0, "an append with no subscriber owes nothing");
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 3, 30) == 0, "subscribe");
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(g_k2.sent_count == 0, "nothing is owed before an append");
    kmsg_notify_mark(&g_n);
    kmsg_notify_mark(&g_n);
    kmsg_notify_mark(&g_n);
    kmsg_notify_deliver(&g_n, &g_env);
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(sends_to(30) == 1, "three appends and two deliveries make one notify");
}

static void test_delivery_drops_a_subscriber_that_exited(void) {
    reset_notify();
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 5, 50) == 0, "subscribe");
    g_k2.dead_context[g_k2.dead_count++] = 5;
    kmsg_notify_mark(&g_n);
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(sends_to(50) == 0, "nothing is sent to an exited subscriber");
    g_k2.dead_count = 0;
    kmsg_notify_mark(&g_n);
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(sends_to(50) == 0, "and its subscription ended");
    CHECK(!g_k2.callback_under_lock, "liveness and sends ran with the lock released");
}

/* Regression: 2026-11-16-kmsg-dead-subscribers-hold-slots -- subscriptions were
 * pruned only while delivering, and delivery only happens after an append. Four
 * subscribers that exited with nothing logged since kept every slot, and the next
 * process to subscribe -- a restarted logger -- was refused with
 * NO_SUBSCRIBER_SLOT although no subscriber was alive. */
static void test_exited_subscribers_do_not_hold_their_slots(void) {
    reset_notify();
    for (uint32_t i = 0; i < KMSG_SUBSCRIBER_MAX; ++i) {
        CHECK(kmsg_notify_subscribe(&g_n, &g_env, 10 + i, 200 + i) == 0, "fill");
        g_k2.dead_context[g_k2.dead_count++] = 10 + i;
    }
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 99, 299) == 0,
          "with every subscriber gone, a new one takes a slot without waiting for an append");
    kmsg_notify_mark(&g_n);
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(sends_to(299) == 1, "and is notified");
    CHECK(!g_k2.callback_under_lock, "liveness ran with the lock released");
}

/* Regression: 2026-11-16-kmsg-notify-lost-on-full-queue -- the pending flag was
 * cleared before the send and a failed send was dropped, so a subscriber whose
 * queue was full when the notify went out was never woken for that append: its
 * idle task does not wake on unrelated messages, and without a further append
 * the bytes stayed unread. */
static void test_a_notify_that_could_not_be_queued_is_retried(void) {
    reset_notify();
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 1, 11) == 0, "subscribe a");
    CHECK(kmsg_notify_subscribe(&g_n, &g_env, 2, 22) == 0, "subscribe b");
    g_k2.full_endpoint = 11;
    kmsg_notify_mark(&g_n);
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(sends_to(11) == 0 && sends_to(22) == 1, "a's queue was full, b was notified");
    g_k2.full_endpoint = 0;
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(sends_to(11) == 1, "the next delivery retries a without a further append");
    CHECK(sends_to(22) == 1, "and does not notify b a second time");
    kmsg_notify_deliver(&g_n, &g_env);
    CHECK(sends_to(11) == 1, "once queued, it is not sent again");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_text_is_stored_as_written),
    WASMOS_TEST_CASE(test_reader_resumes_where_it_stopped),
    WASMOS_TEST_CASE(test_overwritten_reader_learns_what_it_lost),
    WASMOS_TEST_CASE(test_read_across_the_wrap_is_contiguous),
    WASMOS_TEST_CASE(test_copy_offset_counts_from_the_oldest),
    WASMOS_TEST_CASE(test_future_sequence_is_refused),
    WASMOS_TEST_CASE(test_a_context_holds_one_subscription),
    WASMOS_TEST_CASE(test_a_full_table_of_live_contexts_is_reported),
    WASMOS_TEST_CASE(test_appends_coalesce_into_one_notify),
    WASMOS_TEST_CASE(test_delivery_drops_a_subscriber_that_exited),
    WASMOS_TEST_CASE(test_exited_subscribers_do_not_hold_their_slots),
    WASMOS_TEST_CASE(test_a_notify_that_could_not_be_queued_is_retried),
};

int main(void) {
    printf("test_kmsg\n");

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_kmsg: FAIL\n");
        return 1;
    }
    printf("test_kmsg: OK\n");
    return 0;
}
