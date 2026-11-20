/* Host unit test for the process manager's spawn capability descriptor parser
 * (src/kernel/process_manager_caps.c): a wasmos_spawn_caps_v2_t header followed
 * by io_range_count I/O windows and then dma.window_count DMA windows, sized
 * exactly by its counts. Also who may put such grants on a child at all
 * (pm_caps_grant_allowed), against a stubbed capability table.
 *
 * Each case returns 0 to pass or __LINE__ to fail, and wasmos_test_run_all stops
 * at the first failure (test_shuffle.h). */

#include "capability.h"
#include "process_manager_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

static uint8_t g_buf[256];
static pm_spawn_caps_t g_caps;

static uint32_t build(uint32_t flags, uint16_t irq_mask, uint16_t io_min, uint16_t io_max,
                      const wasmos_io_range_t* ranges, uint16_t range_count,
                      const wasmos_dma_window_t* windows, uint32_t window_count) {
    wasmos_spawn_caps_v2_t hdr;
    uint32_t off = sizeof(hdr);
    memset(&hdr, 0, sizeof(hdr));
    memset(g_buf, 0, sizeof(g_buf));
    hdr.cap_flags = flags;
    hdr.irq_mask = irq_mask;
    hdr.io_port_min = io_min;
    hdr.io_port_max = io_max;
    hdr.io_range_count = range_count;
    if (window_count > 0u) {
        hdr.dma.direction_flags = WASMOS_DMA_DIR_BIDIR;
        hdr.dma.max_bytes = 4096u;
        hdr.dma.window_count = window_count;
    }
    memcpy(g_buf, &hdr, sizeof(hdr));
    memcpy(g_buf + off, ranges, (size_t)range_count * sizeof(wasmos_io_range_t));
    off += range_count * (uint32_t)sizeof(wasmos_io_range_t);
    memcpy(g_buf + off, windows, (size_t)window_count * sizeof(wasmos_dma_window_t));
    off += window_count * (uint32_t)sizeof(wasmos_dma_window_t);
    return off;
}

/* A bare header with one I/O window and an IRQ line: the legacy-device shape. */
static int test_a_single_window_and_an_irq(void) {
    uint32_t size = build(DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ, 1u << 8, 0x70, 0x71, 0, 0, 0, 0);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != 0) {
        return __LINE__;
    }
    if (!g_caps.valid || g_caps.io_range_count != 1u || g_caps.io_ranges[0].first != 0x70 ||
        g_caps.io_ranges[0].last != 0x71 || g_caps.irq_mask != (1u << 8) ||
        g_caps.dma_window_count != 0u) {
        return __LINE__;
    }
    return 0;
}

/* Disjoint I/O windows without DMA: the i8042 keyboard controller's data and
 * command ports are such a pair.
 *
 * Regression: 2026-11-19-caps-v2-ranges-without-dma -- the parser accepted
 * extra bytes only for a DMA request, so a payload whose I/O ranges made it
 * longer than the bare header was refused with BAD_CAPS. Every in-tree caller
 * with ranges also asked for DMA, so no spawn hit it until a watched spawn
 * carried a keyboard's two windows. */
static int test_disjoint_windows_without_dma(void) {
    const wasmos_io_range_t ranges[2] = {{0x60, 0x60}, {0x64, 0x64}};
    uint32_t size = build(DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ, 1u << 1, 0, 0, ranges, 2, 0, 0);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != 0) {
        return __LINE__;
    }
    if (g_caps.io_range_count != 2u || g_caps.io_ranges[1].first != 0x64) {
        return __LINE__;
    }
    return 0;
}

/* Ranges and DMA windows together, each read from its place in the tail. */
static int test_windows_and_dma(void) {
    const wasmos_io_range_t ranges[1] = {{0x1F0, 0x1F7}};
    const wasmos_dma_window_t windows[1] = {{0x100000, 0x10000}};
    uint32_t size = build(DEVMGR_CAP_IO_PORT | DEVMGR_CAP_DMA, 0, 0, 0, ranges, 1, windows, 1);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != 0) {
        return __LINE__;
    }
    if (g_caps.io_range_count != 1u || g_caps.dma_window_count != 1u ||
        g_caps.dma_windows[0].base != 0x100000 || g_caps.dma_windows[0].length != 0x10000 ||
        g_caps.dma_direction_flags != WASMOS_DMA_DIR_BIDIR) {
        return __LINE__;
    }
    return 0;
}

/* The size must be exactly what the counts imply, one byte either way. */
static int test_the_size_must_match_the_counts(void) {
    const wasmos_io_range_t ranges[2] = {{0x60, 0x60}, {0x64, 0x64}};
    uint32_t size = build(DEVMGR_CAP_IO_PORT, 0, 0, 0, ranges, 2, 0, 0);
    if (pm_caps_from_v2(g_buf, size - 1u, &g_caps) != WASMOS_ERR_PROC_PM_BAD_CAPS ||
        pm_caps_from_v2(g_buf, size + 1u, &g_caps) != WASMOS_ERR_PROC_PM_BAD_CAPS) {
        return __LINE__;
    }
    size = build(DEVMGR_CAP_IO_PORT, 0, 0x70, 0x71, 0, 0, 0, 0);
    if (pm_caps_from_v2(g_buf, size + 1u, &g_caps) != WASMOS_ERR_PROC_PM_BAD_CAPS) {
        return __LINE__;
    }
    return 0;
}

/* Grants that cannot be honoured are refused, never narrowed. */
static int test_bad_grants_are_refused(void) {
    const wasmos_io_range_t inverted[1] = {{0x64, 0x60}};
    uint32_t size = build(1u << 30, 0, 0, 0, 0, 0, 0, 0);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != WASMOS_ERR_PROC_PM_BAD_CAPS) {
        return __LINE__;
    }
    size = build(DEVMGR_CAP_IO_PORT, 0, 0x71, 0x70, 0, 0, 0, 0);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != WASMOS_ERR_PROC_PM_BAD_CAPS) {
        return __LINE__;
    }
    size = build(DEVMGR_CAP_IO_PORT, 0, 0, 0, inverted, 1, 0, 0);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != WASMOS_ERR_PROC_PM_BAD_CAPS) {
        return __LINE__;
    }
    /* DMA asked for with no window. */
    size = build(DEVMGR_CAP_DMA, 0, 0, 0, 0, 0, 0, 0);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != WASMOS_ERR_PROC_PM_BAD_CAPS) {
        return __LINE__;
    }
    if (pm_caps_from_v2(g_buf, sizeof(wasmos_spawn_caps_v2_t) - 1u, &g_caps) !=
        WASMOS_ERR_PROC_PM_BAD_CAPS) {
        return __LINE__;
    }
    return 0;
}

/* Ranges without the io.port bit grant no window. */
static int test_ranges_need_the_io_bit(void) {
    const wasmos_io_range_t ranges[1] = {{0x60, 0x64}};
    uint32_t size = build(DEVMGR_CAP_IRQ, 1u << 1, 0, 0, ranges, 1, 0, 0);
    if (pm_caps_from_v2(g_buf, size, &g_caps) != 0 || g_caps.io_range_count != 0u) {
        return __LINE__;
    }
    return 0;
}

/* The context that holds spawn.grant in the stubbed capability table; every
 * other context holds nothing. */
#define GRANT_HOLDER 7u
#define REQUESTER 9u

int capability_has(uint32_t context_id, capability_kind_t kind) {
    return context_id == GRANT_HOLDER && kind == CAP_SPAWN_GRANT;
}

/* Regression: 2026-11-20-unchecked-spawn-grants -- no caps-carrying spawn
 * checked its requester, so any process could spawn a module with I/O windows,
 * IRQ lines or a DMA window over physical memory of its choosing, authority the
 * module's manifest alone does not give. Every grant-bearing bit is refused to
 * a requester without spawn.grant. */
static int test_a_grant_from_a_requester_without_spawn_grant_is_refused(void) {
    const uint32_t bits[] = {
        DEVMGR_CAP_IO_PORT, DEVMGR_CAP_IRQ, DEVMGR_CAP_MMIO_MAP, DEVMGR_CAP_DMA};
    for (uint32_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        memset(&g_caps, 0, sizeof(g_caps));
        g_caps.valid = 1;
        g_caps.cap_flags = bits[i];
        if (pm_caps_grant_allowed(REQUESTER, &g_caps) != 0) {
            return __LINE__;
        }
    }
    return 0;
}

static int test_a_holder_of_spawn_grant_may_grant(void) {
    memset(&g_caps, 0, sizeof(g_caps));
    g_caps.valid = 1;
    g_caps.cap_flags = DEVMGR_CAP_IO_PORT | DEVMGR_CAP_IRQ | DEVMGR_CAP_DMA;
    return pm_caps_grant_allowed(GRANT_HOLDER, &g_caps) == 1 ? 0 : __LINE__;
}

/* No caps, or a profile with no bits, only narrows the child: fs-manager spawns
 * its backends with PROC_IPC_SPAWN_PATH_CAPS_SYNC and cap_flags 0. */
static int test_a_spawn_without_grants_is_open_to_everyone(void) {
    memset(&g_caps, 0, sizeof(g_caps));
    if (pm_caps_grant_allowed(REQUESTER, &g_caps) != 1) {
        return __LINE__;
    }
    g_caps.valid = 1;
    if (pm_caps_grant_allowed(REQUESTER, &g_caps) != 1) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_single_window_and_an_irq),
        WASMOS_TEST_CASE(test_disjoint_windows_without_dma),
        WASMOS_TEST_CASE(test_windows_and_dma),
        WASMOS_TEST_CASE(test_the_size_must_match_the_counts),
        WASMOS_TEST_CASE(test_bad_grants_are_refused),
        WASMOS_TEST_CASE(test_ranges_need_the_io_bit),
        WASMOS_TEST_CASE(test_a_grant_from_a_requester_without_spawn_grant_is_refused),
        WASMOS_TEST_CASE(test_a_holder_of_spawn_grant_may_grant),
        WASMOS_TEST_CASE(test_a_spawn_without_grants_is_open_to_everyone),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_pm_caps: ok\n");
    return 0;
}
