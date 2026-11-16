/* test_kmsg.c — the kernel log ring (src/kernel/kmsg.c).
 *
 * dmesg pages the ring with kmsg_read while the kernel keeps appending. The
 * contract that makes that safe is the sequence number: a reader resumes where
 * it stopped, and when the bytes it wanted were overwritten it is moved to the
 * oldest retained byte and can see by how much. These cases pin that, the
 * offset-from-oldest copy the framebuffer and COM2 replays use, and that the
 * ring stores text exactly as written (LF line ends, no CR).
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

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_text_is_stored_as_written),
    WASMOS_TEST_CASE(test_reader_resumes_where_it_stopped),
    WASMOS_TEST_CASE(test_overwritten_reader_learns_what_it_lost),
    WASMOS_TEST_CASE(test_read_across_the_wrap_is_contiguous),
    WASMOS_TEST_CASE(test_copy_offset_counts_from_the_oldest),
    WASMOS_TEST_CASE(test_future_sequence_is_refused),
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
