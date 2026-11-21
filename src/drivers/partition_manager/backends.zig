//! backends.zig — the backend endpoints a block client has lent its buffers to.
//!
//! A client lends each request and descriptor buffer to a backend PROCESS once,
//! not per device: a backend serving two disks answers both on one endpoint,
//! and a second borrow of the same buffer to it is refused ALREADY_BORROWED
//! (docs/architecture/12-dma-transfers.md). This table is what keeps the second
//! attempt from being made.
//!
//! A backend that ENDS takes its borrows with it: the kernel force-revokes every
//! borrow a context holds when it exits. Its entry must go too. A supervised
//! driver comes back on a NEW endpoint, which needs its own grant, and a table
//! that remembered every endpoint it ever lent to fills after a handful of
//! restarts and then lends to nothing -- every request the next instance reads
//! out of an unlent buffer fails.
//!
//! Pure and separate from the drivers so that contract is host-tested
//! (tests/unit/test_partition_backends.zig). The partition manager and the
//! volume manager both use it.

pub const Note = enum {
    /// Newly recorded: the caller lends its buffers to this backend now.
    lend,
    /// Already lent to; nothing to do.
    known,
    /// No room. The caller cannot lend, and its requests to this backend fail.
    full,
};

pub fn Table(comptime capacity: usize) type {
    return struct {
        const Self = @This();

        endpoints: [capacity]i32 = [_]i32{-1} ** capacity,
        count: usize = 0,

        /// Record that `ep` serves a device this client talks to.
        pub fn note(self: *Self, ep: i32) Note {
            for (self.endpoints[0..self.count]) |seen| {
                if (seen == ep) return .known;
            }
            if (self.count >= capacity) return .full;
            self.endpoints[self.count] = ep;
            self.count += 1;
            return .lend;
        }

        /// Forget `ep`: the backend behind it ended or withdrew, and the next
        /// `note` of it lends again. Forgetting an endpoint never recorded is a
        /// no-op, so a repeated end -- a REMOVE and a hangup for the same
        /// process -- is harmless.
        pub fn forget(self: *Self, ep: i32) void {
            var i: usize = 0;
            while (i < self.count) : (i += 1) {
                if (self.endpoints[i] != ep) continue;
                self.count -= 1;
                self.endpoints[i] = self.endpoints[self.count];
                self.endpoints[self.count] = -1;
                return;
            }
        }
    };
}
