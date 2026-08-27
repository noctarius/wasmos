/* vring_shim.c
 * Freestanding WASM C shim exposing wasmos/vring.h to Zig.
 *
 * The virtqueue core is static-inline C. Zig cannot call it directly and
 * @cImport cannot translate it (vring_mb() is a compiler barrier builtin), so
 * this file compiles the core into the module and re-exports it as plain
 * functions over an opaque queue handle -- the same arrangement libui_shim.c
 * uses for libui. The consequence that matters: there is ONE ring
 * implementation in the tree, and a Zig driver and a C driver cannot drift
 * apart in how they drive a device.
 *
 * Compiled by Zig's built-in Clang for wasm32-freestanding alongside the Zig
 * root source, with the SDK sysroot on the include path (WasmosZigApp.cmake).
 */

#include <stdint.h>

#include "wasmos/vring.h"

/* Queues available to one module. A driver owns one queue per virtqueue its
 * device defines: virtio-blk has one, virtio-net has two. Handles come from
 * this fixed pool rather than an allocator because a driver's queue count is
 * known at bring-up and never grows. */
#define VRING_ZIG_MAX_QUEUES 4u

static vring_t g_queues[VRING_ZIG_MAX_QUEUES];
static uint32_t g_queue_count;

/* Claim a queue slot. Returns an opaque handle, or NULL once the pool is
 * exhausted. Slots are never returned: a driver allocates its queues during
 * bring-up and keeps them for its whole life. */
void* vring_zig_alloc(void) {
    if (g_queue_count >= VRING_ZIG_MAX_QUEUES) {
        return 0;
    }
    return &g_queues[g_queue_count++];
}

uint64_t vring_zig_size(uint16_t num, uint64_t align) {
    return vring_size(num, align);
}

int32_t vring_zig_layout(void* vq, uint8_t* region_base, uint64_t region_phys,
                         uint64_t region_bytes, uint16_t num, uint64_t align) {
    return vring_layout((vring_t*)vq, region_base, region_phys, region_bytes, num, align);
}

void vring_zig_set_notify(void* vq, void (*notify)(void* user), void* user) {
    vring_set_notify((vring_t*)vq, notify, user);
}

int32_t vring_zig_alloc_chain(void* vq, const vring_buf_t* bufs, uint16_t count) {
    return vring_alloc_chain((vring_t*)vq, bufs, count);
}

void vring_zig_free_chain(void* vq, uint16_t head) {
    vring_free_chain((vring_t*)vq, head);
}

void vring_zig_publish(void* vq, uint16_t head) {
    vring_publish((vring_t*)vq, head);
}

void vring_zig_kick(void* vq) {
    vring_kick((vring_t*)vq);
}

int32_t vring_zig_get_used(void* vq, uint32_t* out_len) {
    return vring_get_used((vring_t*)vq, out_len);
}

/* Device address of the queue's region base, which is what the device is
 * programmed with (as a page frame number) and what a driver adds its own
 * buffer offsets to. */
uint64_t vring_zig_region_phys(const void* vq) {
    return ((const vring_t*)vq)->region_phys;
}

uint16_t vring_zig_num_free(const void* vq) {
    return ((const vring_t*)vq)->num_free;
}
