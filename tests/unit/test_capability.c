/* Host unit test for the per-context capability records
 * (src/kernel/capability.c): what a reaped context leaves behind, and how a
 * spawn profile's register windows resolve a driver's (region, offset).
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include "capability.h"

#include <pthread.h>
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

#define DRIVER 60u
#define MMIO_KVA 0xFFFFFFFF60000000ull

/* An I/O window, then a memory-mapped one, then a second I/O window: the
 * profile resolves each by its position, whatever the kinds around it. */
static int install_mixed_profile(void) {
    const capability_region_t regions[3] = {
        {WASMOS_REGION_GRANT_IO, 8, 0x1F0, 0},
        {WASMOS_REGION_GRANT_MMIO, 0x4000, 0xFEB00000u, MMIO_KVA},
        {WASMOS_REGION_GRANT_IO, 1, 0x3F6, 0},
    };
    capability_init();
    return capability_set_spawn_profile(
        DRIVER, DEVMGR_CAP_IO_PORT | DEVMGR_CAP_MMIO_MAP, 3, regions, 0, 0, 0, 0, 0, 0);
}

static int test_regions_resolve_by_position_and_kind(void) {
    capability_region_access_t a;
    if (install_mixed_profile() != 0) {
        return __LINE__;
    }
    if (capability_region_resolve(DRIVER, 0, 7, 1, &a) != 0 || a.kind != WASMOS_REGION_GRANT_IO ||
        a.where != 0x1F7) {
        return __LINE__;
    }
    if (capability_region_resolve(DRIVER, 1, 0x3FFC, 4, &a) != 0 ||
        a.kind != WASMOS_REGION_GRANT_MMIO || a.where != MMIO_KVA + 0x3FFC) {
        return __LINE__;
    }
    if (capability_region_resolve(DRIVER, 2, 0, 1, &a) != 0 || a.kind != WASMOS_REGION_GRANT_IO ||
        a.where != 0x3F6) {
        return __LINE__;
    }
    /* Only an I/O window answers the raw-port question. */
    if (!capability_io_port_allowed(DRIVER, 0x1F0) || !capability_io_port_allowed(DRIVER, 0x3F6) ||
        capability_io_port_allowed(DRIVER, 0x1F8)) {
        return __LINE__;
    }
    return 0;
}

/* Each refusal names its reason. */
static int test_region_refusals_name_their_reason(void) {
    capability_region_access_t a;
    if (install_mixed_profile() != 0) {
        return __LINE__;
    }
    if (capability_region_resolve(DRIVER, 3, 0, 1, &a) != WASMOS_ERR_IO_BAD_REGION) {
        return __LINE__;
    }
    /* The whole access must fit, not only its first byte. */
    if (capability_region_resolve(DRIVER, 0, 6, 4, &a) != WASMOS_ERR_IO_OUT_OF_WINDOW ||
        capability_region_resolve(DRIVER, 1, 0x4000, 1, &a) != WASMOS_ERR_IO_OUT_OF_WINDOW ||
        capability_region_resolve(DRIVER, 1, 0, 3, &a) != WASMOS_ERR_IO_OUT_OF_WINDOW) {
        return __LINE__;
    }
    /* A device register is accessed at its natural alignment; a port is not. */
    if (capability_region_resolve(DRIVER, 1, 2, 4, &a) != WASMOS_ERR_IO_MISALIGNED ||
        capability_region_resolve(DRIVER, 0, 1, 2, &a) != 0) {
        return __LINE__;
    }
    if (capability_region_resolve(DRIVER + 1u, 0, 0, 1, &a) != WASMOS_ERR_IO_NOT_AUTHORIZED) {
        return __LINE__;
    }
    return 0;
}

/* A profile the kernel could not honour exactly is refused whole. */
static int test_a_region_the_kernel_cannot_honour_refuses_the_profile(void) {
    const capability_region_t unmapped[1] = {{WASMOS_REGION_GRANT_MMIO, 0x1000, 0xFEB00000u, 0}};
    const capability_region_t mmio[1] = {{WASMOS_REGION_GRANT_MMIO, 0x1000, 0xFEB00000u, MMIO_KVA}};
    const capability_region_t io[1] = {{WASMOS_REGION_GRANT_IO, 0x20, 0xFFF0, 0}};
    capability_init();
    if (capability_set_spawn_profile(DRIVER, DEVMGR_CAP_MMIO_MAP, 1, unmapped, 0, 0, 0, 0, 0, 0) ==
            0 ||
        capability_set_spawn_profile(DRIVER, DEVMGR_CAP_IO_PORT, 1, mmio, 0, 0, 0, 0, 0, 0) == 0 ||
        capability_set_spawn_profile(DRIVER, DEVMGR_CAP_IO_PORT, 1, io, 0, 0, 0, 0, 0, 0) == 0 ||
        capability_set_spawn_profile(
            DRIVER, DEVMGR_CAP_IO_PORT, CAPABILITY_REGION_LIMIT + 1u, io, 0, 0, 0, 0, 0, 0) == 0) {
        return __LINE__;
    }
    return 0;
}

#define SOAK_THREADS 4u
#define SOAK_ROUNDS 20000u

static uint32_t g_soak_lost;

/* One "CPU": grants a context of its own, checks it, and releases it, over and
 * over. Context ids are disjoint between threads, so any answer other than
 * "granted" while the context is live means another CPU's record took or
 * clobbered this one's slot. */
static void* soak_cpu(void* arg) {
    uint32_t base = 0x1000u + (uint32_t)(uintptr_t)arg * 0x100000u;
    for (uint32_t i = 0; i < SOAK_ROUNDS; ++i) {
        uint32_t ctx = base + i;
        if (grant(ctx, "irq.route") != 0 || !capability_has(ctx, CAP_IRQ_ROUTE)) {
            __atomic_fetch_add(&g_soak_lost, 1u, __ATOMIC_RELAXED);
        }
        capability_release_context(ctx);
    }
    return NULL;
}

/* Regression: 2026-11-25-capability-table-unlocked -- the capability table had
 * no lock, while grants and releases (spawn and reap, from the process manager
 * and the reaping CPU) and checks (every hardware host call) run on any CPU.
 * Two concurrent grants could claim the same free slot (list_alloc's
 * test-then-set), and a release zeroed a record another CPU was reading, so a
 * driver's own grant could vanish or answer for a different context. */
static int test_concurrent_grants_keep_their_own_records(void) {
    pthread_t cpus[SOAK_THREADS];
    capability_init();
    g_soak_lost = 0;
    for (uint32_t i = 0; i < SOAK_THREADS; ++i) {
        if (pthread_create(&cpus[i], NULL, soak_cpu, (void*)(uintptr_t)i) != 0) {
            return __LINE__;
        }
    }
    for (uint32_t i = 0; i < SOAK_THREADS; ++i) {
        pthread_join(cpus[i], NULL);
    }
    if (g_soak_lost != 0) {
        printf("  %u grants lost or misattributed\n", (unsigned)g_soak_lost);
        return __LINE__;
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_released_context_holds_nothing),
        WASMOS_TEST_CASE(test_releasing_one_context_leaves_the_others),
        WASMOS_TEST_CASE(test_regions_resolve_by_position_and_kind),
        WASMOS_TEST_CASE(test_region_refusals_name_their_reason),
        WASMOS_TEST_CASE(test_a_region_the_kernel_cannot_honour_refuses_the_profile),
        WASMOS_TEST_CASE(test_concurrent_grants_keep_their_own_records),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_capability: ok\n");
    return 0;
}
