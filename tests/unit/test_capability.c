/* Host unit test for the per-context capability records
 * (src/kernel/capability.c): what a reaped context leaves behind.
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include "capability.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

/* capability.c matches manifest names with this; the kernel's definition lives
 * in libc.c, which a host binary cannot link. 1 only when the lengths match. */
int str_eq_bytes(const uint8_t* bytes, size_t bytes_len, const char* lit) {
    size_t i = 0;
    if (!bytes || !lit) {
        return 0;
    }
    while (lit[i]) {
        if (i >= bytes_len || bytes[i] != (uint8_t)lit[i]) {
            return 0;
        }
        i++;
    }
    return i == bytes_len;
}

static int grant(uint32_t context_id, const char* name) {
    return capability_grant_name(context_id, (const uint8_t*)name, (uint32_t)strlen(name), 0);
}

/* Regression: 2026-11-20-capability-records-outlive-processes -- a context's
 * record was never dropped when its process was reaped, so every process that
 * ever held a capability (each `svc` run, which holds svc.control) left one
 * behind: kernel memory grew with use and every authorization check scanned
 * records of the dead. */
static int test_a_released_context_holds_nothing(void) {
    capability_init();
    if (grant(41, "svc.control") != 0 || !capability_has(41, CAP_SVC_CONTROL)) {
        return __LINE__;
    }
    capability_release_context(41);
    if (capability_has(41, CAP_SVC_CONTROL) || capability_context_configured(41)) {
        return __LINE__;
    }
    return 0;
}

static int test_releasing_one_context_leaves_the_others(void) {
    capability_init();
    if (grant(42, "spawn.grant") != 0 || grant(43, "svc.control") != 0) {
        return __LINE__;
    }
    capability_release_context(42);
    capability_release_context(99); /* never had a record */
    if (capability_has(42, CAP_SPAWN_GRANT) || !capability_has(43, CAP_SVC_CONTROL)) {
        return __LINE__;
    }
    if (!capability_has(0, CAP_SVC_CONTROL)) {
        return __LINE__; /* the kernel context keeps everything */
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_released_context_holds_nothing),
        WASMOS_TEST_CASE(test_releasing_one_context_leaves_the_others),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_capability: ok\n");
    return 0;
}
