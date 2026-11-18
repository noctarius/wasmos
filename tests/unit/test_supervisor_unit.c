/* test_supervisor_unit.c - the supervisor's per-unit policy
 * (src/services/supervisor/supervisor_unit.c).
 *
 * A unit spec has to parse into exactly the policy it states and refuse
 * anything else, since a refused submission spawns nothing. Once running, a
 * unit is restarted by its policy with a doubling backoff, gives up after too
 * many ends inside the sliding window, resets its backoff only after a stable
 * run, is killed when it misses its READY deadline, and a critical unit's
 * first end is reported as such instead of restarted. What the supervisor
 * reports of its table over SUPERVISOR_IPC_LIST_REQ is each held unit's own
 * state, in ABI values, and never more records than the caller made room for.
 *
 * Only supervisor_unit.c is linked; time is passed in explicitly.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "supervisor_unit.h"
#include "../../abi/generated/c/wasmos_constants.h"
#include "../../abi/generated/c/wasmos_status.h"

static int g_failures;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, (what));                              \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static const sv_timing_t k_timing = {
    .ready_timeout_ms = 10000,
    .stable_ms = 10000,
    .backoff_min_ms = 250,
    .backoff_max_ms = 30000,
    .intensity_max = 5,
    .intensity_window_ms = 60000,
};

static sv_unit_t parse_ok(const char* spec, uint32_t now) {
    sv_unit_t u;
    memset(&u, 0xAB, sizeof(u));
    CHECK(sv_spec_parse(spec, &k_timing, now, &u) == 0, spec);
    return u;
}

/* Brings a parsed unit to a spawned instance with `pid` at `now`. */
static void start(sv_unit_t* u, uint32_t pid, uint32_t now) {
    CHECK(sv_unit_tick(u, &k_timing, now) == SV_VERDICT_SPAWN, "a due unit is spawned");
    sv_unit_spawn_sent(u);
    sv_unit_spawned(u, &k_timing, pid, now);
}

static void test_a_spec_parses_into_its_policy(void) {
    sv_unit_t u = parse_ok("cli@tty1 /boot/system/services/cli.wap restart=always "
                           "class=required tty=1 -- -x  two words  ",
                           100);
    CHECK(strcmp(u.name, "cli@tty1") == 0, "name");
    CHECK(strcmp(u.path, "/boot/system/services/cli.wap") == 0, "path");
    CHECK(u.restart == SV_RESTART_ALWAYS, "restart");
    CHECK(u.cls == SV_CLASS_REQUIRED, "class");
    CHECK(u.tty == 2u, "tty is slot + 1");
    CHECK(strcmp(u.args, "-x  two words") == 0, "args keep inner spaces, lose the trailing ones");
    CHECK(u.state == SV_UNIT_SPAWN_DUE && u.deadline == 100u, "due at once");
    CHECK(u.backoff_ms == k_timing.backoff_min_ms, "backoff starts at the minimum");
}

static void test_a_bare_spec_takes_the_defaults(void) {
    sv_unit_t u = parse_ok("  logger\t/boot/system/services/logger.wap  ", 0);
    CHECK(strcmp(u.name, "logger") == 0, "name");
    CHECK(u.restart == SV_RESTART_ON_FAILURE, "default restart is on-failure");
    CHECK(u.cls == SV_CLASS_OPTIONAL, "default class is optional");
    CHECK(u.tty == 0u, "no tty pin");
    CHECK(u.args[0] == '\0', "no args");
}

static void test_a_bad_spec_is_refused_untouched(void) {
    static const char* const bad[] = {
        "",
        "   ",
        "onlyname",
        "name relative/path.wap",
        "name /p restart=sometimes",
        "name /p class=vital",
        "name /p tty=x",
        "name /p tty=0", /* the kernel log, not a slot */
        "name /p tty=255",
        "name /p tty=1000",
        "name /p restart",
        "name /p bogus=1",
        "a12345678901234567890123456789012345 /p", /* 36 bytes: one too long */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        sv_unit_t u;
        memset(&u, 0x5A, sizeof(u));
        CHECK(sv_spec_parse(bad[i], &k_timing, 0, &u) == WASMOS_ERR_SUPERVISOR_BAD_SPEC, bad[i]);
        CHECK(((const uint8_t*)&u)[0] == 0x5A, "a refused spec leaves the unit untouched");
    }
}

static void test_a_path_too_long_is_refused(void) {
    char longpath[SV_PATH_MAX + 8];
    sv_unit_t u;
    memcpy(longpath, "n /", 3);
    memset(longpath + 3, 'p', sizeof(longpath) - 4);
    longpath[sizeof(longpath) - 1] = '\0';
    CHECK(sv_spec_parse(longpath, &k_timing, 0, &u) == WASMOS_ERR_SUPERVISOR_BAD_SPEC,
          "a path past SV_PATH_MAX is refused");
}

static void test_the_longest_name_fits(void) {
    sv_unit_t u = parse_ok("a1234567890123456789012345678901234 /p", 0); /* 35 bytes */
    CHECK(strlen(u.name) == SV_UNIT_NAME_MAX - 1u, "35-byte name kept whole");
}

static void test_a_crash_restarts_with_a_doubling_backoff(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    uint32_t now = 0;
    uint32_t expect = 250;
    start(&u, 7, now);
    sv_unit_ready(&u, &k_timing, now);
    for (int i = 0; i < 3; ++i) {
        now += 1000;
        CHECK(sv_unit_exited(&u, &k_timing, -11, PROC_EXIT_REASON_FAULTED, now) ==
                  SV_VERDICT_RESTART,
              "a crash restarts");
        CHECK(u.pid == 0u, "the dead pid is dropped");
        CHECK(u.deadline == now + expect, "the restart waits the current backoff");
        CHECK(sv_unit_tick(&u, &k_timing, now + expect - 1u) == SV_VERDICT_NONE,
              "not before the backoff has passed");
        now += expect;
        start(&u, 8u + (uint32_t)i, now);
        expect *= 2u;
    }
    CHECK(u.restart_count == 3u, "three restarts counted");
    CHECK(u.last_reason == PROC_EXIT_REASON_FAULTED && u.last_status == -11,
          "the last end is kept for the log");
}

static void test_the_backoff_is_capped(void) {
    sv_timing_t t = k_timing;
    sv_unit_t u = parse_ok("svc /p", 0);
    uint32_t now = 0;
    t.intensity_max = 16; /* never give up here */
    t.intensity_window_ms = 1;
    for (int i = 0; i < 12; ++i) {
        CHECK(sv_unit_tick(&u, &t, now) == SV_VERDICT_SPAWN, "spawn");
        sv_unit_spawn_sent(&u);
        sv_unit_spawned(&u, &t, 9, now);
        now += 5;
        CHECK(sv_unit_exited(&u, &t, 1, PROC_EXIT_REASON_EXITED, now) == SV_VERDICT_RESTART,
              "restart");
        now = u.deadline;
    }
    CHECK(u.backoff_ms == t.backoff_max_ms, "backoff stops at its maximum");
}

static void test_the_fifth_end_in_the_window_gives_up(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    uint32_t now = 0;
    for (int i = 0; i < 4; ++i) {
        start(&u, 3, now);
        now += 100;
        CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_TRAPPED, now) == SV_VERDICT_RESTART,
              "the first four ends restart");
        now = u.deadline;
    }
    start(&u, 3, now);
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_TRAPPED, now + 1) == SV_VERDICT_FAILED,
          "the fifth end inside 60 s gives up");
    CHECK(u.state == SV_UNIT_FAILED, "failed");
    CHECK(sv_unit_tick(&u, &k_timing, now + 100000) == SV_VERDICT_NONE, "a failed unit stays down");
    CHECK(sv_unit_deadline(&u, 0) == 0, "and has no deadline");
}

static void test_old_ends_age_out_of_the_window(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    uint32_t now = 0;
    for (int i = 0; i < 10; ++i) {
        start(&u, 3, now);
        now += 20000; /* one end every ~20 s: never five inside 60 s */
        CHECK(sv_unit_exited(&u, &k_timing, 1, PROC_EXIT_REASON_EXITED, now) == SV_VERDICT_RESTART,
              "spaced ends keep restarting");
        now = u.deadline;
    }
}

static void test_a_stable_run_resets_only_the_backoff(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    uint32_t now = 0;
    start(&u, 3, now);
    now += 10;
    CHECK(sv_unit_exited(&u, &k_timing, 1, PROC_EXIT_REASON_EXITED, now) == SV_VERDICT_RESTART,
          "restart");
    now = u.deadline;
    start(&u, 4, now);
    CHECK(u.backoff_ms == 500u, "backoff doubled");
    sv_unit_ready(&u, &k_timing, now);
    CHECK(sv_unit_tick(&u, &k_timing, now + k_timing.stable_ms - 1u) == SV_VERDICT_NONE &&
              u.state == SV_UNIT_RUNNING,
          "not stable before stable_ms");
    now += k_timing.stable_ms;
    CHECK(sv_unit_tick(&u, &k_timing, now) == SV_VERDICT_NONE && u.state == SV_UNIT_STABLE,
          "stable after stable_ms");
    CHECK(u.backoff_ms == k_timing.backoff_min_ms, "a stable run resets the backoff");
    CHECK(u.ends_recorded == 1u, "but not the intensity history");
}

static void test_a_missed_ready_is_killed_and_counted(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    start(&u, 5, 0);
    CHECK(sv_unit_tick(&u, &k_timing, k_timing.ready_timeout_ms - 1u) == SV_VERDICT_NONE,
          "not before the deadline");
    CHECK(sv_unit_tick(&u, &k_timing, k_timing.ready_timeout_ms) == SV_VERDICT_KILL,
          "kill at the deadline");
    CHECK(u.state == SV_UNIT_STOPPING && u.pid == 5u, "stopping, pid still known for the kill");
    CHECK(sv_unit_tick(&u, &k_timing, k_timing.ready_timeout_ms + 1u) == SV_VERDICT_NONE,
          "killed once");
    sv_unit_ready(&u, &k_timing, k_timing.ready_timeout_ms + 1u);
    CHECK(u.state == SV_UNIT_STOPPING, "a READY after the kill changes nothing");
    /* A clean-looking exit of a unit killed for its deadline is still a failure. */
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_EXITED, 10001) == SV_VERDICT_RESTART,
          "the killed unit restarts under on-failure");
    CHECK(u.ends_recorded == 1u, "and the end counts");
}

static void test_a_clean_exit_stops_an_on_failure_unit(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    start(&u, 5, 0);
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_EXITED, 10) == SV_VERDICT_STOPPED,
          "exit 0 is a clean stop");
    CHECK(u.state == SV_UNIT_STOPPED && u.ends_recorded == 0u, "stopped, nothing counted");
}

static void test_always_restarts_a_clean_exit_and_counts_it(void) {
    sv_unit_t u = parse_ok("cli /p restart=always", 0);
    start(&u, 5, 0);
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_EXITED, 10) == SV_VERDICT_RESTART,
          "exit 0 restarts under always");
    CHECK(u.ends_recorded == 1u, "and counts toward the limit");
}

static void test_never_does_not_restart(void) {
    sv_unit_t u = parse_ok("svc /p restart=never", 0);
    start(&u, 5, 0);
    CHECK(sv_unit_exited(&u, &k_timing, 3, PROC_EXIT_REASON_EXITED, 10) == SV_VERDICT_FAILED,
          "a failing never-unit is failed");
    u = parse_ok("svc /p restart=never", 0);
    start(&u, 5, 0);
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_EXITED, 10) == SV_VERDICT_STOPPED,
          "a clean never-unit is stopped");
}

static void test_a_critical_unit_is_never_restarted(void) {
    sv_unit_t u = parse_ok("fsm /p class=critical restart=always", 0);
    start(&u, 5, 0);
    sv_unit_ready(&u, &k_timing, 0);
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_EXITED, 10) == SV_VERDICT_CRITICAL,
          "any end of a critical unit is critical");
    u = parse_ok("fsm /p class=critical", 0);
    CHECK(sv_unit_tick(&u, &k_timing, 0) == SV_VERDICT_SPAWN, "spawn");
    sv_unit_spawn_sent(&u);
    CHECK(sv_unit_spawn_failed(&u, &k_timing, WASMOS_ERR_PROC_SPAWN_FS_READ, 1) ==
              SV_VERDICT_CRITICAL,
          "so is a critical unit that cannot be spawned");
}

static void test_a_refused_spawn_is_a_failure(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    CHECK(sv_unit_tick(&u, &k_timing, 0) == SV_VERDICT_SPAWN, "spawn");
    sv_unit_spawn_sent(&u);
    CHECK(sv_unit_spawn_failed(&u, &k_timing, WASMOS_ERR_PROC_SPAWN_FS_READ, 1) ==
              SV_VERDICT_RESTART,
          "a refused spawn is retried");
    CHECK(u.last_reason == PROC_EXIT_REASON_START_FAILED &&
              u.last_status == WASMOS_ERR_PROC_SPAWN_FS_READ,
          "with the spawn error as its last end");
}

static void test_stale_events_are_ignored(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    CHECK(sv_unit_exited(&u, &k_timing, 1, PROC_EXIT_REASON_EXITED, 1) == SV_VERDICT_NONE,
          "an exit before a spawn is ignored");
    sv_unit_spawned(&u, &k_timing, 9, 1);
    CHECK(u.state == SV_UNIT_SPAWN_DUE && u.pid == 0u, "a spawn reply nobody sent is ignored");
    CHECK(sv_unit_spawn_failed(&u, &k_timing, -1, 1) == SV_VERDICT_NONE,
          "as is a refusal nobody asked for");
}

static void test_the_list_reports_each_held_unit(void) {
    sv_unit_t units[4];
    wasmos_supervisor_unit_record_t out[4];
    uint32_t total = 99;
    memset(units, 0, sizeof(units));
    memset(out, 0x5A, sizeof(out));
    units[1] = parse_ok("cli@tty1 /c.wap restart=always class=required tty=1", 0);
    start(&units[1], 12, 0);
    sv_unit_ready(&units[1], &k_timing, 5);
    units[3] = parse_ok("probe /p.wap", 0);
    start(&units[3], 40, 0);
    (void)sv_unit_exited(&units[3], &k_timing, 7, PROC_EXIT_REASON_TRAPPED, 100);

    CHECK(sv_units_list(units, 4, out, 4, &total) == 2, "two records, free slots skipped");
    CHECK(total == 2, "two units held");
    CHECK(strcmp(out[0].name, "cli@tty1") == 0, "table order: cli first");
    CHECK(out[0].state == SUPERVISOR_UNIT_STATE_RUNNING, "cli is running");
    CHECK(out[0].restart == SUPERVISOR_RESTART_ALWAYS, "cli restart policy");
    CHECK(out[0].cls == SUPERVISOR_CLASS_REQUIRED, "cli class");
    CHECK(out[0].tty == 2u && out[0].pid == 12u, "cli tty pin and pid");
    CHECK(out[0].restart_count == 0u && out[0].last_reason == 0u, "cli never ended");
    CHECK(strcmp(out[1].name, "probe") == 0, "probe second");
    CHECK(out[1].state == SUPERVISOR_UNIT_STATE_SPAWN_DUE, "probe waits out its backoff");
    CHECK(out[1].pid == 0u, "no instance while it waits");
    CHECK(out[1].restart_count == 1u, "one restart scheduled");
    CHECK(out[1].last_reason == PROC_EXIT_REASON_TRAPPED && out[1].last_status == 7,
          "the end that caused it");
    CHECK(out[2].name[0] == 0x5A, "nothing written past the records");
}

static void test_a_short_list_reports_the_total(void) {
    sv_unit_t units[3];
    wasmos_supervisor_unit_record_t out[1];
    uint32_t total = 0;
    memset(units, 0, sizeof(units));
    for (int i = 0; i < 3; ++i) {
        char spec[32];
        snprintf(spec, sizeof(spec), "u%d /u.wap", i);
        units[i] = parse_ok(spec, 0);
    }
    CHECK(sv_units_list(units, 3, out, 1, &total) == 1, "one record fits");
    CHECK(total == 3, "the total says the room was short");
    CHECK(strcmp(out[0].name, "u0") == 0, "the first unit");
    CHECK(sv_units_list(units, 3, 0, 0, &total) == 0 && total == 3, "a count-only call");
}

int main(void) {
    test_a_spec_parses_into_its_policy();
    test_a_bare_spec_takes_the_defaults();
    test_a_bad_spec_is_refused_untouched();
    test_a_path_too_long_is_refused();
    test_the_longest_name_fits();
    test_a_crash_restarts_with_a_doubling_backoff();
    test_the_backoff_is_capped();
    test_the_fifth_end_in_the_window_gives_up();
    test_old_ends_age_out_of_the_window();
    test_a_stable_run_resets_only_the_backoff();
    test_a_missed_ready_is_killed_and_counted();
    test_a_clean_exit_stops_an_on_failure_unit();
    test_always_restarts_a_clean_exit_and_counts_it();
    test_never_does_not_restart();
    test_a_critical_unit_is_never_restarted();
    test_a_refused_spawn_is_a_failure();
    test_stale_events_are_ignored();
    test_the_list_reports_each_held_unit();
    test_a_short_list_reports_the_total();
    if (g_failures != 0) {
        printf("test_supervisor_unit: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_supervisor_unit: ok\n");
    return 0;
}
