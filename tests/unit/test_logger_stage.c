/* test_logger_stage.c - what the logger service writes next
 * (src/services/logger/logger_stage.c).
 *
 * The logger appends the kernel log to /var/log/messages and puts a marker line
 * in the file for anything it cannot copy: bytes the kernel ring overwrote
 * before they were read, and the file's own truncation at its size cap. Both
 * markers have to reach the file, and a write the filesystem completed only in
 * part must not count as a write of the whole chunk.
 *
 * Only logger_stage.c is linked; the kernel ring is a scripted reader.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "logger_stage.h"

static int g_failures;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, (what));                              \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* A kernel ring holding the bytes [first, next), each byte equal to its
 * sequence number mod 26 as a letter. */
typedef struct {
    uint64_t first;
    uint64_t next;
} ring_t;

static int32_t ring_read(void* user, uint8_t* dst, int32_t len, uint64_t* seq) {
    ring_t* r = (ring_t*)user;
    if (*seq > r->next) {
        return -1; /* kmsg_read: a position the log never reached */
    }
    uint64_t start = *seq < r->first ? r->first : *seq;
    uint64_t avail = r->next - start;
    int32_t n = avail < (uint64_t)len ? (int32_t)avail : len;
    for (int32_t i = 0; i < n; ++i) {
        dst[i] = (uint8_t)('a' + (start + (uint64_t)i) % 26u);
    }
    *seq = start;
    return n;
}

static int staged_contains(const logger_stage_t* st, const char* text) {
    size_t len = strlen(text);
    for (uint32_t i = 0; i + len <= st->out_len; ++i) {
        if (memcmp(st->out + i, text, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_a_fresh_boot_is_staged_whole(void) {
    logger_stage_t st;
    ring_t ring = {0, 100};
    memset(&st, 0, sizeof(st));
    CHECK(logger_stage_next(&st, ring_read, &ring) == 100, "the whole ring is staged");
    CHECK(st.out[0] == 'a' && st.seq == 100, "from sequence 0, with the position advanced");
    CHECK(logger_stage_next(&st, ring_read, &ring) == 0, "then nothing");
}

static void test_bytes_lost_before_reading_are_reported(void) {
    logger_stage_t st;
    ring_t ring = {500, 600};
    memset(&st, 0, sizeof(st));
    st.seq = 400;
    CHECK(logger_stage_next(&st, ring_read, &ring) > 100, "the chunk and a marker are staged");
    CHECK(staged_contains(&st, "[logger] 100 bytes of kernel log lost before they were read\n"),
          "the loss is reported in the file");
}

/* Regression: 2026-11-16-logger-truncation-notice-replaced -- a truncation
 * notice waited in the marker slot while the file was closed and reopened, and
 * the next ring read that found bytes overwritten in the meantime wrote its loss
 * notice into the same slot. The new file then never said its earlier contents
 * had been truncated deliberately. */
static void test_a_truncation_notice_survives_a_loss_during_the_reopen(void) {
    logger_stage_t st;
    ring_t ring = {500, 600};
    int saw_truncated = 0;
    int saw_lost = 0;
    memset(&st, 0, sizeof(st));
    st.seq = 400;
    logger_stage_truncated(&st, "/var/log/messages", 1048576u);
    for (int i = 0; i < 4 && logger_stage_next(&st, ring_read, &ring) > 0; ++i) {
        saw_truncated |= staged_contains(&st, "was truncated\n");
        saw_lost |= staged_contains(&st, "lost before they were read\n");
    }
    CHECK(saw_truncated, "the truncation notice reaches the file");
    CHECK(saw_lost, "and so does the loss notice");
}

/* Regression: 2026-11-16-logger-short-write-accepted -- any non-negative FS
 * completion was taken as a write of the whole chunk. tmpfs completes a write
 * short when its block pool runs out, and the ring had already been read past
 * the chunk, so the unwritten suffix vanished from the file without a trace. */
static void test_a_short_write_is_not_a_complete_one(void) {
    CHECK(logger_stage_written(100, 100), "a complete write");
    CHECK(!logger_stage_written(100, 40), "a short write is not complete");
    CHECK(!logger_stage_written(100, -1), "nor is a failed one");
}

/* Regression: 2026-11-20-logger-restart-duplicates -- a restarted logger read
 * the kernel ring from its oldest retained byte again and appended it, so a
 * restart under the supervisor put up to the ring's 64 KiB a second time into
 * /var/log/messages. The position survives the restart as a saved string. */
static void test_a_restarted_logger_continues_where_it_stopped(void) {
    logger_stage_t first;
    logger_stage_t second;
    ring_t ring = {0, 100};
    char saved[24];
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(logger_stage_next(&first, ring_read, &ring) == 100, "the first instance wrote 100");
    uint32_t n = logger_stage_save(&first, saved, sizeof(saved));
    CHECK(n == 3 && strcmp(saved, "100") == 0, "its position is saved as text");
    ring.next = 130;
    logger_stage_resume(&second, saved, n);
    CHECK(logger_stage_next(&second, ring_read, &ring) == 30, "the next instance writes the rest");
    CHECK(second.out[0] == (char)('a' + 100 % 26), "from where the first stopped");
}

static void test_a_position_that_does_not_parse_starts_over(void) {
    logger_stage_t st;
    char big[24];
    memset(&st, 0, sizeof(st));
    logger_stage_resume(&st, "12x", 3);
    CHECK(st.seq == 0, "not a number");
    logger_stage_resume(&st, "", 0);
    CHECK(st.seq == 0, "empty");
    logger_stage_resume(&st, "99999999999999999999", 20);
    CHECK(st.seq == 0, "does not fit 64 bits");
    st.seq = 18446744073709551615ull;
    CHECK(logger_stage_save(&st, big, sizeof(big)) == 20, "the largest position fits");
    CHECK(logger_stage_save(&st, big, 20) == 0, "but not without room for the NUL");
}

/* Regression: 2026-11-20-logger-poisoned-position -- kenv is writable by any
 * process, and a stored position past the ring's end made every kernel-log
 * read fail, so each restarted logger exited at once. A position the ring
 * refuses is not trusted: the logger starts over from the oldest byte. */
static void test_a_position_the_ring_refuses_starts_over(void) {
    logger_stage_t st;
    ring_t ring = {0, 50};
    memset(&st, 0, sizeof(st));
    logger_stage_resume(&st, "18446744073709551615", 20);
    CHECK(logger_stage_next(&st, ring_read, &ring) == 50, "the whole ring, not a failure");
    CHECK(st.out[0] == 'a' && st.seq == 50, "from the oldest byte");
}

/* A second follower of the ring (the virtio-serial kernel-log port) stages with
 * the same rules but names itself in its markers. */
static void test_a_tagged_stage_names_its_owner(void) {
    logger_stage_t st;
    ring_t ring = {500, 600};
    memset(&st, 0, sizeof(st));
    st.tag = "[virtio-serial]";
    CHECK(logger_stage_next(&st, ring_read, &ring) > 100, "the chunk and a marker are staged");
    CHECK(staged_contains(&st,
                          "[virtio-serial] 500 bytes of earlier kernel log were not "
                          "retained\n"),
          "the marker carries the stage's tag");
    CHECK(!staged_contains(&st, "[logger]"), "and not the logger's");
}

int main(void) {
    test_a_tagged_stage_names_its_owner();
    test_a_fresh_boot_is_staged_whole();
    test_bytes_lost_before_reading_are_reported();
    test_a_truncation_notice_survives_a_loss_during_the_reopen();
    test_a_short_write_is_not_a_complete_one();
    test_a_restarted_logger_continues_where_it_stopped();
    test_a_position_that_does_not_parse_starts_over();
    test_a_position_the_ring_refuses_starts_over();
    if (g_failures != 0) {
        printf("test_logger_stage: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_logger_stage: ok\n");
    return 0;
}
