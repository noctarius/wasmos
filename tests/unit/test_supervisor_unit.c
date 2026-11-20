/* test_supervisor_unit.c - the supervisor's per-unit policy
 * (src/services/supervisor/supervisor_unit.c).
 *
 * A unit spec has to parse into exactly the policy it states and refuse
 * anything else, since a refused submission spawns nothing. Once running, a
 * unit is restarted by its policy with a doubling backoff, gives up after too
 * many ends inside the sliding window, resets its backoff only after a stable
 * run, is killed when it misses its READY deadline, and a critical unit's
 * first end is reported as such instead of restarted. A kill on request is
 * refused unless an instance runs, and its end is a failure. What the supervisor
 * reports of its table over SUPERVISOR_IPC_LIST_REQ is each held unit's own
 * state, in ABI values, and never more records than the caller made room for.
 * Only a sender holding svc.control changes what runs, and only one also
 * holding spawn.grant hands a unit hardware grants.
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
    .probe_interval_ms = 10000,
    .probe_misses_max = 3,
    .scale = 1,
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
    CHECK(u.probe_s == 0u && sv_unit_probe_interval(&u, &k_timing) == k_timing.probe_interval_ms,
          "the default probe period");
    CHECK(u.probe_ep == SV_PROBE_NONE, "not probed before a READY names an endpoint");
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
        "name /p probe=0",
        "name /p probe=x",
        "name /p probe=1000",
        "a12345678901234567890123456789012345 /p", /* 36 bytes: one too long */
        "name module:",                            /* names no module */
        "name modules:fs-manager",                 /* neither absolute nor the prefix */
        "name module:fs-init -- -x",               /* a module takes no args */
        "name module:fs-init tty=1",               /* nor a tty pin */
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

/* A boot-chain unit names its boot module, not a file: the supervisor spawns
 * it from the module image, before any filesystem exists to read one from. */
static void test_a_module_unit_names_its_boot_module(void) {
    sv_unit_t u = parse_ok("fs-manager module:fs-manager class=critical", 0);
    CHECK(u.cls == SV_CLASS_CRITICAL, "class");
    CHECK(sv_unit_module(&u) != 0 && strcmp(sv_unit_module(&u), "fs-manager") == 0,
          "the module is the app name after the prefix");
    sv_unit_t f = parse_ok("logger /boot/system/services/logger.wap", 0);
    CHECK(sv_unit_module(&f) == 0, "a file unit names no module");
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
    sv_unit_ready(&u, &k_timing, SV_PROBE_NONE, now);
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
    sv_unit_ready(&u, &k_timing, SV_PROBE_NONE, now);
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
    sv_unit_ready(&u, &k_timing, SV_PROBE_NONE, k_timing.ready_timeout_ms + 1u);
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
    sv_unit_ready(&u, &k_timing, SV_PROBE_NONE, 0);
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
    sv_unit_ready(&units[1], &k_timing, SV_PROBE_NONE, 5);
    units[3] = parse_ok("probe /p.wap", 0);
    start(&units[3], 40, 0);
    (void)sv_unit_exited(&units[3], &k_timing, 7, PROC_EXIT_REASON_TRAPPED, 100);

    CHECK(sv_units_list(units, 4, &k_timing, out, 4, &total) == 2,
          "two records, free slots skipped");
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
    CHECK(sv_units_list(units, 3, &k_timing, out, 1, &total) == 1, "one record fits");
    CHECK(total == 3, "the total says the room was short");
    CHECK(strcmp(out[0].name, "u0") == 0, "the first unit");
    CHECK(sv_units_list(units, 3, &k_timing, 0, 0, &total) == 0 && total == 3, "a count-only call");
}

/* Brings a parsed unit to a READY instance with `pid` that named `ep` for
 * probes, at `now`. */
static void start_probed(sv_unit_t* u, uint32_t pid, uint32_t ep, uint32_t now) {
    start(u, pid, now);
    sv_unit_ready(u, &k_timing, ep, now);
}

/* Sends the due probe as `id` at `now`; checks that one was due. */
static void probe(sv_unit_t* u, uint32_t id, uint32_t now) {
    CHECK(sv_unit_probe_tick(u, &k_timing, now) == SV_VERDICT_PROBE, "a probe is due");
    sv_unit_probe_sent(u, &k_timing, id, now);
}

static void test_a_spec_names_its_probe_period_in_scaled_seconds(void) {
    sv_unit_t u = parse_ok("svc /p probe=2", 0);
    sv_timing_t t = k_timing;
    CHECK(u.probe_s == 2u && sv_unit_probe_interval(&u, &k_timing) == 2000u, "probe=2 is 2 s");
    t.scale = 3;
    CHECK(sv_unit_probe_interval(&u, &t) == 6000u, "and the timescale stretches it");
}

static void test_a_unit_that_names_no_endpoint_is_never_probed(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    uint32_t d = 0;
    start(&u, 7, 0);
    sv_unit_ready(&u, &k_timing, SV_PROBE_NONE, 0);
    CHECK(sv_unit_probe_tick(&u, &k_timing, 0) == SV_VERDICT_NONE, "no probe at READY");
    CHECK(sv_unit_probe_tick(&u, &k_timing, 1000000) == SV_VERDICT_NONE, "nor ever after");
    CHECK(sv_unit_deadline(&u, &d) == 1 && d == k_timing.stable_ms, "only the stable deadline");
    CHECK(sv_unit_tick(&u, &k_timing, k_timing.stable_ms) == SV_VERDICT_NONE &&
              u.state == SV_UNIT_STABLE,
          "and it becomes stable on time");
}

static void test_a_unit_that_answers_stays_up(void) {
    sv_unit_t u = parse_ok("svc /p probe=1", 0);
    uint32_t now = 0;
    start_probed(&u, 7, 33, now);
    for (uint32_t i = 1; i <= 20; ++i) {
        probe(&u, i, now);
        CHECK(sv_unit_probe_tick(&u, &k_timing, now + 999u) == SV_VERDICT_NONE,
              "nothing before the period is up");
        CHECK(sv_unit_probe_answered(&u, &k_timing, i, now + 500u) == 1, "the answer counts");
        now += 1000u;
    }
    CHECK(u.probes_answered == 20u && u.probe_misses == 0u, "twenty answered, none missed");
    CHECK(u.state == SV_UNIT_STABLE, "and it became stable along the way");
}

/* A hung unit never exits; its unanswered probes are the only sign. */
static void test_three_misses_in_a_row_kill_the_unit(void) {
    sv_unit_t u = parse_ok("svc /p probe=1", 0);
    start_probed(&u, 7, 33, 0);
    probe(&u, 1, 0);
    CHECK(sv_unit_probe_answered(&u, &k_timing, 1, 10) == 1, "the first probe is answered");
    probe(&u, 2, 1000);
    probe(&u, 3, 2000);
    CHECK(u.probe_misses == 1u, "one miss");
    probe(&u, 4, 3000);
    CHECK(u.probe_misses == 2u, "two misses");
    CHECK(sv_unit_probe_tick(&u, &k_timing, 4000) == SV_VERDICT_HUNG, "the third kills it");
    CHECK(u.state == SV_UNIT_STOPPING && u.pid == 7u, "stopping; the pid is still the one to kill");
    CHECK(sv_unit_probe_tick(&u, &k_timing, 5000) == SV_VERDICT_NONE,
          "a stopping unit is not probed");
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_KILLED, 4100) == SV_VERDICT_RESTART,
          "its end is a failure and restarts it");
    CHECK(u.probe_ep == SV_PROBE_NONE && u.probe_misses == 3u,
          "the ended instance is not probed, and its misses are still reported");
}

static void test_an_answer_resets_the_misses(void) {
    sv_unit_t u = parse_ok("svc /p probe=1", 0);
    start_probed(&u, 7, 33, 0);
    probe(&u, 1, 0);
    probe(&u, 2, 1000);
    probe(&u, 3, 2000);
    CHECK(u.probe_misses == 2u, "two misses");
    CHECK(sv_unit_probe_answered(&u, &k_timing, 3, 2500) == 1 && u.probe_misses == 0u,
          "an answer clears them");
    probe(&u, 4, 3000);
    probe(&u, 5, 4000);
    probe(&u, 6, 5000);
    CHECK(sv_unit_probe_tick(&u, &k_timing, 6000) == SV_VERDICT_HUNG, "only misses in a row count");
}

static void test_a_late_answer_does_not_count(void) {
    sv_unit_t u = parse_ok("svc /p probe=1", 0);
    start_probed(&u, 7, 33, 0);
    probe(&u, 1, 0);
    probe(&u, 2, 1000);
    CHECK(sv_unit_probe_answered(&u, &k_timing, 1, 1100) == 0, "the earlier probe's answer");
    CHECK(u.probe_misses == 1u && u.probes_answered == 0u, "changes nothing");
    CHECK(sv_unit_probe_answered(&u, &k_timing, 0, 1100) == 0, "nor does request id 0");
}

static void test_stability_waits_for_the_first_answer(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    uint32_t d = 0;
    uint32_t now = k_timing.stable_ms;
    start_probed(&u, 7, 33, 0);
    probe(&u, 1, 0);
    CHECK(sv_unit_tick(&u, &k_timing, now) == SV_VERDICT_NONE && u.state == SV_UNIT_RUNNING,
          "READY alone is not stable");
    CHECK(sv_unit_deadline(&u, &d) == 1 && d == k_timing.probe_interval_ms,
          "and the passed stable deadline is not a deadline to wake for");
    CHECK(sv_unit_probe_answered(&u, &k_timing, 1, now + 5u) == 1 && u.state == SV_UNIT_STABLE,
          "the first answer after it makes the unit stable");

    u = parse_ok("svc /p", 0);
    start_probed(&u, 8, 33, 0);
    probe(&u, 1, 0);
    CHECK(sv_unit_probe_answered(&u, &k_timing, 1, 5) == 1 && u.state == SV_UNIT_RUNNING,
          "an answer before the stable deadline");
    CHECK(sv_unit_tick(&u, &k_timing, k_timing.stable_ms) == SV_VERDICT_NONE &&
              u.state == SV_UNIT_STABLE,
          "leaves the deadline to make it stable");
}

static void test_a_respawn_starts_the_probe_count_over(void) {
    sv_unit_t u = parse_ok("svc /p probe=1", 0);
    start_probed(&u, 7, 33, 0);
    probe(&u, 1, 0);
    (void)sv_unit_probe_answered(&u, &k_timing, 1, 10);
    probe(&u, 2, 1000);
    (void)sv_unit_exited(&u, &k_timing, 1, PROC_EXIT_REASON_TRAPPED, 1500);
    CHECK(sv_unit_tick(&u, &k_timing, 1500 + k_timing.backoff_min_ms) == SV_VERDICT_SPAWN,
          "the restart is due");
    sv_unit_spawn_sent(&u);
    sv_unit_spawned(&u, &k_timing, 9, 2000);
    CHECK(u.probes_answered == 0u && u.probe_misses == 0u && u.probe_pending == 0u,
          "a new instance starts with no probe history");
    CHECK(sv_unit_probe_tick(&u, &k_timing, 2000) == SV_VERDICT_NONE,
          "and is not probed before its own READY");
}

static void test_the_list_reports_liveness(void) {
    sv_unit_t units[2];
    wasmos_supervisor_unit_record_t out[2];
    uint32_t total = 0;
    memset(units, 0, sizeof(units));
    units[0] = parse_ok("probed /p.wap probe=4", 0);
    start_probed(&units[0], 12, 33, 0);
    probe(&units[0], 1, 0);
    (void)sv_unit_probe_answered(&units[0], &k_timing, 1, 1);
    probe(&units[0], 2, 4000);
    probe(&units[0], 3, 8000);
    units[1] = parse_ok("plain /p.wap", 0);
    start(&units[1], 13, 0);
    sv_unit_ready(&units[1], &k_timing, SV_PROBE_NONE, 0);

    CHECK(sv_units_list(units, 2, &k_timing, out, 2, &total) == 2, "two records");
    CHECK(out[0].probe_interval_ms == 4000u, "the probed unit's period");
    CHECK(out[0].probes_answered == 1u && out[0].probe_misses == 1u, "its answers and misses");
    CHECK(out[1].probe_interval_ms == 0u && out[1].probes_answered == 0u,
          "an unprobed unit reports no period");
}

/* A boot table: vt and cli required, logger optional, all in the boot set. */
static void boot_table(sv_unit_t units[4]) {
    memset(units, 0, 4 * sizeof(units[0]));
    units[0] = parse_ok("vt /vt.wap class=required", 0);
    units[1] = parse_ok("logger /logger.wap", 0);
    units[3] = parse_ok("cli@tty1 /cli.wap class=critical", 0);
    for (int i = 0; i < 4; ++i) {
        units[i].boot = units[i].state != SV_UNIT_FREE;
    }
    start(&units[0], 10, 0);
    start(&units[1], 11, 0);
    start(&units[3], 13, 0);
}

static void test_boot_completes_when_every_required_unit_is_up(void) {
    sv_unit_t units[4];
    uint32_t count = 99;
    boot_table(units);
    CHECK(sv_boot_check(units, 4, &count, 0) == SV_BOOT_PENDING, "nothing is up yet");
    sv_unit_ready(&units[0], &k_timing, SV_PROBE_NONE, 5);
    CHECK(sv_boot_check(units, 4, &count, 0) == SV_BOOT_PENDING, "cli is not up yet");
    sv_unit_ready(&units[3], &k_timing, SV_PROBE_NONE, 6);
    CHECK(sv_boot_check(units, 4, &count, 0) == SV_BOOT_COMPLETE && count == 2u,
          "vt and cli are up; the optional logger does not count");
    CHECK(units[0].state == SV_UNIT_RUNNING, "up does not wait to be stable");
}

static void test_a_probed_unit_is_up_once_it_answers(void) {
    sv_unit_t units[4];
    boot_table(units);
    sv_unit_ready(&units[0], &k_timing, 44, 5);
    sv_unit_ready(&units[3], &k_timing, SV_PROBE_NONE, 5);
    CHECK(!sv_unit_is_up(&units[0]), "READY alone is not up for a probed unit");
    CHECK(sv_boot_check(units, 4, 0, 0) == SV_BOOT_PENDING, "so boot waits for it");
    probe(&units[0], 1, 5);
    CHECK(sv_unit_probe_answered(&units[0], &k_timing, 1, 6) == 1, "it answers");
    CHECK(sv_unit_is_up(&units[0]) && sv_boot_check(units, 4, 0, 0) == SV_BOOT_COMPLETE,
          "and is up");
}

static void test_a_required_unit_that_gives_up_fails_the_boot(void) {
    sv_unit_t units[4];
    uint32_t index = 99;
    boot_table(units);
    sv_unit_ready(&units[3], &k_timing, SV_PROBE_NONE, 5);
    units[0].restart = SV_RESTART_NEVER;
    CHECK(sv_unit_exited(&units[0], &k_timing, 1, PROC_EXIT_REASON_TRAPPED, 7) == SV_VERDICT_FAILED,
          "vt fails for good");
    CHECK(sv_boot_check(units, 4, 0, &index) == SV_BOOT_FAILED && index == 0u,
          "the boot fails, naming vt");
}

static void test_an_optional_or_late_unit_does_not_hold_the_boot(void) {
    sv_unit_t units[4];
    boot_table(units);
    sv_unit_ready(&units[0], &k_timing, SV_PROBE_NONE, 5);
    sv_unit_ready(&units[3], &k_timing, SV_PROBE_NONE, 5);
    (void)sv_unit_exited(&units[1], &k_timing, 1, PROC_EXIT_REASON_TRAPPED, 6);
    units[1].restart = SV_RESTART_NEVER;
    units[2] = parse_ok("late /late.wap class=required", 0);
    CHECK(sv_boot_check(units, 4, 0, 0) == SV_BOOT_COMPLETE,
          "a crashing optional unit and a required unit submitted later do not count");
}

static void test_a_restarting_required_unit_holds_the_boot(void) {
    sv_unit_t units[4];
    boot_table(units);
    sv_unit_ready(&units[3], &k_timing, SV_PROBE_NONE, 5);
    sv_unit_ready(&units[0], &k_timing, SV_PROBE_NONE, 5);
    CHECK(sv_unit_exited(&units[0], &k_timing, 1, PROC_EXIT_REASON_TRAPPED, 7) ==
              SV_VERDICT_RESTART,
          "vt crashed and is due again");
    CHECK(sv_boot_check(units, 4, 0, 0) == SV_BOOT_PENDING, "boot waits for it to come back");
}

/* Writes a wasmos_spawn_caps_v2_t with `ranges` I/O ranges into buf. */
static uint32_t caps_v2(uint8_t* buf, uint32_t flags, uint16_t ranges) {
    wasmos_spawn_caps_v2_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cap_flags = flags;
    hdr.io_range_count = ranges;
    memset(buf, 0, SUPERVISOR_CAPS_MAX);
    memcpy(buf, &hdr, sizeof(hdr));
    return WASMOS_SPAWN_CAPS_V2_SIZE(ranges, 0);
}

static void test_a_unit_keeps_the_caps_it_was_submitted_with(void) {
    uint8_t buf[SUPERVISOR_CAPS_MAX];
    sv_unit_t u = parse_ok("kbd@acpi:0 /boot/system/drivers/keyboard.wap", 0);
    uint32_t len = caps_v2(buf, DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ, WASMOS_IO_RANGE_LIMIT);
    CHECK(u.caps_len == 0u, "a parsed unit has no caps");
    CHECK(sv_unit_set_caps(&u, buf, len) == 0, "I/O and IRQ caps are taken");
    CHECK(u.caps_len == len && memcmp(u.caps, buf, len) == 0, "the caps are kept verbatim");
    CHECK(sv_unit_set_caps(&u, buf, 0) == 0 && u.caps_len == 0u, "len 0 clears them");
}

static void test_dma_caps_are_refused(void) {
    uint8_t buf[SUPERVISOR_CAPS_MAX];
    sv_unit_t u = parse_ok("rng@pci:0 /p", 0);
    uint32_t len = caps_v2(buf, DEVMGR_CAP_IO_PORT | DEVMGR_CAP_DMA, 0);
    CHECK(sv_unit_set_caps(&u, buf, len) == WASMOS_ERR_SUPERVISOR_DMA_CAPS,
          "a DMA unit cannot be restarted safely, so it is not taken");
    CHECK(u.caps_len == 0u, "a refused descriptor leaves the unit unchanged");
}

static void test_a_caps_length_outside_its_bounds_is_refused(void) {
    uint8_t buf[SUPERVISOR_CAPS_MAX + 1];
    sv_unit_t u = parse_ok("n /p", 0);
    (void)caps_v2(buf, DEVMGR_CAP_IO_PORT, 0);
    CHECK(sv_unit_set_caps(&u, buf, (uint32_t)sizeof(wasmos_spawn_caps_v2_t) - 1u) ==
              WASMOS_ERR_SUPERVISOR_BAD_DESC,
          "shorter than the header");
    CHECK(sv_unit_set_caps(&u, buf, SUPERVISOR_CAPS_MAX + 1u) == WASMOS_ERR_SUPERVISOR_BAD_DESC,
          "longer than SUPERVISOR_CAPS_MAX");
    CHECK(u.caps_len == 0u, "unchanged");
}

static void test_a_kill_on_request_is_a_failure(void) {
    sv_unit_t u = parse_ok("rtc@acpi:0070 /p", 0);
    start(&u, 5, 0);
    sv_unit_ready(&u, &k_timing, SV_PROBE_NONE, 10);
    CHECK(sv_unit_kill(&u) == 0, "a running unit can be killed");
    CHECK(u.state == SV_UNIT_STOPPING && u.pid == 5u, "stopping; the pid is the one to kill");
    CHECK(sv_unit_kill(&u) == WASMOS_ERR_SUPERVISOR_NOT_RUNNING, "not killed twice");
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_EXITED, 20) == SV_VERDICT_RESTART,
          "its end restarts an on-failure unit, whatever the exit says");
    CHECK(u.restart_count == 1u && u.ends_recorded == 1u, "and counts as an end");

    u = parse_ok("svc /p restart=never", 0);
    start(&u, 5, 0);
    CHECK(sv_unit_kill(&u) == 0, "a starting unit can be killed");
    CHECK(sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_KILLED, 10) == SV_VERDICT_FAILED,
          "a killed never-unit is given up as failed, not stopped");
}

static void test_a_unit_without_an_instance_is_not_killed(void) {
    sv_unit_t u = parse_ok("svc /p", 0);
    CHECK(sv_unit_kill(&u) == WASMOS_ERR_SUPERVISOR_NOT_RUNNING, "a due unit has no instance");
    CHECK(u.state == SV_UNIT_SPAWN_DUE, "and is left due");
    CHECK(sv_unit_tick(&u, &k_timing, 0) == SV_VERDICT_SPAWN, "spawn");
    sv_unit_spawn_sent(&u);
    CHECK(sv_unit_kill(&u) == WASMOS_ERR_SUPERVISOR_NOT_RUNNING, "nor has a spawning one");
    CHECK(u.state == SV_UNIT_SPAWNING, "which stays spawning");
    sv_unit_spawned(&u, &k_timing, 5, 0);
    (void)sv_unit_exited(&u, &k_timing, 0, PROC_EXIT_REASON_EXITED, 10);
    CHECK(u.state == SV_UNIT_STOPPED, "stopped");
    CHECK(sv_unit_kill(&u) == WASMOS_ERR_SUPERVISOR_NOT_RUNNING, "nor has a stopped one");
    CHECK(u.state == SV_UNIT_STOPPED && u.pid == 0u, "which stays stopped");
}

/* Regression: 2026-11-20-unchecked-supervisor-requests -- the supervisor
 * never looked at who sent a request, so any process could hand it a unit to
 * run under its name and restart forever. */
static void test_a_submit_without_svc_control_is_refused(void) {
    CHECK(sv_authorize(SUPERVISOR_IPC_SUBMIT_REQ, 0, 0) == WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED,
          "a submit from a sender holding nothing is refused");
    CHECK(sv_authorize(SUPERVISOR_IPC_SUBMIT_REQ, SV_SENDER_GRANT, 0) ==
              WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED,
          "spawn.grant alone does not entitle a sender to submit");
}

/* Regression: 2026-11-20-unchecked-supervisor-requests -- a submission's
 * capability descriptor reached the process manager as the supervisor's own
 * watched spawn, so the supervisor relayed I/O and IRQ grants from any
 * submitter, laundering them past the process manager's requester check. */
static void test_grants_from_a_submitter_without_spawn_grant_are_refused(void) {
    CHECK(sv_authorize(SUPERVISOR_IPC_SUBMIT_REQ, SV_SENDER_CONTROL, 16) ==
              WASMOS_ERR_SUPERVISOR_GRANT_NOT_AUTHORIZED,
          "svc.control alone does not entitle a submitter to grants");
}

/* Regression: 2026-11-20-unchecked-supervisor-requests -- any process could
 * kill any supervised unit, a crash on demand of the vt, a driver or the
 * shell. */
static void test_a_kill_without_svc_control_is_refused(void) {
    CHECK(sv_authorize(SUPERVISOR_IPC_KILL_REQ, 0, 0) == WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED,
          "a kill from a sender holding nothing is refused");
}

/* Regression: 2026-11-20-unchecked-supervisor-requests -- any process could
 * declare the boot set before sysinit had submitted it, so the boot milestone
 * was reported over a partial set. */
static void test_a_boot_submitted_without_svc_control_is_refused(void) {
    CHECK(sv_authorize(SUPERVISOR_IPC_BOOT_SUBMITTED_REQ, 0, 0) ==
              WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED,
          "declaring the boot set requires svc.control");
}

static void test_an_entitled_sender_is_accepted(void) {
    CHECK(sv_authorize(SUPERVISOR_IPC_SUBMIT_REQ, SV_SENDER_CONTROL, 0) == 0,
          "svc.control submits a unit without grants");
    CHECK(sv_authorize(SUPERVISOR_IPC_SUBMIT_REQ, SV_SENDER_CONTROL | SV_SENDER_GRANT, 16) == 0,
          "svc.control and spawn.grant submit a unit with grants");
    CHECK(sv_authorize(SUPERVISOR_IPC_KILL_REQ, SV_SENDER_CONTROL, 0) == 0, "svc.control kills");
    CHECK(sv_authorize(SUPERVISOR_IPC_BOOT_SUBMITTED_REQ, SV_SENDER_CONTROL, 0) == 0,
          "svc.control declares the boot set");
}

static void test_listing_is_open_to_everyone(void) {
    CHECK(sv_authorize(SUPERVISOR_IPC_LIST_REQ, 0, 0) == 0, "anyone may list the units");
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
    test_a_spec_names_its_probe_period_in_scaled_seconds();
    test_a_unit_that_names_no_endpoint_is_never_probed();
    test_a_unit_that_answers_stays_up();
    test_three_misses_in_a_row_kill_the_unit();
    test_an_answer_resets_the_misses();
    test_a_late_answer_does_not_count();
    test_stability_waits_for_the_first_answer();
    test_a_respawn_starts_the_probe_count_over();
    test_the_list_reports_liveness();
    test_boot_completes_when_every_required_unit_is_up();
    test_a_probed_unit_is_up_once_it_answers();
    test_a_required_unit_that_gives_up_fails_the_boot();
    test_an_optional_or_late_unit_does_not_hold_the_boot();
    test_a_restarting_required_unit_holds_the_boot();
    test_a_unit_keeps_the_caps_it_was_submitted_with();
    test_dma_caps_are_refused();
    test_a_caps_length_outside_its_bounds_is_refused();
    test_a_kill_on_request_is_a_failure();
    test_a_unit_without_an_instance_is_not_killed();
    test_a_module_unit_names_its_boot_module();
    test_a_submit_without_svc_control_is_refused();
    test_grants_from_a_submitter_without_spawn_grant_are_refused();
    test_a_kill_without_svc_control_is_refused();
    test_a_boot_submitted_without_svc_control_is_refused();
    test_an_entitled_sender_is_accepted();
    test_listing_is_open_to_everyone();
    if (g_failures != 0) {
        printf("test_supervisor_unit: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_supervisor_unit: ok\n");
    return 0;
}
