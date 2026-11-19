/* test_pm_shutdown.c — the orderly shutdown sequence's ending
 * (src/kernel/process_manager_shutdown.c).
 *
 * Once a vt owns the console, everything the kernel prints reaches COM1 only
 * when the vt drains its console ring. The sequence's last lines -- a
 * participant's "unmounted clean", "[pm] shutdown: complete" -- are written just
 * before the power goes, so the contract pinned here is that pm_shutdown_step
 * cuts the power only once the console ring has drained, and that a ring which
 * never drains delays it by a bounded time only.
 *
 * Regression: 2026-11-16-shutdown-console-lost. The sequence powered off right
 * after "complete", ahead of the vt: on wasm3_smp COM1 lost every line after
 * "[pm] shutdown: begin" (COM2, written directly, showed "[fs-wfs] unmounted
 * clean" and "complete"), and test_wfs_clean_unmount failed on 6 of 10 CI runs.
 *
 * process_manager_shutdown.c is linked with shutdown_order.c only; the timer,
 * IPC, process table, service list, class registry, app parser, klog, power
 * control and the console-ring query are fakes here.
 */

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "ipc.h"
#include "process.h"
#include "process_manager_internal.h"
#include "service_class_registry.h"
#include "wasmos_app.h"
#include "system_control.h"

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

pm_state_t g_pm;

static uint64_t g_ticks;
static int g_drained;
static int g_drain_queries;
static int g_completes;
static jmp_buf g_power;

enum { POWER_NONE = 0, POWER_OFF = 1, POWER_REBOOT = 2 };

uint64_t timer_ticks(void) {
    return g_ticks;
}

uint64_t timer_ms_to_ticks(uint32_t ms) {
    return ms;
}

int console_ring_drained(void) {
    g_drain_queries++;
    return g_drained;
}

void klog_write(const char* s) {
    if (s && strcmp(s, "[pm] shutdown: complete\n") == 0) {
        g_completes++;
    }
}

int ipc_send_from(uint32_t sender_context_id, uint32_t endpoint, const ipc_message_t* message) {
    (void)sender_context_id;
    (void)endpoint;
    (void)message;
    return IPC_OK;
}

int ipc_endpoint_create(uint32_t owner_context_id, uint32_t* out_endpoint) {
    (void)owner_context_id;
    (void)out_endpoint;
    return IPC_ERR_INVALID;
}

int ipc_endpoint_wait_for(uint32_t receiver_context_id, uint32_t endpoint, uint32_t timeout_ms) {
    (void)receiver_context_id;
    (void)endpoint;
    (void)timeout_ms;
    return IPC_ERR_INVALID;
}

/* No registered services: the sequence has no participants and goes straight
 * to its ending, which is what these cases are about. */
process_t* process_find_by_context(uint32_t context_id) {
    (void)context_id;
    return 0;
}

void* list_first(list_t* list, list_iter_t* iter) {
    (void)list;
    (void)iter;
    return 0;
}

void* list_next(list_iter_t* iter) {
    (void)iter;
    return 0;
}

/* Nothing the dependency order reads exists here: no app slots, no class
 * providers, no image to parse. With no participants it orders nothing. */
process_t* process_get(uint32_t pid) {
    (void)pid;
    return 0;
}

uint32_t service_class_registry_lookup(const char* class_name, service_class_provider_t* out,
                                       uint32_t max) {
    (void)class_name;
    (void)out;
    (void)max;
    return 0;
}

int wasmos_app_parse(const uint8_t* blob, uint32_t blob_size, wasmos_app_desc_t* out_desc) {
    (void)blob;
    (void)blob_size;
    (void)out_desc;
    return -1;
}

void klog_printf(const char* fmt, ...) {
    (void)fmt;
}

__attribute__((noreturn)) void kernel_system_poweroff(void) {
    longjmp(g_power, POWER_OFF);
}

__attribute__((noreturn)) void kernel_system_reboot(void) {
    longjmp(g_power, POWER_REBOOT);
}

static void reset(uint32_t reason) {
    memset(&g_pm, 0, sizeof(g_pm));
    g_ticks = 1000;
    g_drained = 0;
    g_drain_queries = 0;
    g_completes = 0;
    kernel_system_shutdown_arm(reason);
}

/* One PM dispatch: the step, and whether it cut the power. */
static int step(void) {
    int power = setjmp(g_power);
    if (power == POWER_NONE) {
        pm_shutdown_step(1);
    }
    return power;
}

static void s_power_waits_for_the_console_ring_to_drain(void) {
    reset(0);
    CHECK(step() == POWER_NONE, "the step that completes the sequence leaves the power on");
    CHECK(g_drain_queries > 0, "the console ring is asked whether it has drained");
    g_ticks += 50;
    CHECK(step() == POWER_NONE, "the power stays on while the vt has text to write");
    g_drained = 1;
    g_ticks += 50;
    CHECK(step() == POWER_OFF, "the power goes once the ring has drained");
    CHECK(g_completes == 1, "\"complete\" is logged once, not on every waiting step");
}

static void s_a_ring_that_never_drains_delays_power_off_by_a_bound(void) {
    reset(0);
    CHECK(step() == POWER_NONE, "the sequence waits for the ring first");
    g_ticks += 60000ull;
    CHECK(step() == POWER_OFF, "a minute later the power goes regardless");
}

static void s_reboot_waits_for_the_ring_too(void) {
    reset(WASMOS_SHUTDOWN_REASON_REBOOT);
    CHECK(step() == POWER_NONE, "the reboot waits for the ring");
    g_drained = 1;
    g_ticks += 50;
    CHECK(step() == POWER_REBOOT, "and then reboots rather than powering off");
}

static const wasmos_test_void_case_t k_cases[] = {
    {"power_waits_for_the_console_ring_to_drain", s_power_waits_for_the_console_ring_to_drain},
    {"a_ring_that_never_drains_delays_power_off_by_a_bound",
     s_a_ring_that_never_drains_delays_power_off_by_a_bound},
    {"reboot_waits_for_the_ring_too", s_reboot_waits_for_the_ring_too},
};

int main(void) {
    printf("test_pm_shutdown\n");

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_pm_shutdown: FAIL\n");
        return 1;
    }
    printf("test_pm_shutdown: OK\n");
    return 0;
}
