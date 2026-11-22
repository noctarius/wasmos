/* Host unit test for the kernel mappings of device register windows
 * (src/kernel/device_window.c), against stubbed page tables and a stubbed RAM
 * map: which ranges are refused, that a window is mapped uncached, that a range
 * inside an existing window reuses it, and that a failed mapping leaves nothing
 * behind.
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include "device_window.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arch/vm_layout.h"
#include "memory.h"
#include "test_shuffle.h"

/* System RAM in the stubbed memory map. */
#define RAM_BASE 0x100000ull
#define RAM_END 0x20000000ull

/* The stubbed page table: one slot per page of the first 16 MiB of the
 * device-window span, which is more than every case together maps. */
static uint64_t g_phys[4096];
static uint64_t g_flags[4096];
static uint8_t g_mapped[4096];
static uint32_t g_maps_until_failure; /* 0 = never fail */
static uint32_t g_map_calls;

int pfa_range_overlaps_ram(uint64_t base, uint64_t length) {
    return base < RAM_END && base + length > RAM_BASE;
}

static int span_slot(uint64_t virt, uint64_t* out) {
    if (virt < KERNEL_MMIO_DEVICE_VA || (virt - KERNEL_MMIO_DEVICE_VA) / 0x1000ull >= 4096u) {
        return -1;
    }
    *out = (virt - KERNEL_MMIO_DEVICE_VA) / 0x1000ull;
    return 0;
}

int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t slot = 0;
    g_map_calls++;
    if (g_maps_until_failure != 0u && g_map_calls >= g_maps_until_failure) {
        return -1;
    }
    if (span_slot(virt, &slot) != 0) {
        return -1;
    }
    g_phys[slot] = phys;
    g_flags[slot] = flags;
    g_mapped[slot] = 1;
    return 0;
}

int paging_unmap_4k(uint64_t virt) {
    uint64_t slot = 0;
    if (span_slot(virt, &slot) != 0) {
        return -1;
    }
    g_mapped[slot] = 0;
    return 0;
}

void paging_invalidate(uint64_t virt) {
    (void)virt;
}

void klog_printf(const char* fmt, ...) {
    (void)fmt;
}

static uint32_t mapped_pages(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4096u; ++i) {
        n += g_mapped[i];
    }
    return n;
}

/* The window table is process-global and only grows, so each case starts with
 * a fresh view of the stubs and asserts on what IT added. */
static void reset_stubs(void) {
    g_maps_until_failure = 0;
    g_map_calls = 0;
}

static int test_a_window_is_mapped_uncached_at_its_offset(void) {
    uint64_t kva = 0;
    uint32_t before = mapped_pages();
    reset_stubs();
    if (device_window_map(0xFEB00000ull, 0x4000ull, &kva) != 0) {
        return __LINE__;
    }
    if (kva < KERNEL_MMIO_DEVICE_VA ||
        kva + 0x4000ull > KERNEL_MMIO_DEVICE_VA + KERNEL_MMIO_DEVICE_BYTES ||
        mapped_pages() != before + 4u) {
        return __LINE__;
    }
    uint64_t slot = (kva - KERNEL_MMIO_DEVICE_VA) / 0x1000ull;
    for (uint64_t i = 0; i < 4u; ++i) {
        if (g_phys[slot + i] != 0xFEB00000ull + i * 0x1000ull ||
            (g_flags[slot + i] & MEM_REGION_FLAG_DEVICE) == 0u ||
            (g_flags[slot + i] & MEM_REGION_FLAG_USER) != 0u) {
            return __LINE__;
        }
    }
    return 0;
}

/* A restarted driver asks for the same window again; it gets the mapping that
 * already exists, so the translation of a VA never changes. */
static int test_a_range_inside_a_window_reuses_it(void) {
    uint64_t first = 0;
    uint64_t again = 0;
    uint64_t inner = 0;
    reset_stubs();
    if (device_window_map(0xFEC10000ull, 0x2000ull, &first) != 0) {
        return __LINE__;
    }
    uint32_t pages = mapped_pages();
    if (device_window_map(0xFEC10000ull, 0x2000ull, &again) != 0 ||
        device_window_map(0xFEC11000ull, 0x1000ull, &inner) != 0) {
        return __LINE__;
    }
    if (again != first || inner != first + 0x1000ull || mapped_pages() != pages) {
        return __LINE__;
    }
    return 0;
}

/* A grant can never become a window onto kernel memory. */
static int test_ram_and_malformed_ranges_are_refused(void) {
    uint64_t kva = 0;
    uint32_t pages = mapped_pages();
    reset_stubs();
    if (device_window_map(RAM_BASE, 0x1000ull, &kva) == 0 ||
        device_window_map(RAM_END - 0x1000ull, 0x2000ull, &kva) == 0 ||
        device_window_map(0xFEB00800ull, 0x1000ull, &kva) == 0 ||
        device_window_map(0xFEB00000ull, 0x800ull, &kva) == 0 ||
        device_window_map(0xFEB00000ull, 0, &kva) == 0 ||
        device_window_map(0, 0x1000ull, &kva) == 0 ||
        device_window_map(0xFFFFFFFFFFFFF000ull, 0x2000ull, &kva) == 0 ||
        device_window_map(0xF0000000ull, DEVICE_WINDOW_MAX_BYTES + 0x1000ull, &kva) == 0 ||
        device_window_map(0xFEB00000ull, 0x1000ull, 0) == 0) {
        return __LINE__;
    }
    return mapped_pages() == pages ? 0 : __LINE__;
}

/* A page-table update that fails part-way leaves none of the window mapped. */
static int test_a_failed_mapping_leaves_nothing_behind(void) {
    uint64_t kva = 0;
    uint32_t pages = mapped_pages();
    reset_stubs();
    g_maps_until_failure = 3;
    if (device_window_map(0xFED00000ull, 0x4000ull, &kva) == 0) {
        return __LINE__;
    }
    return mapped_pages() == pages ? 0 : __LINE__;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_window_is_mapped_uncached_at_its_offset),
        WASMOS_TEST_CASE(test_a_range_inside_a_window_reuses_it),
        WASMOS_TEST_CASE(test_ram_and_malformed_ranges_are_refused),
        WASMOS_TEST_CASE(test_a_failed_mapping_leaves_nothing_behind),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_device_window: ok\n");
    return 0;
}
