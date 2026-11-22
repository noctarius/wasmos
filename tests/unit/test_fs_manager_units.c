/* test_fs_manager_units.c — the supervised units behind fs-manager's mounts
 * (fs_manager_units.h): what a mount's unit is called, the spec it is submitted
 * with, and the table that records it until its retraction is answered.
 *
 * src/services/fs_manager/fs_manager_units.c is the only source linked in: every
 * decision is a pure function of its arguments or of a table built per case.
 *
 * The cases report through assert(); main() prints "ok" only if every case ran
 * to completion. The suite is compiled without -DNDEBUG.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "fs_manager_units.h"
#include "wasmos_driver_abi.h"

static void test_name_is_the_module_stem_at_the_mount(void) {
    char name[SUPERVISOR_UNIT_NAME_MAX + 1];
    assert(fsmgr_unit_name("/boot/system/drivers/fs_wfs.wap", "/wfs", name, sizeof(name)) ==
           (int32_t)strlen("fs_wfs@/wfs"));
    assert(strcmp(name, "fs_wfs@/wfs") == 0);
    assert(fsmgr_unit_name("/init/system/drivers/fs_tmpfs.wap", "/home/user", name, sizeof(name)) >
           0);
    assert(strcmp(name, "fs_tmpfs@/home/user") == 0);
}

static void test_a_module_without_directory_or_extension_is_its_own_stem(void) {
    char name[SUPERVISOR_UNIT_NAME_MAX + 1];
    assert(fsmgr_unit_name("fs_fat", "/user", name, sizeof(name)) > 0);
    assert(strcmp(name, "fs_fat@/user") == 0);
}

/* A unit name is a service name's length (35); a mount path may be longer, and a
 * truncated name would make two mounts one unit. */
static void test_a_name_past_the_unit_ceiling_is_refused_not_truncated(void) {
    char name[64];
    /* "fs_tmpfs@" is 9 characters: 26 more fit exactly, 27 do not. */
    const char* fits = "/abcdefghijklmnopqrstuvwxy";      /* 26 */
    const char* too_long = "/abcdefghijklmnopqrstuvwxyz"; /* 27 */
    assert(fsmgr_unit_name("/x/fs_tmpfs.wap", fits, name, sizeof(name)) ==
           (int32_t)SUPERVISOR_UNIT_NAME_MAX);
    name[0] = 'x';
    assert(fsmgr_unit_name("/x/fs_tmpfs.wap", too_long, name, sizeof(name)) ==
           WASMOS_ERR_FS_PATH_TOO_LONG);
    assert(name[0] == '\0');
}

static void test_a_name_past_the_buffer_is_refused(void) {
    char name[8];
    assert(fsmgr_unit_name("/x/fs_wfs.wap", "/wfs", name, sizeof(name)) ==
           WASMOS_ERR_FS_PATH_TOO_LONG);
    assert(name[0] == '\0');
}

static void test_spec_carries_policy_and_args(void) {
    char spec[SUPERVISOR_SPEC_MAX + 1];
    int32_t len = fsmgr_unit_spec("fs_wfs@/wfs",
                                  "/boot/system/drivers/fs_wfs.wap",
                                  "id=block:ata:2 mount=/wfs",
                                  spec,
                                  sizeof(spec));
    const char* want =
        "fs_wfs@/wfs /boot/system/drivers/fs_wfs.wap restart=on-failure -- id=block:ata:2 "
        "mount=/wfs";
    assert(len == (int32_t)strlen(want));
    assert(strcmp(spec, want) == 0);
}

static void test_spec_past_the_supervisor_ceiling_is_refused(void) {
    char spec[512];
    char args[300];
    memset(args, 'a', sizeof(args) - 1);
    args[sizeof(args) - 1] = '\0';
    assert(fsmgr_unit_spec("fs_wfs@/wfs", "/x/fs_wfs.wap", args, spec, sizeof(spec)) ==
           WASMOS_ERR_FS_BAD_ARGS);
    assert(spec[0] == '\0');
}

static void test_claim_records_a_submitting_unit(void) {
    fsmgr_unit_t table[4];
    memset(table, 0, sizeof(table));
    fsmgr_unit_t* unit = fsmgr_unit_claim(table, 4, "fs_wfs@/wfs", "/wfs");
    assert(unit != NULL);
    assert(unit->state == FSMGR_UNIT_SUBMITTING);
    assert(strcmp(unit->name, "fs_wfs@/wfs") == 0);
    assert(strcmp(unit->mount, "/wfs") == 0);
    assert(unit->client == -1 && unit->buffer_id == -1);
    assert(fsmgr_unit_by_mount(table, 4, "/wfs") == unit);
    assert(fsmgr_unit_by_mount(table, 4, "/user") == NULL);
}

/* The supervisor refuses a second unit of one name; the table does too, so a
 * remount cannot race its own retraction into two records. */
static void test_a_name_already_held_is_not_claimed_twice(void) {
    fsmgr_unit_t table[4];
    memset(table, 0, sizeof(table));
    assert(fsmgr_unit_claim(table, 4, "fs_wfs@/wfs", "/wfs") != NULL);
    assert(fsmgr_unit_claim(table, 4, "fs_wfs@/wfs", "/wfs") == NULL);
}

static void test_a_full_table_refuses_and_a_release_frees_a_slot(void) {
    fsmgr_unit_t table[2];
    memset(table, 0, sizeof(table));
    fsmgr_unit_t* a = fsmgr_unit_claim(table, 2, "fs_tmpfs@/a", "/a");
    assert(a != NULL);
    assert(fsmgr_unit_claim(table, 2, "fs_tmpfs@/b", "/b") != NULL);
    assert(fsmgr_unit_claim(table, 2, "fs_tmpfs@/c", "/c") == NULL);
    fsmgr_unit_release(a);
    assert(a->state == FSMGR_UNIT_FREE);
    assert(fsmgr_unit_by_mount(table, 2, "/a") == NULL);
    assert(fsmgr_unit_claim(table, 2, "fs_tmpfs@/c", "/c") != NULL);
}

static void test_by_request_finds_only_an_outstanding_request(void) {
    fsmgr_unit_t table[4];
    memset(table, 0, sizeof(table));
    fsmgr_unit_t* unit = fsmgr_unit_claim(table, 4, "fs_wfs@/wfs", "/wfs");
    unit->request_id = 77;
    assert(fsmgr_unit_by_request(table, 4, 77) == unit);
    assert(fsmgr_unit_by_request(table, 4, 78) == NULL);
    unit->state = FSMGR_UNIT_HELD;
    assert(fsmgr_unit_by_request(table, 4, 77) == NULL);
    unit->state = FSMGR_UNIT_RETRACTING;
    assert(fsmgr_unit_by_request(table, 4, 77) == unit);
}

static void test_deadline_holds_across_the_wrap(void) {
    assert(!fsmgr_deadline_passed(100u, 200u));
    assert(fsmgr_deadline_passed(200u, 200u));
    assert(fsmgr_deadline_passed(201u, 200u));
    assert(!fsmgr_deadline_passed(0xFFFFFFF0u, 0x10u));
    assert(fsmgr_deadline_passed(0x20u, 0x10u));
}

/* Regression: #68-review-mount-path-claimed-twice -- the claim checked only the
 * unit name, so while fs_wfs@/wfs was restarting (its mount out of the table) a
 * tmpfs could be mounted at /wfs, and both backends served the path once the
 * first one registered again. */
static void test_a_mount_path_already_held_is_not_claimed_by_another_type(void) {
    fsmgr_unit_t table[4];
    memset(table, 0, sizeof(table));
    assert(fsmgr_unit_claim(table, 4, "fs_wfs@/wfs", "/wfs") != NULL);
    assert(fsmgr_unit_claim(table, 4, "fs_tmpfs@/wfs", "/wfs") == NULL);
}

static fsmgr_unit_t retiring_unit(int32_t quiesced, uint32_t deadline) {
    fsmgr_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    unit.state = FSMGR_UNIT_RETIRING;
    unit.backend = 9;
    unit.quiesced = quiesced;
    unit.deadline_ms = deadline;
    unit.client = 4;
    return unit;
}

static void test_a_quiesced_backend_is_retracted_once_it_has_ended(void) {
    fsmgr_unit_t unit = retiring_unit(1, 1000u);
    assert(fsmgr_unit_due(&unit, 10u, 1) == FSMGR_UNIT_WAIT);
    assert(fsmgr_unit_due(&unit, 10u, 0) == FSMGR_UNIT_DO_RETRACT);
}

/* Regression: #68-review-unmount-waits-on-a-silent-backend -- the deadline
 * applied only after the quiesce was answered, so a backend that never answered
 * SHUTDOWN_REQ held its unmount forever. */
static void test_a_backend_that_never_quiesces_is_retracted_at_the_deadline(void) {
    fsmgr_unit_t unit = retiring_unit(0, 1000u);
    assert(fsmgr_unit_due(&unit, 999u, 1) == FSMGR_UNIT_WAIT);
    assert(fsmgr_unit_due(&unit, 1000u, 1) == FSMGR_UNIT_DO_RETRACT);
    /* Without its quiesce answered, an ended backend still waits for the record
     * that answers it: the end is not the unmount's to report. */
    assert(fsmgr_unit_due(&unit, 10u, 0) == FSMGR_UNIT_WAIT);
}

/* Regression: #68-review-forced-retract-answers-early -- a retraction that
 * killed a live backend was answered at once, while the supervisor kept the
 * name until the backend ended, so an immediate remount was refused EXISTS. */
static void test_a_retraction_that_killed_an_instance_waits_for_its_end(void) {
    fsmgr_unit_t unit = retiring_unit(1, 0u);
    unit.state = FSMGR_UNIT_RETRACTING;
    assert(fsmgr_unit_retracted(&unit, 1, 0, 57, 100u) == FSMGR_UNIT_WAIT);
    assert(unit.state == FSMGR_UNIT_ENDING);
    assert(fsmgr_unit_due(&unit, 101u, 1) == FSMGR_UNIT_WAIT);
    assert(fsmgr_unit_due(&unit, 101u, 0) == FSMGR_UNIT_DO_ANSWER);
    assert(fsmgr_unit_due(&unit, 100u + FSMGR_RETIRE_TIMEOUT_MS, 1) == FSMGR_UNIT_DO_ANSWER);
}

static void test_a_retraction_of_an_ended_instance_is_answered_at_once(void) {
    fsmgr_unit_t unit = retiring_unit(1, 0u);
    unit.state = FSMGR_UNIT_RETRACTING;
    assert(fsmgr_unit_retracted(&unit, 1, 0, 0, 100u) == FSMGR_UNIT_DO_ANSWER);
}

/* Regression: #68-review-refused-retract-reported-as-success -- a refused
 * retraction answered the unmount as a success and forgot the unit, whose name
 * the supervisor still held. */
static void test_a_refused_retraction_is_reported_and_the_unit_kept(void) {
    fsmgr_unit_t unit = retiring_unit(1, 0u);
    unit.state = FSMGR_UNIT_RETRACTING;
    assert(fsmgr_unit_retracted(&unit, 0, WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED, 0, 100u) ==
           FSMGR_UNIT_DO_REFUSE);
    assert(unit.state == FSMGR_UNIT_HELD);
    assert(unit.result == WASMOS_ERR_SUPERVISOR_NOT_AUTHORIZED);
}

static void test_a_retraction_of_a_unit_already_gone_is_a_success(void) {
    fsmgr_unit_t unit = retiring_unit(1, 0u);
    unit.state = FSMGR_UNIT_RETRACTING;
    assert(fsmgr_unit_retracted(&unit, 0, WASMOS_ERR_SUPERVISOR_NO_UNIT, 0, 100u) ==
           FSMGR_UNIT_DO_ANSWER);
}

/* Regression: #68-review-unsent-retract-forgets-the-unit -- a RETRACT that
 * could not be sent released the unit and answered the unmount as done. */
static void test_an_unsent_retraction_is_retried(void) {
    fsmgr_unit_t unit = retiring_unit(1, 0u);
    unit.state = FSMGR_UNIT_RETRACTING;
    fsmgr_unit_retract_unsent(&unit, 100u);
    assert(unit.state == FSMGR_UNIT_RETIRING);
    assert(fsmgr_unit_due(&unit, 100u, 1) == FSMGR_UNIT_WAIT);
    assert(fsmgr_unit_due(&unit, 100u + FSMGR_RETRACT_RETRY_MS, 1) == FSMGR_UNIT_DO_RETRACT);
}

static void test_held_and_submitting_units_are_never_due(void) {
    fsmgr_unit_t unit = retiring_unit(1, 0u);
    unit.state = FSMGR_UNIT_HELD;
    assert(fsmgr_unit_due(&unit, 5000u, 0) == FSMGR_UNIT_WAIT);
    unit.state = FSMGR_UNIT_SUBMITTING;
    assert(fsmgr_unit_due(&unit, 5000u, 0) == FSMGR_UNIT_WAIT);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_name_is_the_module_stem_at_the_mount),
        WASMOS_TEST_CASE(test_a_module_without_directory_or_extension_is_its_own_stem),
        WASMOS_TEST_CASE(test_a_name_past_the_unit_ceiling_is_refused_not_truncated),
        WASMOS_TEST_CASE(test_a_name_past_the_buffer_is_refused),
        WASMOS_TEST_CASE(test_spec_carries_policy_and_args),
        WASMOS_TEST_CASE(test_spec_past_the_supervisor_ceiling_is_refused),
        WASMOS_TEST_CASE(test_claim_records_a_submitting_unit),
        WASMOS_TEST_CASE(test_a_name_already_held_is_not_claimed_twice),
        WASMOS_TEST_CASE(test_a_full_table_refuses_and_a_release_frees_a_slot),
        WASMOS_TEST_CASE(test_by_request_finds_only_an_outstanding_request),
        WASMOS_TEST_CASE(test_deadline_holds_across_the_wrap),
        WASMOS_TEST_CASE(test_a_mount_path_already_held_is_not_claimed_by_another_type),
        WASMOS_TEST_CASE(test_a_quiesced_backend_is_retracted_once_it_has_ended),
        WASMOS_TEST_CASE(test_a_backend_that_never_quiesces_is_retracted_at_the_deadline),
        WASMOS_TEST_CASE(test_a_retraction_that_killed_an_instance_waits_for_its_end),
        WASMOS_TEST_CASE(test_a_retraction_of_an_ended_instance_is_answered_at_once),
        WASMOS_TEST_CASE(test_a_refused_retraction_is_reported_and_the_unit_kept),
        WASMOS_TEST_CASE(test_a_retraction_of_a_unit_already_gone_is_a_success),
        WASMOS_TEST_CASE(test_an_unsent_retraction_is_retried),
        WASMOS_TEST_CASE(test_held_and_submitting_units_are_never_due),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_fs_manager_units: ok\n");
    return 0;
}
