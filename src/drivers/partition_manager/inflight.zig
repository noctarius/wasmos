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
    /// The id we used downstream, which is what the reply carries back.
    down_request_id: i32 = 0,
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

    pub fn findByDownId(self: *Table, request_id: i32) ?*Inflight {
        for (&self.entries) |*e| {
            if (e.in_use and e.down_request_id == request_id) return e;
        }
        return null;
    }
};
