/* test_libc_startup_argv.c - argv construction from the spawn-info args blob.
 *
 * The startup contract hands a process ONE NUL-terminated argument string, not an
 * argv array (see wasmos_spawn_info.h), so every utility that wanted tokens
 * tokenized the string itself. wasmos_startup_argv is the shared tokenizer, and
 * crt1 builds main()'s argc/argv from it.
 *
 * What is pinned here is the part callers cannot see when it goes wrong: argv[0]
 * is reserved for the program name so that argv[1] is the FIRST ARGUMENT, as in
 * any C program. A tokenizer that packs the first argument into argv[0] instead
 * would satisfy a naive test and silently shift every argument for real code.
 * The bounds cases matter for the same reason -- argv_max and the buffer cap are
 * both attacker-reachable through a long command line.
 *
 * Compiled against the real spawn_info.c on the host, with the two hostcalls it
 * uses stubbed to serve a synthetic spawn-info blob.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_shuffle.h"
#include "wasmos/startup.h"
#include "wasmos_spawn_info.h"

/* Test-only seam in spawn_info.c: drops the one-shot cache so a second blob can
 * be served. Declared here rather than in wasmos/startup.h because nothing in a
 * running process may re-read its startup contract, which is fixed at spawn. */
void wasmos_startup_reset_for_test(void);

/* --- the synthetic spawn-info buffer the stubs serve --------------------- */

#define STUB_BUF_CAP 512

static uint8_t g_stub_buf[STUB_BUF_CAP];
static uint32_t g_stub_len;
static int32_t g_stub_buffer_id = 1;

/* Build a spawn-info blob whose args blob is `args`, and reset the accessors'
 * one-shot cache so the next read observes it. */
static void stub_set_args(const char* args) {
    wasmos_spawn_info_t hdr;
    size_t args_len = strlen(args);

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = WASMOS_SPAWN_INFO_MAGIC;
    hdr.version = WASMOS_SPAWN_INFO_VERSION;
    hdr.header_size = (uint32_t)sizeof(hdr);
    hdr.args_off = (uint32_t)sizeof(hdr);
    hdr.args_len = (uint32_t)args_len;

    memset(g_stub_buf, 0, sizeof(g_stub_buf));
    memcpy(g_stub_buf, &hdr, sizeof(hdr));
    memcpy(g_stub_buf + sizeof(hdr), args, args_len + 1u);
    g_stub_len = (uint32_t)(sizeof(hdr) + args_len + 1u);

    wasmos_startup_reset_for_test();
}

int32_t wasmos_spawn_info_buffer(void) {
    return g_stub_buffer_id;
}

int32_t wasmos_xfer_buffer_read(int32_t buffer_id, void* dst, int32_t len, int32_t offset) {
    if (buffer_id != g_stub_buffer_id || !dst || len < 0 || offset < 0) {
        return -1;
    }
    if ((uint32_t)offset + (uint32_t)len > g_stub_len) {
        return -1;
    }
    memcpy(dst, g_stub_buf + offset, (size_t)len);
    return 0;
}

/* --- the tokenizer ------------------------------------------------------- */

/* argv[0] is the program-name slot and argv[1] is the first argument. The
 * command name is not in the args blob (the CLI passes only what followed it),
 * so the slot is an empty string rather than a name -- but it is still a slot,
 * because shifting arguments down by one is the failure this pins. */
static int test_argv_reserves_slot_zero(void) {
    char buf[64];
    char* argv[8];
    int argc;

    stub_set_args("alpha beta");
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 8);
    if (argc != 3) {
        return __LINE__;
    }
    if (argv[0] == NULL || argv[0][0] != '\0') {
        return __LINE__;
    }
    if (strcmp(argv[1], "alpha") != 0 || strcmp(argv[2], "beta") != 0) {
        return __LINE__;
    }
    if (argv[3] != NULL) {
        return __LINE__;
    }
    return 0;
}

/* An empty argument string still yields a usable argv: argc 1, argv[0] the
 * program slot, argv[1] the NULL terminator. */
static int test_argv_no_arguments(void) {
    char buf[64];
    char* argv[4];
    int argc;

    stub_set_args("");
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 4);
    if (argc != 1) {
        return __LINE__;
    }
    if (argv[0] == NULL || argv[0][0] != '\0' || argv[1] != NULL) {
        return __LINE__;
    }
    return 0;
}

/* Runs of spaces and tabs separate tokens and never produce an empty one, and
 * leading and trailing whitespace is not a token of its own. */
static int test_argv_collapses_whitespace(void) {
    char buf[64];
    char* argv[8];
    int argc;

    stub_set_args("  one\t\t two   three \t ");
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 8);
    if (argc != 4) {
        return __LINE__;
    }
    if (strcmp(argv[1], "one") != 0 || strcmp(argv[2], "two") != 0 ||
        strcmp(argv[3], "three") != 0) {
        return __LINE__;
    }
    return 0;
}

/* argv_max bounds the array including both the program slot and the NULL
 * terminator, so a longer command line is truncated rather than written past the
 * caller's array. */
static int test_argv_respects_argv_max(void) {
    char buf[64];
    char* argv[4];
    int argc;

    argv[3] = (char*)0x1;
    stub_set_args("a b c d e");
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 4);
    if (argc != 3) {
        return __LINE__;
    }
    if (strcmp(argv[1], "a") != 0 || strcmp(argv[2], "b") != 0) {
        return __LINE__;
    }
    if (argv[3] != NULL) {
        return __LINE__;
    }
    return 0;
}

/* The token buffer is a hard bound too, and no token may run off the end of it. */
static int test_argv_respects_buffer_cap(void) {
    char buf[8];
    char* argv[8];
    int argc;
    int i;

    stub_set_args("aaa bbb ccc ddd");
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 8);
    if (argc < 1) {
        return __LINE__;
    }
    for (i = 1; i < argc; ++i) {
        if (argv[i] < buf || argv[i] + strlen(argv[i]) >= buf + sizeof(buf)) {
            return __LINE__;
        }
    }
    if (argv[argc] != NULL) {
        return __LINE__;
    }
    return 0;
}

/* An argument the buffer cannot hold whole is dropped, not handed over as a
 * fragment. "aaa bbbbbb" into 8 bytes copies "aaa bbb", and passing a program a
 * silently shortened argument -- a path, a hostname, a number -- is worse than
 * passing it one fewer argument, because nothing downstream can detect it. */
static int test_argv_drops_a_cut_argument(void) {
    char buf[8];
    char* argv[8];
    int argc;

    stub_set_args("aaa bbbbbb");
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 8);
    if (argc != 2) {
        return __LINE__;
    }
    if (strcmp(argv[1], "aaa") != 0 || argv[2] != NULL) {
        return __LINE__;
    }
    return 0;
}

/* The complement: a token that ends exactly at the cut is complete and is kept.
 * "aaa bbb ccc" into 8 bytes copies "aaa bbb", and the source byte that follows
 * is a space, so bbb was not cut. */
static int test_argv_keeps_an_exactly_fitting_argument(void) {
    char buf[8];
    char* argv[8];
    int argc;

    stub_set_args("aaa bbb ccc");
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 8);
    if (argc != 3) {
        return __LINE__;
    }
    if (strcmp(argv[1], "aaa") != 0 || strcmp(argv[2], "bbb") != 0 || argv[3] != NULL) {
        return __LINE__;
    }
    return 0;
}

/* Degenerate arguments are answered, not dereferenced: a caller with no room for
 * even the program slot and NULL gets argc 0. */
static int test_argv_rejects_unusable_arrays(void) {
    char buf[64];
    char* argv[4];

    stub_set_args("x y");
    if (wasmos_startup_argv(NULL, sizeof(buf), argv, 4) != 0) {
        return __LINE__;
    }
    if (wasmos_startup_argv(buf, sizeof(buf), NULL, 4) != 0) {
        return __LINE__;
    }
    if (wasmos_startup_argv(buf, sizeof(buf), argv, 1) != 0) {
        return __LINE__;
    }
    if (wasmos_startup_argv(buf, 0, argv, 4) != 0) {
        return __LINE__;
    }
    return 0;
}

/* wasmos_startup_args stays byte-for-byte what it was: the tokenizer must not
 * consume or rewrite the blob the existing utilities read. */
static int test_args_string_still_intact(void) {
    char buf[64];
    char* argv[8];
    char raw[64];

    stub_set_args("one two");
    (void)wasmos_startup_argv(buf, sizeof(buf), argv, 8);
    if (wasmos_startup_args(raw, sizeof(raw)) != 7u) {
        return __LINE__;
    }
    if (strcmp(raw, "one two") != 0) {
        return __LINE__;
    }
    return 0;
}

/* The argument string is cut in two independent places -- the load's 255-byte cap
 * into libc's own buffer, and the caller's buf_cap -- and drop-whole has to hold
 * for both. Here the caller's buffer is generous and the LOAD is what cuts: 260
 * bytes of arguments, so the last token arrives as a fragment and must be
 * dropped. Getting this wrong is invisible in the buf_cap tests above, because
 * there the two caps coincide.
 *
 * The blob is "aaa.. bbb.." with the second token running past the cap, so the
 * surviving argv is the first token alone. */
static int test_argv_drops_a_token_cut_by_the_load_cap(void) {
    char args[300];
    char buf[300];
    char* argv[8];
    int argc;
    int i;

    for (i = 0; i < 100; ++i) {
        args[i] = 'a';
    }
    args[100] = ' ';
    for (i = 101; i < 260; ++i) {
        args[i] = 'b';
    }
    args[260] = '\0';

    stub_set_args(args);
    argc = wasmos_startup_argv(buf, sizeof(buf), argv, 8);
    if (argc != 2) {
        return __LINE__;
    }
    if (strlen(argv[1]) != 100u || argv[1][0] != 'a' || argv[2] != NULL) {
        return __LINE__;
    }
    return 0;
}

static const wasmos_test_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_argv_reserves_slot_zero),
    WASMOS_TEST_CASE(test_argv_no_arguments),
    WASMOS_TEST_CASE(test_argv_collapses_whitespace),
    WASMOS_TEST_CASE(test_argv_respects_argv_max),
    WASMOS_TEST_CASE(test_argv_respects_buffer_cap),
    WASMOS_TEST_CASE(test_argv_drops_a_cut_argument),
    WASMOS_TEST_CASE(test_argv_keeps_an_exactly_fitting_argument),
    WASMOS_TEST_CASE(test_argv_drops_a_token_cut_by_the_load_cap),
    WASMOS_TEST_CASE(test_argv_rejects_unusable_arrays),
    WASMOS_TEST_CASE(test_args_string_still_intact),
};

int main(void) {
    return wasmos_test_run_all(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));
}
