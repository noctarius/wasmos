/* Host unit test for framebuffer_set_geometry (src/kernel/framebuffer.c): the
 * geometry a framebuffer driver records after reprogramming its adapter, which
 * every later framebuffer_get_info reports.
 *
 * Each case returns 0 to pass or __LINE__ to fail (test_shuffle.h). */
#include "framebuffer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

/* framebuffer.c's kernel dependencies: the log, the higher-half switch (never
 * set on the host, so globals are addressed directly) and the page mapper the
 * tests never reach. */
void klog_printf(const char* fmt, ...) {
    (void)fmt;
}

uint8_t serial_high_alias_enabled(void) {
    return 0;
}

int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)virt;
    (void)phys;
    (void)flags;
    return -1;
}

#define FB_BYTES (1024ull * 768ull * 4ull)

/* A recorded 1024x768 framebuffer of exactly 3 MiB, as edk2 hands over ramfb. */
static void record_boot_framebuffer(void) {
    boot_info_t info;
    memset(&info, 0, sizeof(info));
    info.flags = BOOT_INFO_FLAG_GOP_PRESENT;
    info.framebuffer_base = (void*)(uintptr_t)0x80000000ull;
    info.framebuffer_size = FB_BYTES;
    info.framebuffer_width = 1024;
    info.framebuffer_height = 768;
    info.framebuffer_pixels_per_scanline = 1024;
    framebuffer_init(&info);
}

static int geometry_is(uint32_t width, uint32_t height, uint32_t stride) {
    framebuffer_info_t fb;
    return framebuffer_get_info(&fb) == WASMOS_OK && fb.framebuffer_width == width &&
           fb.framebuffer_height == height && fb.framebuffer_stride == stride &&
           fb.framebuffer_size == FB_BYTES;
}

static int test_a_geometry_that_fits_is_recorded(void) {
    record_boot_framebuffer();
    if (framebuffer_set_geometry(640, 480, 640) != WASMOS_OK || !geometry_is(640, 480, 640)) {
        return __LINE__;
    }
    if (framebuffer_set_geometry(1024, 768, 1024) != WASMOS_OK || !geometry_is(1024, 768, 1024)) {
        return __LINE__;
    }
    return 0;
}

static int test_a_geometry_past_the_recorded_size_is_refused(void) {
    record_boot_framebuffer();
    if (framebuffer_set_geometry(1280, 800, 1280) != WASMOS_ERR_FRAMEBUFFER_GEOMETRY_TOO_LARGE ||
        framebuffer_set_geometry(1024, 769, 1024) != WASMOS_ERR_FRAMEBUFFER_GEOMETRY_TOO_LARGE ||
        framebuffer_set_geometry(0, 480, 640) != WASMOS_INVAL ||
        framebuffer_set_geometry(640, 480, 639) != WASMOS_INVAL) {
        return __LINE__;
    }
    return geometry_is(1024, 768, 1024) ? 0 : __LINE__;
}

/* Regression: 2026-11-24-framebuffer-geometry-overflow -- the size check
 * multiplied stride x height x 4 in 64 bits, and 0x80000000 x 0x80000000 x 4
 * wraps to zero, so an impossible geometry passed and was published to every
 * framebuffer consumer, which then drew far outside the mapped range. */
static int test_a_geometry_whose_byte_count_wraps_is_refused(void) {
    record_boot_framebuffer();
    if (framebuffer_set_geometry(1, 0x80000000u, 0x80000000u) !=
        WASMOS_ERR_FRAMEBUFFER_GEOMETRY_TOO_LARGE) {
        return __LINE__;
    }
    return geometry_is(1024, 768, 1024) ? 0 : __LINE__;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_geometry_that_fits_is_recorded),
        WASMOS_TEST_CASE(test_a_geometry_past_the_recorded_size_is_refused),
        WASMOS_TEST_CASE(test_a_geometry_whose_byte_count_wraps_is_refused),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    printf("test_framebuffer_geometry: ok\n");
    return 0;
}
