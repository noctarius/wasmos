/* test_physmem.c — the physical frame allocator (src/kernel/physmem.c).
 *
 * The allocator keeps free memory as a sorted list of extents. The contract a
 * caller relies on is conservation: every frame handed out and freed again is
 * free again, however the frees interleave, so a run of contiguous frames that
 * was available before a burst of small allocations is available after it.
 * These cases fragment a fake memory map one frame at a time and check that
 * contract by the free byte count and by a contiguous allocation spanning all
 * of it.
 *
 * pfa_init runs before paging_init, while the higher half maps only the
 * bootstrap window (entry.S: 64 MiB), so the tables it builds for itself must
 * sit inside it. Two cases boot a map with RAM above the window in a child
 * process; the host stand-in for the higher-half alias ends the child with
 * TOUCHED_UNMAPPED where the kernel would fault.
 *
 * Regression: 2026-11-16-pfa-dropped-free. A free that needed a new extent
 * while the extent table was full was discarded: its frames left the allocator
 * for good. Under a busy boot (the logger's one-page transfer buffer per write
 * among long-lived allocations) the table filled, the losses accumulated, and
 * the process manager's 2 MiB contiguous spawn buffer stopped fitting —
 * every later exec failed with "transfer buffer unavailable" (CI boot-and-init,
 * warp_smp and wasm3_smp, test_logger).
 *
 * src/kernel/physmem.c is linked in whole, built with WASMOS_PHYSMEM_TEST_SEAMS
 * so the frames it keeps its own tables in are host memory; klog is silenced
 * here and kpanic aborts (stubs_kpanic.c).
 */

#define _POSIX_C_SOURCE 200809L /* fork, waitpid under -std=c11 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_shuffle.h"

#include "boot.h"
#include "physmem.h"

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

#define PAGE 4096ull
/* One conventional-memory extent, inside the allocator's static 64 MiB refcount
 * window and clear of page 0, the AP trampoline page and the (host-address)
 * kernel image reservation. */
#define MAP_BASE 0x100000ull
#define MAP_PAGES 4096ull

/* physmem.c reserves [__kernel_start, __kernel_end) translated from the higher
 * half; on the host that lands far outside MAP_BASE, so it reserves nothing. */
uint8_t __kernel_start;
uint8_t __kernel_end;

/* Host memory standing in for the bootstrap higher-half window: the allocator
 * writes its own tables into frames it takes from the map. */
#define WINDOW_BYTES (64ull * 1024 * 1024)
#define TOUCHED_UNMAPPED 42
static uint8_t* g_backing;

void* physmem_test_phys_ptr(uint64_t phys) {
    if (phys >= WINDOW_BYTES) {
        printf("  the allocator dereferenced 0x%llx, above the bootstrap window\n",
               (unsigned long long)phys);
        fflush(stdout);
        _exit(TOUCHED_UNMAPPED);
    }
    return g_backing + phys;
}

void klog_write(const char* s) {
    (void)s;
}

void klog_printf(const char* fmt, ...) {
    (void)fmt;
}

typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} efi_desc_t;

static efi_desc_t g_map[2];
static boot_info_t g_boot;
static uint64_t g_frames[MAP_PAGES];

/* Re-initialises the allocator over the single extent. Frames from a previous
 * case keep their refcount, which allocation overwrites; the cases compare
 * against the free byte count measured here, not against pfa_total_bytes. */
static uint64_t reset(void) {
    memset(&g_boot, 0, sizeof(g_boot));
    g_map[0].Type = 7; /* EfiConventionalMemory */
    g_map[0].PhysicalStart = MAP_BASE;
    g_map[0].NumberOfPages = MAP_PAGES;
    g_boot.memory_map = g_map;
    g_boot.memory_map_size = sizeof(g_map[0]);
    g_boot.memory_desc_size = sizeof(g_map[0]);
    pfa_init(&g_boot);
    return pfa_free_bytes();
}

/* RAM split across the window: `low_pages` at 1 MiB and 16 MiB at 128 MiB.
 * Boots it in a child (the refcount array it installs outlives pfa_init) and
 * returns the child's wait status. With `allocate`, the child also takes and
 * frees one frame from the high extent before exiting 0. */
static int boot_split_map(uint64_t low_pages, int allocate) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        memset(&g_boot, 0, sizeof(g_boot));
        g_map[0].Type = 7;
        g_map[0].PhysicalStart = MAP_BASE;
        g_map[0].NumberOfPages = low_pages;
        g_map[1].Type = 7;
        g_map[1].PhysicalStart = 128ull * 1024 * 1024;
        g_map[1].NumberOfPages = 4096;
        g_boot.memory_map = g_map;
        g_boot.memory_map_size = sizeof(g_map);
        g_boot.memory_desc_size = sizeof(g_map[0]);
        pfa_init(&g_boot);
        if (allocate) {
            uint64_t frame = pfa_alloc_pages_above(1, 128ull * 1024 * 1024);
            if (frame == 0) {
                _exit(1);
            }
            pfa_free_pages(frame, 1);
        }
        _exit(0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return status;
}

/* Takes every free frame one at a time, in address order. */
static uint64_t take_all_frames(void) {
    uint64_t n = 0;
    while (n < MAP_PAGES) {
        uint64_t frame = pfa_alloc_pages(1);
        if (frame == 0) {
            break;
        }
        g_frames[n++] = frame;
    }
    return n;
}

static void free_every_other(uint64_t n, uint64_t first) {
    for (uint64_t i = first; i < n; i += 2) {
        pfa_free_pages(g_frames[i], 1);
    }
}

static void s_a_checkerboard_of_frees_is_all_free(void) {
    uint64_t baseline = reset();
    uint64_t n = take_all_frames();
    CHECK(n * PAGE == baseline, "every free frame is handed out one at a time");
    CHECK(pfa_free_bytes() == 0, "nothing is free once every frame is taken");

    /* Every second frame: each free is its own extent, far more than the
     * allocator has ever needed at boot. */
    free_every_other(n, 0);
    CHECK(pfa_free_bytes() == ((n + 1) / 2) * PAGE, "every freed frame is counted free");
}

static void s_fragmented_memory_freed_is_contiguous_again(void) {
    uint64_t baseline = reset();
    uint64_t n = take_all_frames();

    free_every_other(n, 0);
    free_every_other(n, 1);
    CHECK(pfa_free_bytes() == baseline, "after the churn the free count is back at baseline");

    /* The spawn buffer case: the whole extent is one contiguous run again. */
    uint64_t run = pfa_alloc_pages(baseline / PAGE);
    CHECK(run != 0, "one allocation spans every frame that was fragmented and freed");
    if (run != 0) {
        pfa_free_pages(run, baseline / PAGE);
    }
}

static void s_a_frame_freed_into_a_fragmented_list_can_be_allocated(void) {
    uint64_t baseline = reset();
    uint64_t n = take_all_frames();
    (void)baseline;

    free_every_other(n, 0);
    /* Each frame freed in the second pass merges two neighbours; none of them
     * may be lost, so 2 MiB (the process manager's spawn buffer) fits. */
    for (uint64_t i = 1; i < 1024 && i < n; i += 2) {
        pfa_free_pages(g_frames[i], 1);
    }
    uint64_t spawn = pfa_alloc_pages(512);
    CHECK(spawn != 0, "a 2 MiB contiguous run fits once its frames are all freed");
}

static void s_an_above_allocation_that_splits_an_extent_keeps_both_remnants(void) {
    (void)reset();
    uint64_t n = take_all_frames();

    /* One contiguous block at the top, then the low half fragmented. */
    for (uint64_t i = n - 64; i < n; ++i) {
        pfa_free_pages(g_frames[i], 1);
    }
    for (uint64_t i = 0; i < n / 2; i += 2) {
        pfa_free_pages(g_frames[i], 1);
    }
    uint64_t before = pfa_free_bytes();

    /* A floor in the middle of the block carves its middle: the block splits. */
    uint64_t frame = pfa_alloc_pages_above(1, g_frames[n - 32]);
    CHECK(frame == g_frames[n - 32], "the run starts at the floor");
    CHECK(pfa_free_bytes() == before - PAGE, "only the allocated frame leaves the free list");
    if (frame != 0) {
        pfa_free_pages(frame, 1);
    }
    CHECK(pfa_free_bytes() == before, "freeing it restores the block");
}

/* A ceiling keeps the whole run below it: a run that fits only above the
 * ceiling is refused rather than placed there, and one that fits between floor
 * and ceiling is carved there even when the extent runs on past the ceiling.
 * Regression: 2026-11-28-warp-alias-ceiling -- WARP reaches its frames at
 * phys | KERNEL_HIGHER_HALF_BASE, valid only below the direct-map window, but
 * allocated them with a floor alone, so once memory below the window ran out a
 * frame above it aliased the wrong memory. */
static void s_a_between_allocation_stays_below_its_ceiling(void) {
    (void)reset();
    const uint64_t floor = MAP_BASE + 16 * PAGE;
    const uint64_t ceiling = MAP_BASE + 32 * PAGE;

    uint64_t frame = pfa_alloc_pages_between(4, floor, ceiling);
    CHECK(frame == floor, "the run starts at the floor");
    CHECK(frame + 4 * PAGE <= ceiling, "the run ends at or below the ceiling");

    uint64_t too_big = pfa_alloc_pages_between(16, floor, ceiling);
    CHECK(too_big == 0, "a run that would cross the ceiling is refused");

    uint64_t exact = pfa_alloc_pages_between(12, floor, ceiling);
    CHECK(exact == floor + 4 * PAGE, "the rest of the window is still usable");
    CHECK(pfa_alloc_pages_between(1, floor, ceiling) == 0, "a full window refuses");
    CHECK(pfa_alloc_pages_above(1, floor) == ceiling, "the floor alone goes on past it");
    reset();
}

static void s_boot_tables_land_inside_the_bootstrap_window(void) {
    int status = boot_split_map(64, 1);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) != TOUCHED_UNMAPPED,
          "pfa_init touches no frame above the bootstrap window");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the allocator then serves and takes back a frame above it");
}

static void s_a_window_too_small_for_the_boot_tables_panics(void) {
    /* Four low pages hold neither the refcount array nor the extent table; the
     * high extent holds both but has no alias yet. */
    int status = boot_split_map(4, 0);
    CHECK(!(WIFEXITED(status) && WEXITSTATUS(status) == TOUCHED_UNMAPPED),
          "pfa_init does not place a boot table above the bootstrap window");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "pfa_init panics when the window cannot hold its tables");
}

/* Firmware ACPI extents decide what wasmos_phys_map may map into a guest: RAM
 * and device MMIO must never pass, and a range only partly inside an ACPI
 * extent must not either. */
static void s_only_ranges_inside_firmware_acpi_memory_are_firmware_tables(void) {
    static efi_desc_t map[4];
    const uint64_t acpi = MAP_BASE + MAP_PAGES * 0x1000ull;
    memset(&g_boot, 0, sizeof(g_boot));
    memset(map, 0, sizeof(map));
    map[0].Type = 7; /* EfiConventionalMemory */
    map[0].PhysicalStart = MAP_BASE;
    map[0].NumberOfPages = MAP_PAGES;
    map[1].Type = 9; /* EfiACPIReclaimMemory */
    map[1].PhysicalStart = acpi;
    map[1].NumberOfPages = 2;
    map[2].Type = 10; /* EfiACPIMemoryNVS, adjacent */
    map[2].PhysicalStart = acpi + 0x2000;
    map[2].NumberOfPages = 1;
    map[3].Type = 11; /* EfiMemoryMappedIO */
    map[3].PhysicalStart = 0xFEC00000ull;
    map[3].NumberOfPages = 1;
    g_boot.memory_map = map;
    g_boot.memory_map_size = sizeof(map);
    g_boot.memory_desc_size = sizeof(map[0]);
    pfa_init(&g_boot);

    CHECK(pfa_range_is_firmware_table(acpi + 0x24, 0x100) == 1,
          "an unaligned range inside a reclaim extent is a firmware table");
    CHECK(pfa_range_is_firmware_table(acpi + 0x1F00, 0x1000) == 1,
          "a range crossing from reclaim into adjacent NVS is a firmware table");
    CHECK(pfa_range_is_firmware_table(MAP_BASE, 0x1000) == 0, "RAM is not a firmware table");
    CHECK(pfa_range_is_firmware_table(acpi - 0x10, 0x20) == 0,
          "a range reaching back into RAM is not a firmware table");
    CHECK(pfa_range_is_firmware_table(acpi + 0x2F00, 0x200) == 0,
          "a range running past the last ACPI page is not a firmware table");
    CHECK(pfa_range_is_firmware_table(0xFEC00000ull, 0x1000) == 0,
          "device MMIO is not a firmware table");
    CHECK(pfa_range_is_firmware_table(acpi, 0) == 0, "an empty range is refused");
    CHECK(pfa_range_is_firmware_table(~0ull - 0x10, 0x100) == 0, "a wrapping range is refused");
    reset();
}

static const wasmos_test_void_case_t k_cases[] = {
    {"a_checkerboard_of_frees_is_all_free", s_a_checkerboard_of_frees_is_all_free},
    {"fragmented_memory_freed_is_contiguous_again", s_fragmented_memory_freed_is_contiguous_again},
    {"a_frame_freed_into_a_fragmented_list_can_be_allocated",
     s_a_frame_freed_into_a_fragmented_list_can_be_allocated},
    {"an_above_allocation_that_splits_an_extent_keeps_both_remnants",
     s_an_above_allocation_that_splits_an_extent_keeps_both_remnants},
    {"a_between_allocation_stays_below_its_ceiling",
     s_a_between_allocation_stays_below_its_ceiling},
    {"boot_tables_land_inside_the_bootstrap_window",
     s_boot_tables_land_inside_the_bootstrap_window},
    {"a_window_too_small_for_the_boot_tables_panics",
     s_a_window_too_small_for_the_boot_tables_panics},
    {"only_ranges_inside_firmware_acpi_memory_are_firmware_tables",
     s_only_ranges_inside_firmware_acpi_memory_are_firmware_tables},
};

int main(void) {
    printf("test_physmem\n");
    g_backing = calloc(1, WINDOW_BYTES);
    if (!g_backing) {
        printf("test_physmem: no host memory for the map\n");
        return 1;
    }

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_physmem: FAIL\n");
        return 1;
    }
    printf("test_physmem: OK\n");
    return 0;
}
