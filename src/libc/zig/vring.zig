//! vring.zig — Zig binding for the WASMOS virtqueue core.
//!
//! There is no ring logic here. The split-virtqueue implementation is
//! `wasmos/vring.h`, which is static-inline C; it is compiled into the module
//! by `vring_shim.c` and exposed below as extern C declarations, the same way
//! `libui.zig` reaches libui. A Zig driver therefore drives a device through
//! exactly the code a C driver does, and the two cannot drift.
//!
//! Read `wasmos/vring.h` for the contracts — descriptor ownership, the layout,
//! the ordering guarantees and the single-producer rule all live there. What
//! this file adds is the Zig shape: a `Queue` handle with methods, and
//! `?u16`/`bool` in place of the C sentinel returns.
//!
//! Descriptor ownership, restated because getting it wrong corrupts the queue
//! silently: a chain head is the driver's after `allocChain`, the DEVICE's from
//! `publish` until `getUsed` reports it, and the driver's again after that. The
//! buffers a chain names must not be touched while the device owns it, and a
//! chain must be released with `freeChain`, once.

/// Descriptor flags, mirroring VRING_DESC_F_* in `wasmos/vring.h`.
/// WRITE marks a buffer the DEVICE writes and the driver reads after
/// completion; a buffer the driver filled for the device to read carries 0.
/// NEXT is set by `allocChain` itself and must not be passed in.
pub const DESC_F_WRITE: u16 = 2;

/// Legacy vring alignment, fixed by the spec's page granularity: the queue
/// address is programmed into the device as a page frame number.
pub const LEGACY_ALIGN: u64 = 4096;

/// One buffer of a descriptor chain. Layout mirrors `vring_buf_t`.
pub const Buf = extern struct {
    /// Device address of the buffer.
    addr: u64,
    len: u32,
    /// DESC_F_WRITE, or 0 when the device only reads the buffer.
    flags: u16,
};

extern fn vring_zig_alloc() callconv(.c) ?*anyopaque;
extern fn vring_zig_size(num: u16, alignment: u64) callconv(.c) u64;
extern fn vring_zig_layout(vq: *anyopaque, region_base: [*]u8, region_phys: u64, region_bytes: u64, num: u16, alignment: u64) callconv(.c) i32;
extern fn vring_zig_set_notify(vq: *anyopaque, notify: *const fn (user: ?*anyopaque) callconv(.c) void, user: ?*anyopaque) callconv(.c) void;
extern fn vring_zig_alloc_chain(vq: *anyopaque, bufs: [*]const Buf, count: u16) callconv(.c) i32;
extern fn vring_zig_free_chain(vq: *anyopaque, head: u16) callconv(.c) void;
extern fn vring_zig_publish(vq: *anyopaque, head: u16) callconv(.c) void;
extern fn vring_zig_kick(vq: *anyopaque) callconv(.c) void;
extern fn vring_zig_get_used(vq: *anyopaque, out_len: ?*u32) callconv(.c) i32;
extern fn vring_zig_region_phys(vq: *const anyopaque) callconv(.c) u64;
extern fn vring_zig_num_free(vq: *const anyopaque) callconv(.c) u16;

/// Bytes of contiguous region a queue of `num` entries needs with an
/// `alignment`-aligned used ring. Use this to size the region allocation.
pub fn size(num: u16, alignment: u64) u64 {
    return vring_zig_size(num, alignment);
}

/// A virtqueue. The handle names a queue owned by the shim, so a Queue is a
/// plain value that may be copied and stored; the storage behind it lives for
/// the module's whole life.
pub const Queue = struct {
    handle: *anyopaque,

    /// Claim a queue from the shim's fixed pool, or null once it is exhausted
    /// (four per module — a driver takes one per virtqueue its device defines).
    pub fn create() ?Queue {
        const handle = vring_zig_alloc() orelse return null;
        return .{ .handle = handle };
    }

    /// Lay the queue out over the region, zero the rings and build the free
    /// list. `region_phys` is the device address of the same memory and `num`
    /// must be a power of two. False means the region is too small or `num` is
    /// unusable, and the queue must not be used.
    pub fn layout(self: Queue, region_base: [*]u8, region_phys: u64, region_bytes: u64, num: u16, alignment: u64) bool {
        return vring_zig_layout(self.handle, region_base, region_phys, region_bytes, num, alignment) == 0;
    }

    /// Install the doorbell `kick` rings. `layout` clears it, so this must be
    /// re-done after every layout.
    pub fn setNotify(self: Queue, notify: *const fn (user: ?*anyopaque) callconv(.c) void, user: ?*anyopaque) void {
        vring_zig_set_notify(self.handle, notify, user);
    }

    /// Allocate and link one descriptor per entry of `bufs`, returning the
    /// chain head. Null means the chain does not fit: too long, or not enough
    /// free descriptors — ordinary backpressure, so retry after reclaiming
    /// completions.
    pub fn allocChain(self: Queue, bufs: []const Buf) ?u16 {
        if (bufs.len == 0) return null;
        const head = vring_zig_alloc_chain(self.handle, bufs.ptr, @intCast(bufs.len));
        if (head < 0) return null;
        return @intCast(head);
    }

    /// Return a whole chain to the free list. Legal only after `getUsed` has
    /// reported the head, or before the chain was ever published.
    pub fn freeChain(self: Queue, head: u16) void {
        vring_zig_free_chain(self.handle, head);
    }

    /// Publish a prepared chain so the device can consume it. Does not ring the
    /// doorbell — batch several publishes, then `kick`.
    pub fn publish(self: Queue, head: u16) void {
        vring_zig_publish(self.handle, head);
    }

    /// Ring the device's doorbell for everything published since the last kick.
    pub fn kick(self: Queue) void {
        vring_zig_kick(self.handle);
    }

    /// Consume the next completed chain, returning its head and, through
    /// `out_len`, the byte count the device reported. Null when nothing is
    /// pending. The chain is not auto-freed: read the buffers, then `freeChain`.
    pub fn getUsed(self: Queue, out_len: ?*u32) ?u16 {
        const head = vring_zig_get_used(self.handle, out_len);
        if (head < 0) return null;
        return @intCast(head);
    }

    /// Device address of the region base — what the device is programmed with,
    /// and what a driver adds its own buffer offsets to.
    pub fn regionPhys(self: Queue) u64 {
        return vring_zig_region_phys(self.handle);
    }

    pub fn numFree(self: Queue) u16 {
        return vring_zig_num_free(self.handle);
    }
};
