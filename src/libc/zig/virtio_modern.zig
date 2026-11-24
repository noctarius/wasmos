//! virtio_modern.zig — Zig binding for the WASMOS virtio 1.x PCI transport.
//!
//! There is no transport logic here. The transport is `wasmos/virtio_modern.h`
//! (static-inline C), compiled into the module by `virtio_modern_shim.c` and
//! exposed below as extern C declarations, exactly as `vring.zig` does for the
//! virtqueue core. Read `wasmos/virtio_modern.h` for the contracts: where the
//! structures come from, the BAR rule, the negotiation order and what a refused
//! vector looks like. What this file adds is the Zig shape: `bool`/`?T` in place
//! of the C sentinel returns.
//!
//! A module drives ONE device through this binding; the shim holds its state.

const vring = @import("vring.zig");

/// Device status bits (virtio 1.2 §2.1), mirroring VIRTIO_STATUS_* in the header.
pub const STATUS_ACKNOWLEDGE: u8 = 1;
pub const STATUS_DRIVER: u8 = 2;
pub const STATUS_DRIVER_OK: u8 = 4;
pub const STATUS_FEATURES_OK: u8 = 8;
pub const STATUS_FAILED: u8 = 128;

/// MSI-X "no vector": pass it to `queueEnable` for a queue served by INTx.
pub const MSI_NO_VECTOR: u16 = 0xFFFF;

/// The used-ring alignment the modern layout is laid out with
/// (VIRTIO_MODERN_VRING_ALIGN); pass it to `vring.size` and `Queue.layout`.
pub const VRING_ALIGN: u64 = 4096;

extern fn virtio_modern_zig_probe(bdf: u32, bar: u8, region: i32) callconv(.c) i32;
extern fn virtio_modern_zig_negotiate(wanted: u64, out_offered: ?*u64, out_accepted: ?*u64) callconv(.c) i32;
extern fn virtio_modern_zig_set_status(status: u8) callconv(.c) void;
extern fn virtio_modern_zig_set_config_vector(entry: u16) callconv(.c) i32;
extern fn virtio_modern_zig_queue_size(index: u16) callconv(.c) u16;
extern fn virtio_modern_zig_queue_enable(index: u16, vq: *const anyopaque, msix_entry: u16, out_notify: *u32) callconv(.c) i32;
extern fn virtio_modern_zig_notify(notify: u32, index: u16) callconv(.c) void;
extern fn virtio_modern_zig_isr() callconv(.c) u8;
extern fn virtio_modern_zig_device_read(off: u32, out: [*]u8, len: u32) callconv(.c) i32;
extern fn virtio_modern_zig_device_write8(off: u32, v: u8) callconv(.c) i32;

/// Locate the device's structures from function `bdf`'s config space; each must
/// sit in BAR `bar`, which the manifest declared as region `region`. False for
/// a legacy-only device or one that places a structure elsewhere.
pub fn probe(bdf: u32, bar: u8, region: i32) bool {
    return virtio_modern_zig_probe(bdf, bar, region) == 0;
}

/// What negotiation settled on.
pub const Features = struct {
    offered: u64,
    accepted: u64,
};

/// Reset, acknowledge and accept the offered bits of `wanted` plus VERSION_1,
/// leaving the device in FEATURES_OK. Null (with FAILED set) when the device
/// does not offer VERSION_1 or refuses the subset.
pub fn negotiate(wanted: u64) ?Features {
    var f = Features{ .offered = 0, .accepted = 0 };
    if (virtio_modern_zig_negotiate(wanted, &f.offered, &f.accepted) != 0) return null;
    return f;
}

pub fn setStatus(status: u8) void {
    virtio_modern_zig_set_status(status);
}

/// Route configuration changes to MSI-X entry `entry`; false when refused.
pub fn setConfigVector(entry: u16) bool {
    return virtio_modern_zig_set_config_vector(entry) == 0;
}

/// The size queue `index` offers, or 0 when the device has no such queue.
pub fn queueSize(index: u16) u16 {
    return virtio_modern_zig_queue_size(index);
}

/// Hand queue `index` to the device -- the size and ring areas `queue` was laid
/// out with, and its MSI-X entry (MSI_NO_VECTOR for none) -- and enable it.
/// Returns the queue's doorbell offset for `notify`, or null when the device
/// refuses the vector.
pub fn queueEnable(index: u16, queue: vring.Queue, msix_entry: u16) ?u32 {
    var notify_off: u32 = 0;
    if (virtio_modern_zig_queue_enable(index, queue.handle, msix_entry, &notify_off) != 0) return null;
    return notify_off;
}

/// Ring queue `index`'s doorbell at the offset `queueEnable` returned.
pub fn notify(notify_off: u32, index: u16) void {
    virtio_modern_zig_notify(notify_off, index);
}

/// Read (and so clear) the ISR byte; only an INTx driver needs it.
pub fn isr() u8 {
    return virtio_modern_zig_isr();
}

/// Copy `out.len` bytes of the device-specific configuration at `off` as one
/// consistent snapshot. False when there is no such structure or range.
pub fn deviceRead(off: u32, out: []u8) bool {
    return virtio_modern_zig_device_read(off, out.ptr, @intCast(out.len)) == 0;
}

/// Write one byte of the device-specific configuration (a driver-writable
/// field such as virtio-input's select). False when there is no such structure
/// or `off` lies outside it.
pub fn deviceWrite8(off: u32, v: u8) bool {
    return virtio_modern_zig_device_write8(off, v) == 0;
}
