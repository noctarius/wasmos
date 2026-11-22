//! inflight.zig — the transfers the partition manager has forwarded to a disk
//! and not yet seen answered.
//!
//! Each entry is everything one forwarded transfer needs until its reply: whom
//! to answer, under which id, which downstream borrow to drop, and WHERE ITS
//! REQUEST DESCRIPTOR IS STAGED. The last is the one that cannot be shared. A
//! backing driver reads a request descriptor when it reaches the message, not
//! when the message is sent, so a descriptor must stay untouched for as long as
//! its transfer is in flight. Every entry therefore owns a fixed, disjoint
//! descriptor slot in the one downstream request buffer, at `req_offset`, for
//! the whole life of the table.
//!
//! An entry outlives the driver it was forwarded to. When a disk's driver ends,
//! its entries are HELD -- the descriptor stays staged, nothing is on the wire
//! -- and resent to the successor that serves the same disk, so a restart is
//! invisible to the client (abi/constants.yaml, block_hold).
//!
//! Pure and separate from the driver so that contract can be tested on the
//! host: staging one transfer must never alter the bytes another live transfer's
//! message points at.

/// Transfers forwarded at once. An entry holds the client's identity until the
/// backing device replies, and may not be reused before then.
pub const MAX: usize = 8;

pub const Inflight = struct {
    /// The requesting client's endpoint, and the request id it will match a
    /// reply against. Both belong to the client, not to us.
    client: i32 = -1,
    client_request_id: i32 = 0,
    /// The id we used downstream, which is what the reply carries back; 0
    /// while the transfer is held, since nothing is on the wire.
    down_request_id: i32 = 0,
    /// The disk the transfer is for, as an index into the driver's disk table.
    /// A held transfer is resent to whatever serves this disk next.
    disk: usize = 0,
    /// BLOCK_IPC_READ_REQ or BLOCK_IPC_WRITE_REQ, as the client sent it.
    req_type: i32 = 0,
    /// The client's grant of a zero-copy destination to this driver, or -1 for
    /// a block-buffer destination. Each forward reborrows it to the disk's
    /// current driver, so a resend reaches the client's pages too.
    up_borrow_id: i32 = -1,
    /// Waiting for the disk's next driver: staged, not sent, and answered by
    /// nothing until it is resent.
    held: bool = false,
    /// The backing driver's endpoint the transfer went to. A driver that ends
    /// answers none of its transfers, so they are failed by this.
    server: i32 = -1,
    /// Sectors asked for after clamping, so a short transfer can be reported
    /// against what was actually requested.
    sectors: u32 = 0,
    resp_type: i32 = 0,
    /// The downstream borrow minted for the backing device on a zero-copy
    /// transfer, or -1 when the destination was the client's own block buffer
    /// and no grant was needed. Dropped when the reply arrives: a reborrow held
    /// past its transfer keeps the client's buffer lent to a device that is done
    /// with it, and the client's next request on the same buffer would be
    /// refused ALREADY_BORROWED.
    down_borrow_id: i32 = -1,
    /// Byte offset of this entry's request descriptor in the downstream request
    /// buffer. Assigned once by `Table.init` and never changed, so no two
    /// entries, live or not, ever share a slot.
    req_offset: u32 = 0,
    in_use: bool = false,
};

pub const Table = struct {
    entries: [MAX]Inflight = [_]Inflight{.{}} ** MAX,

    /// Bytes the downstream request buffer needs: one descriptor slot per entry.
    pub fn bufferBytes(desc_bytes: u32) u32 {
        return desc_bytes * @as(u32, MAX);
    }

    /// A table whose entries own consecutive `desc_bytes` slots.
    pub fn init(desc_bytes: u32) Table {
        var t = Table{};
        for (&t.entries, 0..) |*e, i| {
            e.req_offset = desc_bytes * @as(u32, @intCast(i));
        }
        return t;
    }

    /// A free entry, or null when MAX transfers are in flight. The entry is not
    /// reserved until the caller sets `in_use`, so an abandoned claim costs
    /// nothing. The caller assigns fields one by one rather than replacing the
    /// entry, which would reset `req_offset`.
    pub fn claim(self: *Table) ?*Inflight {
        for (&self.entries) |*e| {
            if (!e.in_use) return e;
        }
        return null;
    }

    /// The transfer on the wire under `request_id`. A held transfer is on no
    /// wire, so a late reply from the driver that ended matches nothing.
    pub fn findByDownId(self: *Table, request_id: i32) ?*Inflight {
        for (&self.entries) |*e| {
            if (e.in_use and !e.held and e.down_request_id == request_id) return e;
        }
        return null;
    }

    /// Park `e` until its disk is served again. The caller has dropped its
    /// downstream borrow already.
    pub fn hold(e: *Inflight) void {
        e.held = true;
        e.down_request_id = 0;
        e.down_borrow_id = -1;
        e.server = -1;
    }

    /// The first transfer held for `disk`, or null. A caller resending or
    /// failing every one either clears `held` or frees the entry before asking
    /// again.
    pub fn nextHeld(self: *Table, disk: usize) ?*Inflight {
        for (&self.entries) |*e| {
            if (e.in_use and e.held and e.disk == disk) return e;
        }
        return null;
    }

    /// True while any transfer is held, for any disk.
    pub fn anyHeld(self: *const Table) bool {
        for (&self.entries) |*e| {
            if (e.in_use and e.held) return true;
        }
        return false;
    }
};
