/* test_virtio_gpu_damage.c -- the device-independent flush path of the
 * virtio-gpu driver (src/drivers/gpu/virtio_gpu/virtio_gpu_damage.c): validating a
 * DISPLAY_IPC_FLUSH_REQ descriptor and reducing its rects to the region the
 * device presents.
 *
 * The cases report through assert(); main() prints "ok" only if every case ran
 * to completion. The suite is compiled without -DNDEBUG. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "virtio_gpu_damage.h"

static wasmos_display_flush_desc_t desc_with(uint32_t count) {
    wasmos_display_flush_desc_t d;
    memset(&d, 0, sizeof(d));
    d.version = DISPLAY_FLUSH_DESC_VERSION;
    d.count = count;
    return d;
}

static void test_descriptor_validation(void) {
    wasmos_display_flush_desc_t d = desc_with(2);
    assert(vgpu_damage_check(&d, DISPLAY_FLUSH_DESC_SIZE(2)) == 0);
    assert(vgpu_damage_check(&d, (uint32_t)sizeof(d)) == 0);
    /* The size must cover every rect the count claims. */
    assert(vgpu_damage_check(&d, DISPLAY_FLUSH_DESC_SIZE(1)) == WASMOS_ERR_DISPLAY_BAD_DESC);
    assert(vgpu_damage_check(&d, 4u) == WASMOS_ERR_DISPLAY_BAD_DESC);
    d.version = 2u;
    assert(vgpu_damage_check(&d, DISPLAY_FLUSH_DESC_SIZE(2)) == WASMOS_ERR_DISPLAY_BAD_DESC);
    d = desc_with(DISPLAY_FLUSH_RECTS_MAX + 1u);
    assert(vgpu_damage_check(&d, 0xFFFFu) == WASMOS_ERR_DISPLAY_BAD_DESC);
    d = desc_with(0);
    assert(vgpu_damage_check(&d, DISPLAY_FLUSH_DESC_SIZE(0)) == 0);
}

static void test_bounds_is_the_union(void) {
    wasmos_display_rect_t r[2] = {{10, 20, 30, 40}, {100, 5, 10, 10}};
    vgpu_region_t b = vgpu_damage_bounds(r, 2, 1280, 800);
    assert(b.x == 10 && b.y == 5 && b.width == 100 && b.height == 55);
}

static void test_bounds_clips_to_the_scanout(void) {
    wasmos_display_rect_t r[3] = {
        {1270, 790, 50, 50},              /* crosses the corner */
        {2000, 10, 5, 5},                 /* wholly outside */
        {0xFFFFFFF0u, 0, 0xFFFFFFFFu, 1}, /* would wrap a 32-bit end */
    };
    vgpu_region_t b = vgpu_damage_bounds(r, 3, 1280, 800);
    assert(b.x == 1270 && b.y == 790 && b.width == 10 && b.height == 10);
}

static void test_empty_rects_present_nothing(void) {
    wasmos_display_rect_t r[2] = {{5, 5, 0, 10}, {5, 5, 10, 0}};
    vgpu_region_t b = vgpu_damage_bounds(r, 2, 1280, 800);
    assert(b.width == 0 || b.height == 0);
    b = vgpu_damage_bounds(r, 0, 1280, 800);
    assert(b.width == 0 || b.height == 0);
}

static void test_union_with_empty(void) {
    vgpu_region_t empty = {0, 0, 0, 0};
    vgpu_region_t a = {3, 4, 5, 6};
    vgpu_region_t u = vgpu_region_union(empty, a);
    assert(u.x == 3 && u.y == 4 && u.width == 5 && u.height == 6);
    u = vgpu_region_union(a, empty);
    assert(u.x == 3 && u.y == 4 && u.width == 5 && u.height == 6);
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_descriptor_validation),
        WASMOS_TEST_CASE(test_bounds_is_the_union),
        WASMOS_TEST_CASE(test_bounds_clips_to_the_scanout),
        WASMOS_TEST_CASE(test_empty_rects_present_nothing),
        WASMOS_TEST_CASE(test_union_with_empty),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("ok\n");
    return 0;
}
