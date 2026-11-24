/* virtio_gpu_damage.c - see virtio_gpu_damage.h. */
#include "virtio_gpu_damage.h"

static int region_empty(vgpu_region_t r) {
    return r.width == 0u || r.height == 0u;
}

int32_t vgpu_damage_check(const wasmos_display_flush_desc_t* desc, uint32_t size) {
    if (size < DISPLAY_FLUSH_DESC_SIZE(0) || desc->version != DISPLAY_FLUSH_DESC_VERSION ||
        desc->count > DISPLAY_FLUSH_RECTS_MAX || size < DISPLAY_FLUSH_DESC_SIZE(desc->count)) {
        return WASMOS_ERR_DISPLAY_BAD_DESC;
    }
    return 0;
}

vgpu_region_t vgpu_region_union(vgpu_region_t acc, vgpu_region_t r) {
    if (region_empty(r)) {
        return acc;
    }
    if (region_empty(acc)) {
        return r;
    }
    /* 64-bit ends: x + width of a clipped rect fits 32 bits, but a caller's
     * unclipped one need not. */
    uint64_t x0 = acc.x < r.x ? acc.x : r.x;
    uint64_t y0 = acc.y < r.y ? acc.y : r.y;
    uint64_t ax1 = (uint64_t)acc.x + acc.width, rx1 = (uint64_t)r.x + r.width;
    uint64_t ay1 = (uint64_t)acc.y + acc.height, ry1 = (uint64_t)r.y + r.height;
    uint64_t x1 = ax1 > rx1 ? ax1 : rx1;
    uint64_t y1 = ay1 > ry1 ? ay1 : ry1;
    vgpu_region_t out = {(uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
    return out;
}

vgpu_region_t vgpu_damage_bounds(const wasmos_display_rect_t* rects, uint32_t count, uint32_t width,
                                 uint32_t height) {
    vgpu_region_t acc = {0, 0, 0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        const wasmos_display_rect_t* r = &rects[i];
        if (r->width == 0u || r->height == 0u || r->x >= width || r->y >= height) {
            continue;
        }
        uint64_t x1 = (uint64_t)r->x + r->width;
        uint64_t y1 = (uint64_t)r->y + r->height;
        vgpu_region_t clipped = {r->x,
                                 r->y,
                                 (uint32_t)((x1 > width ? width : x1) - r->x),
                                 (uint32_t)((y1 > height ? height : y1) - r->y)};
        acc = vgpu_region_union(acc, clipped);
    }
    return acc;
}
