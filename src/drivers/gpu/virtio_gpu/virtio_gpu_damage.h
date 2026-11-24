/* virtio_gpu_damage.h - the device-independent half of the virtio-gpu
 * driver's flush path: reading a DISPLAY_IPC_FLUSH_REQ descriptor and reducing
 * its rectangles to the one region the device is asked to present. Nothing here
 * touches the device or makes a host call, so it runs on the host under
 * tests/unit/test_virtio_gpu_damage.c. */
#ifndef WASMOS_VIRTIO_GPU_DAMAGE_H
#define WASMOS_VIRTIO_GPU_DAMAGE_H

#include <stdint.h>

#include "wasmos_driver_abi.h"

/* The region to present: empty when width or height is 0. */
typedef wasmos_display_rect_t vgpu_region_t;

/* Validates a descriptor of `size` bytes as the client sent it (the bytes
 * already copied out of its buffer into `desc`). Returns 0, or
 * WASMOS_ERR_DISPLAY_BAD_DESC when the version is not
 * DISPLAY_FLUSH_DESC_VERSION, `count` exceeds DISPLAY_FLUSH_RECTS_MAX, or
 * `size` does not cover the header and `count` rects. */
int32_t vgpu_damage_check(const wasmos_display_flush_desc_t* desc, uint32_t size);

/* The bounding box of `count` rects, each first clipped to a scanout of
 * `width` x `height`. Rects that are empty or lie wholly outside contribute
 * nothing; the result is empty when none remains. The device is asked for one
 * transfer and one flush per request, so the union is presented whole: cheap
 * next to the round trip for every rect, and never less than was damaged. */
vgpu_region_t vgpu_damage_bounds(const wasmos_display_rect_t* rects, uint32_t count, uint32_t width,
                                 uint32_t height);

/* Grows `acc` to cover `r` as well (either may be empty). */
vgpu_region_t vgpu_region_union(vgpu_region_t acc, vgpu_region_t r);

#endif
