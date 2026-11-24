/* virtio_modern_shim.c
 * Freestanding WASM C shim exposing wasmos/virtio_modern.h to Zig.
 *
 * The virtio 1.x PCI transport is static-inline C over the io_region_* and
 * pci_config_read host calls. As with vring_shim.c, this file compiles it into
 * the module and re-exports it as plain functions, so a Zig driver drives its
 * device through exactly the transport code a C driver does and the two cannot
 * drift apart.
 *
 * A driver drives one device, so the shim holds that one device's transport
 * state rather than handing out handles. Queue handles come from vring_shim.c,
 * whose opaque handle IS a vring_t in the same module.
 *
 * Compiled by Zig's built-in Clang for wasm32-freestanding alongside the Zig
 * root source, with the SDK sysroot on the include path (scripts/sdk/wasmos-zig).
 */

#include <stdint.h>

#include "wasmos/virtio_modern.h"

static virtio_modern_t g_virtio_dev;

int32_t virtio_modern_zig_probe(uint32_t bdf, uint8_t bar, int32_t region) {
    return virtio_modern_probe(&g_virtio_dev, bdf, bar, region);
}

int32_t virtio_modern_zig_negotiate(uint64_t wanted, uint64_t* out_offered,
                                    uint64_t* out_accepted) {
    return virtio_modern_negotiate(&g_virtio_dev, wanted, out_offered, out_accepted);
}

void virtio_modern_zig_set_status(uint8_t status) {
    virtio_modern_set_status(&g_virtio_dev, status);
}

int32_t virtio_modern_zig_set_config_vector(uint16_t entry) {
    return virtio_modern_set_config_vector(&g_virtio_dev, entry);
}

uint16_t virtio_modern_zig_queue_size(uint16_t index) {
    return virtio_modern_queue_size(&g_virtio_dev, index);
}

int32_t virtio_modern_zig_queue_enable(uint16_t index, const void* vq, uint16_t msix_entry,
                                       uint32_t* out_notify) {
    return virtio_modern_queue_enable(
        &g_virtio_dev, index, (const vring_t*)vq, msix_entry, out_notify);
}

void virtio_modern_zig_notify(uint32_t notify, uint16_t index) {
    virtio_modern_notify(&g_virtio_dev, notify, index);
}

uint8_t virtio_modern_zig_isr(void) {
    return virtio_modern_isr(&g_virtio_dev);
}

int32_t virtio_modern_zig_device_read(uint32_t off, uint8_t* out, uint32_t len) {
    return virtio_modern_device_read(&g_virtio_dev, off, out, len);
}

int32_t virtio_modern_zig_device_write8(uint32_t off, uint8_t v) {
    return virtio_modern_device_write8(&g_virtio_dev, off, v);
}
