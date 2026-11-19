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
 * The participants are then put in dependency order, so the last case pins that
 * every provider of a needed class is an edge, however many there are.
 *
 * process_manager_shutdown.c is linked with shutdown_order.c only; the timer,
 * IPC, process table, service list, class registry, app parser, klog, power
 * control and the console-ring query are fakes here. They hold nothing unless a
 * case installs the dependency scenario (g_deps).
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

/* The dependency scenario: two shutdown participants, "fs" (pid 10) and "disk"
 * (pid 20). fs NEEDS class "blk", whose providers are eight processes that are
 * not in the order at all and, ninth, disk. With no scenario installed there
 * are no services, no app slots and no class providers. */
#define DEP_FS_PID 10u
#define DEP_DISK_PID 20u
#define DEP_FS_EP 210u
#define DEP_DISK_EP 220u
#define DEP_PROVIDERS 9u

static int g_deps;
static process_t g_procs[2];
static pm_service_entry_t g_services[2];
static pm_app_state_t g_apps[2];
static const uint8_t k_fs_blob[1] = {1};
static const uint8_t k_disk_blob[1] = {2};
static const uint8_t k_blk[] = {'b', 'l', 'k'};

static void install_deps(void) {
    memset(g_procs, 0, sizeof(g_procs));
    memset(g_services, 0, sizeof(g_services));
    memset(g_apps, 0, sizeof(g_apps));
    g_procs[0].pid = DEP_FS_PID;
    g_procs[0].context_id = 110u;
    g_procs[0].state = PROCESS_STATE_ALIVE;
    g_procs[1].pid = DEP_DISK_PID;
    g_procs[1].context_id = 120u;
    g_procs[1].state = PROCESS_STATE_ALIVE;
    for (uint32_t i = 0; i < 2u; ++i) {
        g_services[i].in_use = 1;
        g_services[i].owner_context_id = g_procs[i].context_id;
        g_services[i].flags = WASMOS_SVC_FLAG_WANTS_SHUTDOWN;
        g_apps[i].in_use = 1;
        g_apps[i].pid = g_procs[i].pid;
        g_apps[i].blob_size = 1;
    }
    g_services[0].endpoint = DEP_FS_EP;
    g_services[1].endpoint = DEP_DISK_EP;
    g_apps[0].blob = k_fs_blob;
    g_apps[1].blob = k_disk_blob;
    g_deps = 1;
}

process_t* process_find_by_context(uint32_t context_id) {
    for (uint32_t i = 0; g_deps && i < 2u; ++i) {
        if (g_procs[i].context_id == context_id) {
            return &g_procs[i];
        }
    }
    return 0;
}

process_t* process_get(uint32_t pid) {
    for (uint32_t i = 0; g_deps && i < 2u; ++i) {
        if (g_procs[i].pid == pid) {
            return &g_procs[i];
        }
    }
    return 0;
}

/* The service list and the app slots, each two entries long under g_deps. */
static void* list_at(list_iter_t* iter) {
    if (!g_deps || iter->index >= 2u) {
        return 0;
    }
    if (iter->list == &g_pm.services) {
        return &g_services[iter->index];
    }
    if (iter->list == &g_pm.apps) {
        return &g_apps[iter->index];
    }
    return 0;
}

void* list_first(list_t* list, list_iter_t* iter) {
    iter->list = list;
    iter->index = 0;
    return list_at(iter);
}

void* list_next(list_iter_t* iter) {
    iter->index++;
    return list_at(iter);
}

/* Provider i of "blk" is pid 31 + i, except the last, which is disk. */
static uint32_t blk_provider_pid(uint32_t i) {
    return i + 1u < DEP_PROVIDERS ? 31u + i : DEP_DISK_PID;
}

int service_class_registry_provided_by(const char* class_name, uint32_t pid) {
    if (!g_deps || strcmp(class_name, "blk") != 0) {
        return 0;
    }
    for (uint32_t i = 0; i < DEP_PROVIDERS; ++i) {
        if (blk_provider_pid(i) == pid) {
            return 1;
        }
    }
    return 0;
}

int wasmos_app_parse(const uint8_t* blob, uint32_t blob_size, wasmos_app_desc_t* out_desc) {
    (void)blob_size;
    if (!g_deps) {
        return -1;
    }
    memset(out_desc, 0, sizeof(*out_desc));
    if (blob == k_fs_blob) {
        out_desc->dep_count = 1;
        out_desc->deps[0].strength = WASMOS_APP_DEP_NEEDS;
        out_desc->deps[0].kind = WASMOS_APP_DEP_KIND_CLASS;
        out_desc->deps[0].name = k_blk;
        out_desc->deps[0].name_len = (uint32_t)sizeof(k_blk);
    }
    return 0;
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
    g_deps = 0;
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

/* Regression: 2026-11-19-shutdown-class-provider-cap -- a class need became an
 * edge only to the first eight providers the registry wrote, so a participant
 * providing the class as the ninth instance (or behind eight instances held by
 * one process) was ordered as if nothing needed it: by descending pid, ahead of
 * the filesystem that still writes through it. */
static void s_every_provider_of_a_needed_class_is_ordered_after_its_dependent(void) {
    reset(0);
    install_deps();
    CHECK(step() == POWER_NONE, "the first step collects, orders and notifies");
    CHECK(g_pm.shutdown.count == 2u, "both participants are collected");
    CHECK(g_pm.shutdown.endpoints[0] == DEP_FS_EP,
          "fs, which needs class blk, is told before the ninth provider of blk");
    CHECK(g_pm.shutdown.endpoints[1] == DEP_DISK_EP, "the provider goes after it");
}

static const wasmos_test_void_case_t k_cases[] = {
    {"power_waits_for_the_console_ring_to_drain", s_power_waits_for_the_console_ring_to_drain},
    {"a_ring_that_never_drains_delays_power_off_by_a_bound",
     s_a_ring_that_never_drains_delays_power_off_by_a_bound},
    {"reboot_waits_for_the_ring_too", s_reboot_waits_for_the_ring_too},
    {"every_provider_of_a_needed_class_is_ordered_after_its_dependent",
     s_every_provider_of_a_needed_class_is_ordered_after_its_dependent},
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
