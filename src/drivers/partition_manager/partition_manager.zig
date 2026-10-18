//! partition_manager.zig — publishes each partition of a disk as a block device.
//!
//! What it is
//! ----------
//! A `block` class CLIENT of every disk, and a `block` class PROVIDER of every
//! partition on them. A filesystem driver mounted on a partition cannot tell it
//! from a whole disk: same class, same descriptor, same transfer protocol. That
//! is the entire design — partitions become devices, so nothing downstream needs
//! to learn what a partition is.
//!
//! Why a driver and not a service
//! ------------------------------
//! It touches no hardware, yet it ships as `kind = "driver"`. `kind` sets the
//! scheduler band (SCHED_PRIO_DRIVER above SCHED_PRIO_SERVICE) and this sits IN
//! the block I/O path, underneath filesystem drivers that are themselves in the
//! driver band. A service-band proxy there would be a priority inversion in the
//! storage path. See docs/architecture/36 §2.
//!
//! One endpoint, not a fan-out
//! ---------------------------
//! An earlier design gave this one endpoint per disk downstream and one per
//! partition upstream, because a transfer request could not say which device it
//! meant and backends inferred it from the sender. Both halves of that are gone:
//! wasmos_block_request_t names its `target`, and the ATA driver no longer binds
//! a client endpoint to a drive. So every partition is served on the ONE endpoint
//! the async runtime owns, and every disk is reached from it, with the target
//! field doing the work an endpoint used to do.
//!
//! Discovery
//! ---------
//! The `block` class is SUBSCRIBED to, not enumerated. Enumeration alone answers
//! "which disks exist now", and a driver that registers later — virtio-blk
//! negotiates a PCI device, claims an MSI-X vector and sets up a virtqueue
//! before it publishes anything — is then invisible for the rest of the boot,
//! table and all. Bring-up subscribes first and enumerates second, so nothing
//! falls between the two, and an arrival for a disk already in the table is
//! dropped.
//!
//! Every partition published here registers under that same class, so our own
//! registrations come back as arrivals. They are dropped by endpoint: probing
//! one would send IDENTIFY to ourselves and wait for a reply only our own
//! message loop could produce.
//!
//! Probing
//! -------
//! A probe is synchronous, like fs_fat's: nothing can be published until the
//! disk has been described and its table read. Per disk: IDENTIFY, then LBA 1
//! for a GPT header, then LBA 0 for an MBR, then nothing. A disk with no table
//! publishes no partitions and is left alone — its own class instance already
//! serves it, which is what keeps partition tables optional.
//!
//! Because it blocks, a probe runs on the ROOT task rather than in the message
//! handler that learned of the disk: a handler that blocks stalls the loop it
//! was dispatched from. The handler queues the arrival and wakes the root.
//!
//! Forwarding
//! ----------
//! A transfer for a partition is the client's own request with `lba` rebased onto
//! the partition's window and `sector_count` clamped to it. The backing device
//! still writes the CLIENT's pages, so a partition costs one IPC hop and no copy.
//!
//! Reaching those pages takes a REBORROW, not a pass-through. A borrow is held
//! per context, so the `dst_borrow_id` a client sends names a grant between the
//! client and THIS driver and resolves to nothing for the disk beneath it. Each
//! zero-copy transfer sub-grants that borrow to the backing device, narrowed to
//! the transfer's direction, and drops it when the reply arrives.
const driver = @import("driver.zig");
const co = @import("coroutine.zig");
const op = @import("wasmos_opcodes.zig");
const status = @import("wasmos_status.zig");
const abi = @import("wasmos_constants.zig");
const pt = @import("partition_table.zig");
const rc = @import("reconcile.zig");

/// Disks probed. Each costs a descriptor plus its partitions' records; the bound
/// is this driver's own state, not a property of the system.
const MAX_DISKS: usize = 4;
/// Partitions published across all disks.
const MAX_PARTITIONS: usize = 16;
/// Transfers forwarded at once. A slot holds the client's identity until the
/// backing device replies, and may not be reused before then.
const MAX_INFLIGHT: usize = 8;

const SECTOR_BYTES: u32 = 512;

const Disk = struct {
    /// The raw disk's `block` class instance, which is what a request to it must
    /// name in `target`.
    instance: u32 = 0,
    /// The backing driver's service endpoint.
    endpoint: i32 = -1,
    sector_count: u64 = 0,
    canonical_id: [driver.BLOCK_ID_MAX]u8 = [_]u8{0} ** driver.BLOCK_ID_MAX,
    backend: u32 = 0,
    unit: u32 = 0,
    scheme: pt.Scheme = .none,
    in_use: bool = false,
};

const Partition = struct {
    /// This partition's own class instance: the fingerprint of its canonical id.
    instance: u32 = 0,
    disk: usize = 0,
    /// Absolute window on the backing disk. Every forwarded request is rebased
    /// onto lba_start and clamped to lba_count; this is the containment a
    /// filesystem driver reaching the raw disk would not have.
    lba_start: u64 = 0,
    lba_count: u64 = 0,
    desc: driver.BlockDescriptor = .{},
    in_use: bool = false,
};

const Inflight = struct {
    /// The requesting client's endpoint, and the request id it will match a
    /// reply against. Both belong to the client, not to us.
    client: i32 = -1,
    client_request_id: i32 = 0,
    /// The id we used downstream, which is what the reply carries back.
    down_request_id: i32 = 0,
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
    in_use: bool = false,
};

var g_disks: [MAX_DISKS]Disk = [_]Disk{.{}} ** MAX_DISKS;
var g_disk_count: usize = 0;
var g_parts: [MAX_PARTITIONS]Partition = [_]Partition{.{}} ** MAX_PARTITIONS;
var g_part_count: usize = 0;
var g_inflight: [MAX_INFLIGHT]Inflight = [_]Inflight{.{}} ** MAX_INFLIGHT;

var g_proc_endpoint: i32 = -1;
/// The device manager's inventory endpoint, resolved once and kept: a disk
/// arriving after bring-up publishes its partitions through the same endpoint,
/// and re-resolving per arrival would block the probe on a lookup that already
/// has an answer. Negative when it could not be resolved, which downgrades
/// publishing to class registration alone.
var g_devmgr: i32 = -1;

/// The volume manager's endpoint, resolved on the FIRST retirement rather than
/// at bring-up.
///
/// Not at bring-up because this driver is an initfs payload spawned ahead of
/// the services that publish volumes, so there is nothing to find yet and a
/// failure then would stand for the life of the process. A retirement happens
/// only on a rescan, which is long after the system is up.
///
/// Negative after a failed attempt, and `g_volmgr_tried` is what keeps that
/// from being retried on every partition of a wiped disk -- each attempt is a
/// blocking lookup on the root task.
var g_volmgr: i32 = -1;
var g_volmgr_tried: bool = false;

/// Backend endpoints this process has lent its probe buffers to.
///
/// Grants are per PROCESS, not per endpoint or per device, so one entry covers
/// every disk a backend serves and a second attempt would be refused as
/// ALREADY_BORROWED. Kept for the life of the process rather than rebuilt per
/// probe, because a late-arriving disk may be served by a backend already lent
/// to -- an ATA controller publishing its second drive, say.
var g_granted: [MAX_DISKS]i32 = [_]i32{-1} ** MAX_DISKS;
var g_granted_count: usize = 0;

/// Disks that have registered under the `block` class but not yet been probed,
/// and disks a RESCAN has asked to be read again.
///
/// An arrival is queued rather than probed where it is received: a probe is
/// synchronous and a message handler must not block the loop it runs on. The
/// root task drains this.
const Arrival = struct {
    instance: u32 = 0,
    endpoint: i32 = -1,
    in_use: bool = false,
    /// Re-read the table of a disk already in the table, which only a RESCAN
    /// does. An arrival from the class never does: a disk already probed is
    /// dropped, which is what makes the subscription and the opening
    /// enumeration safe to overlap.
    force: bool = false,
    /// Who to answer once the rescan has settled, and with which request id.
    /// Negative when nothing is waiting, which is every arrival from the class.
    reply_to: i32 = -1,
    reply_id: i32 = 0,
};
/// One slot per disk this process can hold, which is the most that can ever be
/// waiting: an arrival beyond MAX_DISKS could not be recorded even if probed.
var g_pending: [MAX_DISKS]Arrival = [_]Arrival{.{}} ** MAX_DISKS;

/// A partition being withdrawn, and how far the withdrawal has got.
///
/// Three peers are involved, in a fixed order, and the order is the design.
///
/// The VOLUME MANAGER goes first, and it is the only one that can refuse. A
/// volume carries a CLAIM while a filesystem service holds it, and that claim
/// is the only live record in the system of whether anything is mounted on a
/// device -- the device manager's `active_service` is never set, so its
/// inventory cannot answer this (docs/TASKS.md `[BUG][P3]`). Retiring a
/// partition out from under a mount would strip the device out of discovery and
/// out of the inventory while the filesystem went on writing to it, so a
/// claimed partition is left exactly as it was. A partition with no volume at
/// all -- an unformatted one, or one whose filesystem nothing recognised -- has
/// nothing that could hold it and passes.
///
/// The DEVICE MANAGER goes second, and the CLASS REGISTRY last. Unregistering
/// the class before the inventory would leave a device that cannot be found but
/// is still listed, and a rule engine matching on an entry nothing serves.
///
/// Every stage is request/reply and none is waited for. A service that blocked
/// on a peer's answer could not serve the requests that peer may need in order
/// to produce it, so a retirement is a state machine the message loop advances:
/// a reply arrives, the stage moves on, and the rescan that scheduled it is
/// answered once its last retirement has settled.
const Retire = struct {
    /// The slot in `g_parts` being withdrawn. Its record stays live and serving
    /// until the retirement completes, because a refusal must leave the
    /// partition exactly as it was.
    part: usize = 0,
    /// The id of the request currently outstanding, which is what its reply is
    /// matched on.
    request_id: i32 = 0,
    /// The class registration's withdrawal, holding the staged class name until
    /// the process manager has read it.
    class: driver.ClassRetire = .{},
    /// `claimwait` is a retirement that has not asked yet. Claim checks run ONE
    /// at a time because VOLUME_IPC_IDENTIFY_REQ writes its descriptor at
    /// offset 0 of the buffer it is given and takes no offset, so two in flight
    /// would both read whichever answer landed last. A wiped disk retires every
    /// partition it had, so that is not a rare case.
    stage: enum { claimwait, claimcheck, unpublish, unregister } = .claimwait,
    /// The rescan waiting on this, as an index into `g_rescans`, or null when
    /// the retirement was not scheduled by one.
    rescan: ?usize = null,
    in_use: bool = false,
};
/// One per partition, because a disk whose table is wiped retires all of them
/// at once and a queue shorter than that would leave the remainder published
/// against a table that no longer describes them.
var g_retires: [MAX_PARTITIONS]Retire = [_]Retire{.{}} ** MAX_PARTITIONS;

/// A rescan whose reply is still owed.
///
/// A rescan is answered when the table has been read AND every retirement it
/// scheduled has settled, which is later than the root task's drain: the
/// retirements are driven by replies that arrive on the message loop long after
/// the drain returned. The count is what ties the two together.
const Rescan = struct {
    reply_to: i32 = -1,
    reply_id: i32 = 0,
    disk: usize = 0,
    /// Retirements this rescan started, valid only once `diffed` is set.
    scheduled: u32 = 0,
    /// Retirements that have reached an outcome, refused or not.
    settled: u32 = 0,
    /// Retirements a peer refused, reported rather than failing the rescan: the
    /// rest of the disk was reconciled correctly, and the caller needs to know
    /// which part did not take.
    refused: u32 = 0,
    /// Whether the diff has finished and `scheduled` therefore means anything.
    ///
    /// Settled retirements are counted UP to a total fixed afterwards rather
    /// than a total being counted down, because the total does not exist until
    /// the diff has walked the whole table: a count of zero means "none yet",
    /// not "none left", and answering on it would report a rescan complete
    /// before it had retired anything.
    diffed: bool = false,
    in_use: bool = false,
};
/// One per disk, which is the most that can be outstanding: a rescan of a disk
/// already being rescanned is refused rather than queued.
var g_rescans: [MAX_DISKS]Rescan = [_]Rescan{.{}} ** MAX_DISKS;

/// The root task parks on this and a handler settles it. Returning `yielded`
/// WITHOUT awaiting is not parking: the coroutine stays runnable, the runner
/// never reaches its poll, and the process spins at full CPU instead of
/// sleeping -- which stalls the boot rather than merely wasting time.
var g_wake_future: co.Future = .{};
var g_wake_promise: co.Promise = .{};
/// True while the root is parked, so a handler only settles a promise something
/// is actually waiting on.
var g_root_parked: bool = false;
var g_next_request_id: i32 = 0x7000;

/// Buffer holding the request WE send downstream. Acquired and lent to each disk
/// once at bring-up and reused for every forwarded transfer, which is the shape
/// a request descriptor is meant to be used in — not one buffer per request.
var g_down_req_bid: i32 = -1;
/// Buffer holding the request a PROBE sends downstream.
///
/// Separate from g_down_req_bid even though both carry a wasmos_block_request_t,
/// because a probe no longer happens only at bring-up: a disk registering later
/// is probed while clients are forwarding transfers, and a backend reads a
/// request descriptor when it gets to the message, not when the message is sent.
/// Sharing one buffer would let a probe overwrite a client's request in the
/// window between the two.
var g_probe_req_bid: i32 = -1;
/// Buffer a client's descriptor is written into when it IDENTIFYs a partition.
/// The client owns ITS buffer; this one is only staging for the probe.
var g_probe_bid: i32 = -1;
/// Buffer the device manager reads published partition descriptors out of: one
/// slot per partition, lent READ for the life of the process.
///
/// Separate from g_probe_bid, and slotted rather than reused, for two reasons a
/// publish path cannot do without. A publish is fire-and-forget, so the receiver
/// reads the bytes at a moment of its choosing; a single slot rewritten by the
/// next probe hands it whichever descriptor arrived last. And the device manager
/// is a grantee like any other -- it cannot read a buffer it was never lent, so
/// the borrow below is what makes a publish legible at all rather than an
/// optimisation. Same shape as the PCI bus's device publish.
var g_publish_bid: i32 = -1;
/// Buffer the device manager reads a RETIRING partition's canonical id out of:
/// one slot per in-flight retirement, lent READ for the life of the process.
///
/// Slotted for the same reason the publish buffer is. A retirement is answered
/// rather than fire-and-forget, but the id is still read when the device
/// manager reaches the message, and a disk whose table was wiped has one
/// retirement per partition in flight at once; one slot rewritten by the next
/// would withdraw whichever id arrived last, repeatedly.
var g_retire_bid: i32 = -1;
/// Buffer the volume manager writes a volume descriptor into when a retirement
/// asks whether anything holds the partition. Lent WRITE for the life of the
/// process.
///
/// ONE descriptor, not one per retirement, because VOLUME_IPC_IDENTIFY_REQ
/// writes at offset 0 and takes no offset -- which is what forces the claim
/// checks to run one at a time (see Retire.stage).
var g_claim_bid: i32 = -1;
/// Physical address of this process's block buffer, where probe reads land.
var g_block_phys: u32 = 0;

/// Probe scratch, deliberately STATIC rather than stack locals.
///
/// The Zig shadow stack is 8 KiB (see cmake/WasmosZigApp.cmake, which explains
/// why it must stay small), and these do not fit in it: a pt.Table holds 32
/// partition records of roughly 200 bytes each, and a GptEntryScan embeds one
/// plus a 512-byte reassembly buffer. Declaring them as locals traps the module
/// on its first probe with an out-of-bounds access, before a single line of its
/// own logging runs. Probing is single-threaded and non-reentrant, so one copy
/// of each is enough.
var g_sector: [pt.SECTOR_BYTES]u8 = [_]u8{0} ** pt.SECTOR_BYTES;
var g_table: pt.Table = .{};
var g_scan: pt.GptEntryScan = undefined;

fn nextRequestId() i32 {
    g_next_request_id += 1;
    if (g_next_request_id < 0) g_next_request_id = 0x7000;
    return g_next_request_id;
}

// --- canonical ids -----------------------------------------------------------

/// Append `p<slot>` to a disk's canonical id, which is how a partition is named:
/// `block:ata:0p1`. Returns false if it does not fit, which must fail the
/// registration — a truncated id fingerprints to a different value than anyone
/// looking for it would compute.
fn buildPartitionId(disk_id: []const u8, slot: u32, out: *[driver.BLOCK_ID_MAX]u8) bool {
    for (out) |*b| b.* = 0;
    var n: usize = 0;
    for (disk_id) |c| {
        if (c == 0) break;
        if (n + 1 >= out.len) return false;
        out[n] = c;
        n += 1;
    }
    if (n == 0 or n + 1 >= out.len) return false;
    out[n] = 'p';
    n += 1;

    var digits: [10]u8 = undefined;
    var value = slot;
    var d: usize = 0;
    if (value == 0) {
        digits[0] = '0';
        d = 1;
    } else {
        while (value != 0) : (value /= 10) {
            digits[d] = '0' + @as(u8, @intCast(value % 10));
            d += 1;
        }
    }
    while (d > 0) {
        d -= 1;
        if (n + 1 >= out.len) return false;
        out[n] = digits[d];
        n += 1;
    }
    out[n] = 0;
    return true;
}

fn idSlice(id: *const [driver.BLOCK_ID_MAX]u8) []const u8 {
    var n: usize = 0;
    while (n < id.len and id[n] != 0) : (n += 1) {}
    return id[0..n];
}

// --- talking to a disk (synchronous, bring-up only) --------------------------

/// IDENTIFY a disk into `out`. Synchronous: bring-up has nothing else to do, and
/// nothing can be published until every disk has been described.
fn identifyDisk(disk_endpoint: i32, instance: u32, out: *driver.BlockDescriptor) bool {
    if (g_probe_bid < 0) return false;
    const req_id = nextRequestId();
    const reply = driver.call(
        disk_endpoint,
        driver.privateReplyEndpoint(),
        op.BLOCK_IPC_IDENTIFY_REQ,
        req_id,
        @intCast(instance),
        g_probe_bid,
        0,
        0,
    ) orelse return false;
    if (reply.type != op.BLOCK_IPC_IDENTIFY_RESP or reply.arg0 != 0) return false;
    if (reply.arg1 < @as(i32, @intCast(@sizeOf(driver.BlockDescriptor)))) return false;
    return driver.bufferRead(g_probe_bid, out, 0);
}

/// Read `count` sectors from `lba` of `disk` into this process's block buffer,
/// then copy them out into `out`. Synchronous, for the same reason as above.
fn readSectors(disk: *const Disk, lba: u64, count: u32, out: []u8) bool {
    if (g_probe_req_bid < 0 or g_block_phys == 0) return false;
    var req = driver.BlockRequest{};
    req.target = disk.instance;
    req.lba = lba;
    req.sector_count = count;
    req.dst_kind = driver.BLOCK_DST_BLOCK_BUFFER;
    req.dst_phys = g_block_phys;

    const raw: [*]const u8 = @ptrCast(&req);
    if (!driver.bufferWrite(g_probe_req_bid, raw[0..@sizeOf(driver.BlockRequest)], 0)) return false;

    const req_id = nextRequestId();
    const reply = driver.call(
        disk.endpoint,
        driver.privateReplyEndpoint(),
        op.BLOCK_IPC_READ_REQ,
        req_id,
        g_probe_req_bid,
        0,
        @intCast(@sizeOf(driver.BlockRequest)),
        0,
    ) orelse return false;
    if (reply.type != op.BLOCK_IPC_READ_RESP or reply.arg0 != 0) return false;
    return driver.blockBufferCopy(g_block_phys, out, 0);
}

// --- publishing --------------------------------------------------------------

/// Register one partition under the `block` class and announce it to the device
/// manager, so a rule can match it exactly as it matches a whole disk.
fn publishPartition(part: *Partition, slot: usize, devmgr: i32) void {
    if (driver.registerService(
        g_proc_endpoint,
        endpoint(),
        "partition",
        "block",
        part.instance,
        nextRequestId(),
    ) == null) {
        var line = driver.Line{};
        _ = line.str("[partition-manager] class register failed id=").str(idSlice(&part.desc.canonical_id));
        line.end();
        return;
    }

    announcePartition(part, slot, devmgr);

    var line = driver.Line{};
    _ = line.str("[partition-manager] partition id=").str(idSlice(&part.desc.canonical_id));
    _ = line.str(" instance=").dec(part.instance);
    _ = line.str(" lba=").dec(part.lba_start).str(" count=").dec(part.lba_count);
    line.end();
}

/// Hand the device manager this partition's descriptor.
///
/// Separate from the class registration above because a RESCAN needs exactly
/// this half and not that one: a partition whose extent moved keeps its class
/// instance -- the id it fingerprints is built from the disk and the slot, and
/// neither moved -- so re-registering would be a duplicate, while the inventory
/// still holds the old extent and must be told. The inventory upserts by
/// canonical id, so a second announcement of the same device replaces the
/// record rather than adding one.
fn announcePartition(part: *Partition, slot: usize, devmgr: i32) void {
    if (devmgr < 0 or g_publish_bid < 0 or slot >= MAX_PARTITIONS) return;
    const offset: u32 = @intCast(slot * @sizeOf(driver.BlockDescriptor));
    const raw: [*]const u8 = @ptrCast(&part.desc);
    if (!driver.bufferWrite(g_publish_bid, raw[0..@sizeOf(driver.BlockDescriptor)], offset)) return;
    _ = driver.send(
        devmgr,
        endpoint(),
        op.DEVMGR_PUBLISH_BLOCK_DEVICE,
        0,
        g_publish_bid,
        @intCast(offset),
        @intCast(@sizeOf(driver.BlockDescriptor)),
        0,
    );
}

/// Turn one parsed table entry into a published partition.
fn addPartition(disk_index: usize, entry: pt.Partition, scheme: pt.Scheme, devmgr: i32) void {
    const disk = &g_disks[disk_index];

    // A window that leaves the disk describes nothing on it. Refused rather than
    // clamped: a table saying something impossible is a table to distrust, and
    // silently shrinking it would publish a volume nobody wrote.
    if (!rc.fits(windowOf(entry), disk.sector_count)) {
        var line = driver.Line{};
        _ = line.str("[partition-manager] partition outside disk, skipped: ").str(idSlice(&disk.canonical_id));
        _ = line.str(" slot=").dec(entry.slot);
        line.end();
        return;
    }

    const desc = describeEntry(disk, entry, scheme) orelse return;
    const instance = driver.blockFingerprint(idSlice(&desc.canonical_id));
    if (instance == 0) return;

    const slot = claimPartitionSlot() orelse {
        driver.log("[partition-manager] partition table full; a partition went unpublished");
        return;
    };
    const part = &g_parts[slot];
    part.* = .{
        .instance = instance,
        .disk = disk_index,
        .lba_start = entry.lba_start,
        .lba_count = entry.lba_count,
        .desc = desc,
        .in_use = true,
    };
    g_part_count += 1;
    publishPartition(part, slot, devmgr);
}

/// Build the descriptor one table entry publishes as, or null when it has no
/// usable canonical id.
///
/// Shared by a first publish and a rescan's restatement, so the two cannot
/// describe the same entry differently -- a rescan that derived one field by a
/// second route would rewrite the inventory's record on every pass and look
/// like a table that keeps changing.
fn describeEntry(disk: *const Disk, entry: pt.Partition, scheme: pt.Scheme) ?driver.BlockDescriptor {
    var desc = driver.BlockDescriptor{};
    desc.backend = disk.backend;
    desc.unit = disk.unit;
    desc.partition = entry.slot;
    desc.scheme = switch (scheme) {
        .none => @intCast(abi.PARTITION_SCHEME_NONE),
        .mbr => @intCast(abi.PARTITION_SCHEME_MBR),
        .gpt => @intCast(abi.PARTITION_SCHEME_GPT),
    };
    // Always UNKNOWN here. Recognising a filesystem is the volume manager's
    // job, not the partition table parser's -- see
    // docs/architecture/37-volume-manager.md for why it does not belong in this
    // component and cannot live in the filesystem drivers either.
    desc.fs_type = @intCast(abi.FS_TYPE_UNKNOWN);
    desc.sector_bytes = SECTOR_BYTES;
    desc.flags = @intCast(abi.BLOCK_DESCRIPTOR_FLAG_PRESENT);
    desc.lba_start = entry.lba_start;
    desc.lba_count = entry.lba_count;
    desc.type_guid = entry.type_guid;
    desc.part_guid = entry.part_guid;
    desc.mbr_type = entry.mbr_type;
    desc.label = entry.label;
    if (!buildPartitionId(idSlice(&disk.canonical_id), entry.slot, &desc.canonical_id)) return null;
    return desc;
}

/// The first free slot in `g_parts`.
///
/// Reused rather than appended, because a rescan retires and publishes: a disk
/// whose table is edited a few times would exhaust a table that only ever grew,
/// while holding far fewer live partitions than it has room for.
fn claimPartitionSlot() ?usize {
    for (&g_parts, 0..) |*p, i| {
        if (!p.in_use) return i;
    }
    return null;
}

/// Reduce a parsed table entry to what the rescan diff compares.
fn windowOf(entry: pt.Partition) rc.Window {
    return .{ .slot = entry.slot, .lba_start = entry.lba_start, .lba_count = entry.lba_count };
}

/// Reduce a published partition to what the rescan diff compares. The slot
/// lives in the descriptor, which is the copy a peer holds, so the diff and
/// every consumer of the device agree on which table entry it came from.
fn windowOfPart(part: *const Partition) rc.Window {
    return .{ .slot = part.desc.partition, .lba_start = part.lba_start, .lba_count = part.lba_count };
}

// --- probing -----------------------------------------------------------------

/// What reading a disk's table produced.
///
/// `empty` and `unreadable` are the distinction a RESCAN turns on, and keeping
/// them apart is the difference between an edit and a catastrophe. A disk that
/// genuinely holds no table should have every partition device retired; a disk
/// whose sectors could not be read holds whatever it held before, and retiring
/// against that answer would delete a live filesystem's device because one read
/// timed out. A first probe may treat them alike -- it has nothing published to
/// lose -- which is exactly why the two were conflated until a rescan needed
/// them apart.
const TableRead = union(enum) {
    /// A table parsed. `g_table` holds it.
    table: pt.Scheme,
    /// The disk was read and carries no partition table.
    empty,
    /// The table could not be read or could not be trusted: a failed transfer,
    /// a GPT header whose entry array did not parse, or a protective MBR with
    /// no usable GPT behind it.
    unreadable,
};

/// Read a disk's partition table into `g_table`.
///
/// GPT first, then MBR, then nothing. GPT is tried first because a GPT disk
/// carries a protective MBR that an MBR-first probe would read as one partition
/// spanning the device.
fn readTable(disk_index: usize) TableRead {
    const disk = &g_disks[disk_index];
    g_table = .{};

    // GPT: the header lives at LBA 1.
    if (readSectors(disk, 1, 1, g_sector[0..])) {
        if (pt.parseGptHeader(g_sector[0..])) |header| {
            if (scanGptEntries(disk, header, &g_table)) return .{ .table = .gpt };
            // A header that parsed but whose entries did not is a damaged GPT,
            // not an MBR disk. Falling through to the legacy table would read
            // its protective entry as a real partition covering everything.
            var line = driver.Line{};
            _ = line.str("[partition-manager] gpt entries invalid on ").str(idSlice(&disk.canonical_id));
            line.end();
            return .unreadable;
        }
    }

    if (readSectors(disk, 0, 1, g_sector[0..])) {
        switch (pt.parseMbr(g_sector[0..], &g_table)) {
            .ok => return .{ .table = .mbr },
            .protective => {
                // The disk claims GPT but its header did not parse above.
                var line = driver.Line{};
                _ = line.str("[partition-manager] protective mbr without a usable gpt on ");
                _ = line.str(idSlice(&disk.canonical_id));
                line.end();
                return .unreadable;
            },
            .absent => return .empty,
        }
    }

    // Neither sector could be read. Distinct from a disk that answered and
    // carries nothing, which `.absent` above reports.
    return .unreadable;
}

/// Read a disk's table and publish what it holds. The FIRST read of a disk,
/// which has nothing published to reconcile against.
fn probeDisk(disk_index: usize, devmgr: i32) void {
    const disk = &g_disks[disk_index];
    switch (readTable(disk_index)) {
        .table => |scheme| {
            disk.scheme = scheme;
            publishTable(disk_index, &g_table, devmgr);
        },
        // No table. Publish nothing and leave the disk alone: its own class
        // instance already serves it, which is what makes partition tables
        // optional.
        .empty, .unreadable => {
            var line = driver.Line{};
            _ = line.str("[partition-manager] no partition table on ").str(idSlice(&disk.canonical_id));
            line.end();
        },
    }
}

/// Stream the GPT entry array through the parser a sector at a time. The array
/// can reach 32 sectors, which does not fit one read, and the CRC covers all of
/// it however few entries are retained.
fn scanGptEntries(disk: *const Disk, header: pt.GptHeader, out: *pt.Table) bool {
    g_scan = pt.GptEntryScan.init(header);
    const total = header.entryBytes();
    var fed: u64 = 0;
    var lba = header.entry_lba;
    while (fed < total) {
        if (!readSectors(disk, lba, 1, g_sector[0..])) return false;
        const room = total - fed;
        const take: usize = if (room < pt.SECTOR_BYTES) @intCast(room) else pt.SECTOR_BYTES;
        g_scan.feed(g_sector[0..take]);
        fed += take;
        lba += 1;
    }
    if (g_scan.finish()) |table| {
        out.* = table;
        return true;
    }
    return false;
}

fn publishTable(disk_index: usize, table: *const pt.Table, devmgr: i32) void {
    var i: usize = 0;
    while (i < table.count) : (i += 1) {
        addPartition(disk_index, table.entries[i], table.scheme, devmgr);
    }
}

// --- rescan ------------------------------------------------------------------

/// Re-read `disk_index`'s table and reconcile the partitions published for it
/// against what it now says.
///
/// Returns how many retirements were SCHEDULED -- the rescan is not finished
/// when this returns, because a retirement is answered by two peers later --
/// or null when the table could not be read, in which case nothing was
/// touched at all.
///
/// Refusing to act on an unreadable table is the one decision here that is not
/// symmetric with a first probe. A probe that cannot read a disk publishes
/// nothing and has lost nothing; a rescan that treated the same answer as "no
/// table" would retire every partition of a live disk because one transfer
/// failed.
fn rescanDisk(disk_index: usize, rescan: ?usize) ?u32 {
    const disk = &g_disks[disk_index];

    var desired: [pt.MAX_PARTITIONS]rc.Window = [_]rc.Window{.{}} ** pt.MAX_PARTITIONS;
    var want: usize = 0;
    switch (readTable(disk_index)) {
        .unreadable => return null,
        .empty => disk.scheme = .none,
        .table => |scheme| {
            disk.scheme = scheme;
            while (want < g_table.count) : (want += 1) desired[want] = windowOf(g_table.entries[want]);
            want = rc.retainFitting(desired[0..want], disk.sector_count);
        },
    }

    // The partitions this disk has published, excluding any already being
    // withdrawn. A retirement in flight is the PREVIOUS table's answer, and
    // matching this table against it would rescue a device that is on its way
    // out of both the inventory and the class.
    var have: [MAX_PARTITIONS]rc.Window = [_]rc.Window{.{}} ** MAX_PARTITIONS;
    var have_part: [MAX_PARTITIONS]usize = [_]usize{0} ** MAX_PARTITIONS;
    var held: usize = 0;
    for (&g_parts, 0..) |*part, index| {
        if (!part.in_use or part.disk != disk_index) continue;
        if (retireOfPart(index) != null) continue;
        have[held] = windowOfPart(part);
        have_part[held] = index;
        held += 1;
    }

    var i: usize = 0;
    while (i < want) : (i += 1) {
        const entry = tableEntry(desired[i].slot) orelse continue;
        switch (rc.classify(have[0..held], desired[i])) {
            // Strictly nothing. A filesystem may be mounted on this partition,
            // and re-registering its class or re-announcing its descriptor
            // would disturb that to restate what is already true.
            .keep => {},
            .update => |k| updatePartition(have_part[k], entry, disk.scheme),
            .publish => addPartition(disk_index, entry, disk.scheme, g_devmgr),
        }
    }

    var scheduled: u32 = 0;
    var k: usize = 0;
    while (k < held) : (k += 1) {
        if (!rc.orphaned(desired[0..want], have[k])) continue;
        if (beginRetire(have_part[k], rescan)) scheduled += 1;
    }
    return scheduled;
}

/// The entry in the table just read that occupies `slot`, if any. Slots are
/// unique within a table, so this is exact.
fn tableEntry(slot: u32) ?pt.Partition {
    var i: usize = 0;
    while (i < g_table.count) : (i += 1) {
        if (g_table.entries[i].slot == slot) return g_table.entries[i];
    }
    return null;
}

/// Restate what an already-published partition describes, keeping its identity.
///
/// The class registration is deliberately untouched. A partition's canonical id
/// is built from the disk and the table slot, neither of which a resize moves,
/// so its class instance -- a fingerprint of that id -- is still correct; and a
/// filesystem holding this device keeps reaching it throughout. Only the
/// descriptor changed, and only the device manager holds a copy of that.
fn updatePartition(part_index: usize, entry: pt.Partition, scheme: pt.Scheme) void {
    var part = &g_parts[part_index];
    const disk = &g_disks[part.disk];
    const desc = describeEntry(disk, entry, scheme) orelse return;
    part.lba_start = entry.lba_start;
    part.lba_count = entry.lba_count;
    part.desc = desc;
    announcePartition(part, part_index, g_devmgr);

    var line = driver.Line{};
    _ = line.str("[partition-manager] partition moved id=").str(idSlice(&part.desc.canonical_id));
    _ = line.str(" lba=").dec(part.lba_start).str(" count=").dec(part.lba_count);
    line.end();
}

/// Begin withdrawing a partition. Returns whether a retirement is now in
/// flight.
///
/// The partition keeps serving until every stage has agreed it is gone, which
/// is not caution for its own sake: a filesystem may be mounted on it and this
/// driver cannot tell.
fn beginRetire(part_index: usize, rescan: ?usize) bool {
    const part = &g_parts[part_index];
    const id = idSlice(&part.desc.canonical_id);
    // An id filling the field with no terminator is not one: the device manager
    // requires a terminator within the bytes it is given, and a length past the
    // field would read beyond it here.
    if (id.len == 0 or id.len >= driver.BLOCK_ID_MAX) {
        driver.log("[partition-manager] cannot retire a partition whose id is unterminated");
        return false;
    }
    if (g_retire_bid < 0 or g_claim_bid < 0) {
        var line = driver.Line{};
        _ = line.str("[partition-manager] cannot retire ").str(id);
        _ = line.str("; bring-up did not get the buffers a retirement needs");
        line.end();
        return false;
    }
    if (g_devmgr < 0) {
        var line = driver.Line{};
        _ = line.str("[partition-manager] cannot retire ").str(id);
        _ = line.str("; the device manager is unreachable");
        line.end();
        return false;
    }
    resolveVolmgr();
    if (g_volmgr < 0) {
        // Nothing can say whether a filesystem holds this partition, so it is
        // left alone. Refusing is the conservative half of the choice: a
        // retirement that guessed "unheld" would strip a live mount's device
        // out of discovery and the inventory both.
        var line = driver.Line{};
        _ = line.str("[partition-manager] cannot retire ").str(id);
        _ = line.str("; no volume manager to say whether it is mounted");
        line.end();
        return false;
    }
    const slot = claimRetire() orelse {
        driver.log("[partition-manager] retire queue full; a partition stayed published");
        return false;
    };
    const r = &g_retires[slot];
    const offset: u32 = @intCast(slot * driver.BLOCK_ID_MAX);
    // The terminator travels with the id: the device manager requires one
    // within the bytes it is given, and will not guess at the end of a name.
    if (!driver.bufferWrite(g_retire_bid, part.desc.canonical_id[0 .. id.len + 1], offset)) {
        driver.log("[partition-manager] retire id staging failed; a partition stayed published");
        return false;
    }
    r.* = .{
        .part = part_index,
        .stage = .claimwait,
        .rescan = rescan,
        .in_use = true,
    };
    pumpClaims();
    return true;
}

/// Start the next queued claim check, if none is outstanding.
///
/// Serialized rather than fanned out because the descriptor comes back at a
/// fixed offset of one buffer; see Retire.stage. Called whenever a retirement
/// is queued and whenever one leaves the claim stage, so the queue drains
/// without anything polling it.
fn pumpClaims() void {
    for (&g_retires) |*r| {
        if (r.in_use and r.stage == .claimcheck) return;
    }
    for (&g_retires, 0..) |*r, slot| {
        if (!r.in_use or r.stage != .claimwait) continue;
        const part = &g_parts[r.part];
        r.stage = .claimcheck;
        r.request_id = nextRequestId();
        if (driver.send(
            g_volmgr,
            endpoint(),
            op.VOLUME_IPC_IDENTIFY_REQ,
            r.request_id,
            @bitCast(volumeInstanceOf(part)),
            g_claim_bid,
            0,
            0,
        ) != 0) {
            // The question could not be asked, so it is not answered. Treated
            // as held, like every other way of not knowing.
            finishRetire(slot, true);
        }
        return;
    }
}

/// The `volume` class instance of the volume sitting on `part`.
///
/// DERIVED, not looked up: a volume's canonical id is `volume:` prefixed to its
/// backing device's, and its class instance is a fingerprint of that
/// (docs/architecture/37-volume-manager.md §3). A partition with no volume
/// fingerprints to an instance nothing registered, which the volume manager
/// answers with an error -- and that is the right answer, because nothing can
/// be mounted on a device that has no volume.
fn volumeInstanceOf(part: *const Partition) u32 {
    var id: [driver.VOLUME_ID_MAX]u8 = [_]u8{0} ** driver.VOLUME_ID_MAX;
    const prefix = "volume:";
    var n: usize = 0;
    while (n < prefix.len and n + 1 < id.len) : (n += 1) id[n] = prefix[n];
    for (idSlice(&part.desc.canonical_id)) |c| {
        if (n + 1 >= id.len) break;
        id[n] = c;
        n += 1;
    }
    id[n] = 0;
    return driver.blockFingerprint(id[0..n]);
}

/// Resolve the volume manager once, on the first retirement that needs it, and
/// lend it the buffer it writes a descriptor into.
///
/// The grant is per PROCESS, so it is made here with the resolution rather than
/// per request, where a second attempt would be refused as ALREADY_BORROWED.
fn resolveVolmgr() void {
    if (g_volmgr_tried) return;
    g_volmgr_tried = true;
    // One attempt, not a retry loop: this runs on the root task and a rescan is
    // waiting on it. A volume manager that is not up by the time something asks
    // to rewrite a partition table is not going to appear within a few yields.
    const ep = driver.lookupService(g_proc_endpoint, "volume", nextRequestId(), 1) orelse return;
    if (driver.bufferBorrow(ep, g_claim_bid, driver.BUFFER_GRANT_WRITE) == null) return;
    g_volmgr = ep;
}

/// The volume manager has answered a claim check. `described` is false when it
/// replied with an error, which is what it does for a device carrying no
/// volume -- and a device with no volume has nothing holding it.
fn handleClaimReply(msg: *const co.IpcMessage, described: bool) void {
    const slot = retireByRequest(msg.request_id) orelse return;
    const r = &g_retires[slot];
    if (r.stage != .claimcheck) return;
    const part = &g_parts[r.part];

    if (described) {
        var desc = driver.VolumeDescriptor{};
        if (!driver.bufferRead(g_claim_bid, &desc, 0)) {
            // The answer could not be read, so it is not an answer. Treated as
            // held, for the same reason an unreachable volume manager is.
            var line = driver.Line{};
            _ = line.str("[partition-manager] retire refused for ");
            _ = line.str(idSlice(&part.desc.canonical_id));
            _ = line.str("; its volume could not be read");
            line.end();
            finishRetire(slot, true);
            return;
        }
        if ((desc.flags & @as(u32, @intCast(abi.VOLUME_DESCRIPTOR_FLAG_CLAIMED))) != 0) {
            var line = driver.Line{};
            _ = line.str("[partition-manager] retire refused for ");
            _ = line.str(idSlice(&part.desc.canonical_id));
            _ = line.str("; a filesystem is mounted on it");
            line.end();
            finishRetire(slot, true);
            return;
        }
    }

    r.stage = .unpublish;
    r.request_id = nextRequestId();
    const id = idSlice(&part.desc.canonical_id);
    const offset: u32 = @intCast(slot * driver.BLOCK_ID_MAX);
    if (driver.send(
        g_devmgr,
        endpoint(),
        op.DEVMGR_UNPUBLISH_BLOCK_DEVICE,
        r.request_id,
        g_retire_bid,
        @intCast(offset),
        @intCast(id.len + 1),
        0,
    ) != 0) {
        finishRetire(slot, true);
        return;
    }
    pumpClaims();
}

fn claimRetire() ?usize {
    for (&g_retires, 0..) |*r, i| {
        if (!r.in_use) return i;
    }
    return null;
}

/// The retirement withdrawing `part_index`, if one is in flight.
fn retireOfPart(part_index: usize) ?usize {
    for (&g_retires, 0..) |*r, i| {
        if (r.in_use and r.part == part_index) return i;
    }
    return null;
}

fn retireByRequest(request_id: i32) ?usize {
    for (&g_retires, 0..) |*r, i| {
        if (r.in_use and r.request_id == request_id) return i;
    }
    return null;
}

/// The device manager has answered a withdrawal.
///
/// A refusal here is an inventory answer -- a malformed id, or a record it does
/// not hold -- not a mount; whether anything holds the partition was settled by
/// the claim check before this was sent. It ends the retirement either way, and
/// the partition stays published, class registration and all.
fn handleUnpublishReply(msg: *const co.IpcMessage) void {
    const slot = retireByRequest(msg.request_id) orelse return;
    var r = &g_retires[slot];
    if (r.stage != .unpublish) return;
    const part = &g_parts[r.part];

    if (msg.arg0 != 0) {
        var line = driver.Line{};
        _ = line.str("[partition-manager] retire refused for ").str(idSlice(&part.desc.canonical_id));
        _ = line.str(" code=").dec(@as(u32, @bitCast(msg.arg0)));
        line.end();
        finishRetire(slot, true);
        return;
    }

    r.stage = .unregister;
    r.request_id = nextRequestId();
    if (!r.class.start(g_proc_endpoint, endpoint(), "block", part.instance, r.request_id)) {
        // Out of the inventory but still in the class. Reported as a refusal
        // because the retirement did not complete, and logged because the two
        // views of this device now disagree -- which is the one outcome the
        // ordering above exists to make rare rather than impossible.
        var line = driver.Line{};
        _ = line.str("[partition-manager] class retire could not be sent for ");
        _ = line.str(idSlice(&part.desc.canonical_id));
        line.end();
        finishRetire(slot, true);
    }
}

/// The process manager has answered a class withdrawal. `ok` is false when it
/// refused, which leaves the device out of the inventory but still registered.
fn finishClassRetire(slot: usize, ok: bool) void {
    const r = &g_retires[slot];
    if (r.stage != .unregister) return;
    if (!ok) {
        var line = driver.Line{};
        _ = line.str("[partition-manager] class retire refused for ");
        _ = line.str(idSlice(&g_parts[r.part].desc.canonical_id));
        line.end();
        finishRetire(slot, true);
        return;
    }
    var line = driver.Line{};
    _ = line.str("[partition-manager] partition retired id=");
    _ = line.str(idSlice(&g_parts[r.part].desc.canonical_id));
    line.end();
    g_parts[r.part] = .{};
    if (g_part_count > 0) g_part_count -= 1;
    finishRetire(slot, false);
}

/// Release a retirement's slot and account for it against the rescan that
/// scheduled it, answering that rescan when it was the last one outstanding.
fn finishRetire(slot: usize, refused: bool) void {
    const r = &g_retires[slot];
    const rescan = r.rescan;
    const was_claiming = r.stage == .claimcheck;
    r.class.release();
    r.* = .{};
    // A retirement that ended while holding the claim stage leaves it free, and
    // the queue behind it would otherwise never move.
    if (was_claiming) pumpClaims();
    const index = rescan orelse return;
    const pending = &g_rescans[index];
    if (!pending.in_use) return;
    if (refused) pending.refused += 1;
    pending.settled += 1;
    if (pending.diffed and pending.settled >= pending.scheduled) completeRescan(index);
}

fn claimRescan() ?usize {
    for (&g_rescans, 0..) |*s, i| {
        if (!s.in_use) return i;
    }
    return null;
}

/// Whether a rescan of `disk_index` is already running. A second one would
/// diff against a table the first is still reconciling.
fn rescanInFlight(disk_index: usize) bool {
    for (&g_rescans) |*s| {
        if (s.in_use and s.disk == disk_index) return true;
    }
    return false;
}

fn partitionsOn(disk_index: usize) u32 {
    var n: u32 = 0;
    for (&g_parts) |*p| {
        if (p.in_use and p.disk == disk_index) n += 1;
    }
    return n;
}

/// Answer a rescan and release its slot.
fn completeRescan(index: usize) void {
    const pending = &g_rescans[index];
    const count = partitionsOn(pending.disk);
    if (pending.reply_to >= 0) {
        _ = driver.send(
            pending.reply_to,
            endpoint(),
            op.PARTITION_IPC_RESP,
            pending.reply_id,
            0,
            @intCast(count),
            @intCast(pending.refused),
            0,
        );
    }
    var line = driver.Line{};
    _ = line.str("[partition-manager] rescanned ").str(idSlice(&g_disks[pending.disk].canonical_id));
    _ = line.str(" partitions=").dec(count);
    if (pending.refused > 0) _ = line.str(" refused=").dec(pending.refused);
    line.end();
    pending.* = .{};
}

fn rescanError(dest: i32, request_id: i32, code: i32) void {
    _ = driver.send(dest, endpoint(), op.PARTITION_IPC_ERROR, request_id, code, 0, 0, 0);
}

// --- async service contract --------------------------------------------------

/// Definition of one async WASM guest, mirroring `wasmos_sys_wasm_async_config_t`.
/// The runtime, root task, event loop and reply endpoint are scratch the libsys
/// runner fills in; only `resume`, `prepare` and `user` are inputs.
const AsyncServiceConfig = extern struct {
    runtime: co.Runtime = .{},
    root: co.Coroutine = .{},
    event_loop: co.EventLoop = .{},
    reply_endpoint: i32 = 0,
    @"resume": ?co.TaskResume = null,
    prepare: ?*const fn (?*anyopaque, i32, i32, i32, i32) callconv(.c) void = null,
    user: ?*anyopaque = null,
};

export var wasmos_async_service: AsyncServiceConfig = .{
    .@"resume" = rootTask,
    .prepare = prepare,
};

fn loop() *co.EventLoop {
    return &wasmos_async_service.event_loop;
}

/// The endpoint the runner created. Every partition is served on it and every
/// disk is reached from it: a request names its target, so one endpoint carries
/// what an earlier design gave a fan-out of them.
fn endpoint() i32 {
    return wasmos_async_service.reply_endpoint;
}

// --- forwarding --------------------------------------------------------------

fn findPartition(instance: u32) ?*Partition {
    for (&g_parts) |*p| {
        if (p.in_use and p.instance == instance) return p;
    }
    return null;
}

fn findInflightByDownId(request_id: i32) ?*Inflight {
    for (&g_inflight) |*f| {
        if (f.in_use and f.down_request_id == request_id) return f;
    }
    return null;
}

fn claimInflight() ?*Inflight {
    for (&g_inflight) |*f| {
        if (!f.in_use) return f;
    }
    return null;
}

fn sendError(dest: i32, request_id: i32, code: i32) void {
    _ = driver.send(dest, endpoint(), op.BLOCK_IPC_ERROR, request_id, code, 0, 0, 0);
}

/// Answer IDENTIFY for one of our partitions by writing its descriptor into the
/// CLIENT's buffer, exactly as a disk backend answers for a disk. A client
/// cannot tell the two apart, which is the point.
fn handleIdentify(msg: *const co.IpcMessage) void {
    const part = findPartition(@bitCast(msg.arg0)) orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NO_SUCH_UNIT);
        return;
    };
    if (msg.arg1 <= 0) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_BAD_REQUEST);
        return;
    }
    const raw: [*]const u8 = @ptrCast(&part.desc);
    if (!driver.bufferWrite(msg.arg1, raw[0..@sizeOf(driver.BlockDescriptor)], 0)) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR);
        return;
    }
    _ = driver.send(
        msg.source,
        endpoint(),
        op.BLOCK_IPC_IDENTIFY_RESP,
        msg.request_id,
        0,
        @intCast(@sizeOf(driver.BlockDescriptor)),
        0,
        0,
    );
}

/// Forward a transfer to the disk under the addressed partition.
///
/// The client's request is copied with three edits: `target` becomes the DISK's
/// instance, `lba` is rebased onto the partition's window, and a zero-copy
/// destination's borrow is REBORROWED to the disk. The client's pages are still
/// what the device writes — a partition costs an IPC hop, not a copy.
///
/// The reborrow is what makes the pass-through legal. A borrow is held per
/// context, so `dst_borrow_id` as the client sent it names a grant between the
/// CLIENT and THIS driver; forwarding that id gives the disk a handle nothing
/// resolves for it, and its write is refused. `bufferReborrow` mints the disk a
/// handle of its own within the rights this driver was given, and the chain
/// cascade-revokes: whatever the client drops takes the disk's access with it.
///
/// Rights are narrowed to the direction of the transfer. A read needs WRITE on
/// the destination and a write needs READ, and granting only that keeps a
/// backing device from touching a client buffer in the direction its request
/// never asked for.
fn handleTransfer(msg: *const co.IpcMessage) void {
    var req = driver.BlockRequest{};
    if (msg.arg0 <= 0 or msg.arg1 < 0 or
        msg.arg2 < @as(i32, @intCast(@sizeOf(driver.BlockRequest))) or
        !driver.bufferRead(msg.arg0, &req, msg.arg1))
    {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_BAD_REQUEST);
        return;
    }
    if (req.version != driver.BLOCK_REQUEST_VERSION) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_DESCRIPTOR_VERSION);
        return;
    }
    const part = findPartition(req.target) orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NO_SUCH_UNIT);
        return;
    };

    // The bounds clamp. A request starting past the window addresses another
    // volume's sectors, so it is refused rather than truncated; one that merely
    // runs off the end is shortened, and the reply reports what was moved.
    if (req.lba >= part.lba_count or req.sector_count == 0) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_BAD_REQUEST);
        return;
    }
    const room = part.lba_count - req.lba;
    if (req.sector_count > room) req.sector_count = @intCast(room);

    const slot = claimInflight() orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NOT_READY);
        return;
    };

    const disk = &g_disks[part.disk];
    req.target = disk.instance;
    req.lba += part.lba_start;

    var down_borrow: i32 = -1;
    if (req.dst_kind == driver.BLOCK_DST_XFER_BUFFER) {
        if (req.dst_borrow_id <= 0) {
            sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_BAD_REQUEST);
            return;
        }
        const flags: i32 = if (msg.type == op.BLOCK_IPC_WRITE_REQ)
            driver.BUFFER_GRANT_READ
        else
            driver.BUFFER_GRANT_WRITE;
        down_borrow = driver.bufferReborrow(disk.endpoint, req.dst_borrow_id, flags) orelse {
            sendError(msg.source, msg.request_id, status.WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW);
            return;
        };
        req.dst_borrow_id = down_borrow;
    }

    const raw: [*]const u8 = @ptrCast(&req);
    if (g_down_req_bid < 0 or
        !driver.bufferWrite(g_down_req_bid, raw[0..@sizeOf(driver.BlockRequest)], 0))
    {
        if (down_borrow >= 0) driver.bufferUnborrow(down_borrow);
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NOT_READY);
        return;
    }

    const down_id = nextRequestId();
    if (driver.send(
        disk.endpoint,
        endpoint(),
        msg.type,
        down_id,
        g_down_req_bid,
        0,
        @intCast(@sizeOf(driver.BlockRequest)),
        0,
    ) != 0) {
        if (down_borrow >= 0) driver.bufferUnborrow(down_borrow);
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NOT_READY);
        return;
    }

    slot.* = .{
        .client = msg.source,
        .client_request_id = msg.request_id,
        .down_request_id = down_id,
        .down_borrow_id = down_borrow,
        .sectors = req.sector_count,
        .resp_type = if (msg.type == op.BLOCK_IPC_WRITE_REQ)
            op.BLOCK_IPC_WRITE_RESP
        else
            op.BLOCK_IPC_READ_RESP,
        .in_use = true,
    };
}

/// Relay a backing device's reply to whoever asked for it, under the request id
/// THEY used. A reply nobody is waiting for is dropped rather than forwarded:
/// the downstream id space is ours, so an unmatched one is our bug and not
/// something a client should be told about.
fn handleDownstreamReply(msg: *const co.IpcMessage) void {
    const slot = findInflightByDownId(msg.request_id) orelse return;
    // The device is done with the client's buffer, so its access ends here --
    // before the client is told, since the reply is what frees the client to
    // reuse the buffer and a grant still standing would refuse the next request.
    // Dropped on the error path too: a refused transfer leaves the grant behind
    // just as surely as a served one.
    if (slot.down_borrow_id >= 0) {
        driver.bufferUnborrow(slot.down_borrow_id);
    }
    if (msg.type == op.BLOCK_IPC_ERROR) {
        sendError(slot.client, slot.client_request_id, msg.arg0);
    } else {
        _ = driver.send(
            slot.client,
            endpoint(),
            slot.resp_type,
            slot.client_request_id,
            0,
            msg.arg1,
            0,
            0,
        );
    }
    slot.in_use = false;
}

/// An existence event for the `block` class: arg0 names the kind, arg1 the
/// instance, arg2 the provider's endpoint, arg3 its pid.
///
/// REMOVE is deliberately not acted on. Retiring a disk means unregistering the
/// partitions published from it and telling the device manager they are gone,
/// which is a teardown path with no caller today -- no shipped driver
/// unregisters. Dropping our record alone would be worse than doing nothing: the
/// partitions would stay in the registry addressing a disk we could no longer
/// reach.
/// TODO: retire a disk's partitions when its provider goes away.
fn handleClassEvent(msg: *const co.IpcMessage) void {
    if (msg.arg0 != @as(i32, @intCast(abi.SVC_CLASS_EVENT_ADD))) return;
    noteArrival(@bitCast(msg.arg1), msg.arg2);
}

/// Queue a re-read of one disk's table (PARTITION_IPC_RESCAN_REQ). arg0 is the
/// DISK's `block` class instance.
///
/// Queued rather than served here, and answered later still. Reading a table is
/// synchronous and belongs on the root task, and the retirements it schedules
/// are answered by their own peers after that -- so the result does not exist
/// at any point this handler could return it.
fn handleRescan(msg: *const co.IpcMessage) void {
    queueArrival(@bitCast(msg.arg0), -1, true, msg.source, msg.request_id);
}

/// A reply the process manager owes a class withdrawal. SVC_IPC_ERROR carries
/// the opcode in arg0 and a packed code in arg1, and both forms carry the
/// request id, which is what ties either to its retirement.
fn handleClassRetireReply(msg: *const co.IpcMessage, ok: bool) void {
    const slot = retireByRequest(msg.request_id) orelse return;
    finishClassRetire(slot, ok);
}

fn onMessage(user: ?*anyopaque, msg: *const co.IpcMessage) callconv(.c) void {
    _ = user;
    switch (msg.type) {
        op.BLOCK_IPC_IDENTIFY_REQ => handleIdentify(msg),
        op.BLOCK_IPC_READ_REQ, op.BLOCK_IPC_WRITE_REQ => handleTransfer(msg),
        op.BLOCK_IPC_READ_RESP, op.BLOCK_IPC_WRITE_RESP, op.BLOCK_IPC_ERROR => handleDownstreamReply(msg),
        op.SVC_IPC_CLASS_EVENT => handleClassEvent(msg),
        op.PARTITION_IPC_RESCAN_REQ => handleRescan(msg),
        op.VOLUME_IPC_IDENTIFY_RESP => handleClaimReply(msg, true),
        op.VOLUME_IPC_ERROR => handleClaimReply(msg, false),
        op.DEVMGR_UNPUBLISH_RESP => handleUnpublishReply(msg),
        op.SVC_IPC_UNREGISTER_CLASS_RESP => handleClassRetireReply(msg, true),
        op.SVC_IPC_ERROR => handleClassRetireReply(msg, false),
        else => {},
    }
}

// --- discovery ---------------------------------------------------------------

/// Lend the probe buffers to a backend, once per backend process.
///
/// The client owns a transfer buffer and the server is a grantee, so a backend
/// cannot write a descriptor into our buffer, nor read a request out of it,
/// until we have lent it. Grants are per PROCESS, not per endpoint or per
/// device: an ATA controller serving two drives answers both on one endpoint, so
/// one grant covers them and a second attempt would be refused as
/// ALREADY_BORROWED. Deduplicating here keeps that from looking like a failure.
fn grantBackend(ep: i32) void {
    for (g_granted[0..g_granted_count]) |seen| {
        if (seen == ep) return;
    }
    if (g_granted_count >= g_granted.len) return;
    _ = driver.bufferBorrow(ep, g_probe_bid, driver.BUFFER_GRANT_WRITE);
    _ = driver.bufferBorrow(ep, g_probe_req_bid, driver.BUFFER_GRANT_READ);
    _ = driver.bufferBorrow(ep, g_down_req_bid, driver.BUFFER_GRANT_READ);
    g_granted[g_granted_count] = ep;
    g_granted_count += 1;
}

/// Resolve the device manager's inventory endpoint and lend it the publish
/// buffer. Resolved once; the result, including a failure, stands for the life
/// of the process.
fn resolveDevmgr() void {
    const ep = driver.lookupService(g_proc_endpoint, "devmgr.inv", nextRequestId(), 64) orelse {
        driver.log("[partition-manager] devmgr inventory unavailable; partitions unpublished to it");
        return;
    };
    // Lend the publish buffer before the first publish. Without this the device
    // manager refuses every descriptor at the read, and since a partition still
    // registers under the `block` class the failure looks cosmetic -- while
    // every SUBSYSTEM=="partition" rule in the system matches nothing, because
    // rules match the registry.
    if (g_publish_bid < 0 or
        driver.bufferBorrow(ep, g_publish_bid, driver.BUFFER_GRANT_READ) == null)
    {
        driver.log("[partition-manager] publish buffer grant failed; partitions unpublished to devmgr");
        return;
    }
    // The same for the retire buffer, which carries the canonical id of a
    // partition being withdrawn. Lent here rather than at the first retirement,
    // because a grant is per process and a second attempt is refused as
    // ALREADY_BORROWED.
    if (g_retire_bid < 0 or
        driver.bufferBorrow(ep, g_retire_bid, driver.BUFFER_GRANT_READ) == null)
    {
        driver.log("[partition-manager] retire buffer grant failed; partitions cannot be retired");
        return;
    }
    g_devmgr = ep;
}

/// Probe one `block` provider and publish whatever table it holds.
///
/// Answers whether the provider was taken on as a disk, which is not the same as
/// whether it held partitions: a disk with no table is still ours, and is still
/// counted, so a repeated arrival does not re-probe it.
fn probeProvider(instance: u32, provider_endpoint: i32) bool {
    if (g_disk_count >= MAX_DISKS) {
        driver.log("[partition-manager] disk table full; a block provider went unprobed");
        return false;
    }
    grantBackend(provider_endpoint);

    var desc = driver.BlockDescriptor{};
    if (!identifyDisk(provider_endpoint, instance, &desc)) return false;
    if (desc.version != driver.BLOCK_DESCRIPTOR_VERSION) return false;
    // A partition is itself a `block` provider, so an enumeration -- and every
    // arrival event -- sees our own registrations. Only a whole disk carries
    // partition 0.
    if (desc.partition != 0) return false;

    const index = g_disk_count;
    g_disks[index] = .{
        .instance = instance,
        .endpoint = provider_endpoint,
        .sector_count = desc.lba_count,
        .canonical_id = desc.canonical_id,
        .backend = desc.backend,
        .unit = desc.unit,
        .in_use = true,
    };
    g_disk_count += 1;
    probeDisk(index, g_devmgr);
    return true;
}

fn diskKnown(instance: u32) bool {
    for (g_disks[0..g_disk_count]) |d| {
        if (d.in_use and d.instance == instance) return true;
    }
    return false;
}

/// A disk's canonical id, for reporting. A class instance is a FINGERPRINT of
/// this string, so a message carrying only the instance cannot be tied back to
/// the device any other service names -- and every service names it by id.
fn diskId(instance: u32) []const u8 {
    for (g_disks[0..g_disk_count]) |*d| {
        if (d.in_use and d.instance == instance) return idSlice(&d.canonical_id);
    }
    return "?";
}

/// Record a `block` provider to be probed, and wake the root task to do it.
fn noteArrival(instance: u32, provider_endpoint: i32) void {
    queueArrival(instance, provider_endpoint, false, -1, 0);
}

/// Queue work on a disk and wake the root task.
///
/// `force` re-reads the table of a disk already known, which only a RESCAN
/// does; `reply_to` is answered once that has settled, and is negative for an
/// arrival from the class because nothing is waiting on one.
///
/// Two providers are dropped rather than probed. Our OWN endpoint, because
/// every partition we publish registers under the same class and comes back to
/// us as an arrival: probing one would send IDENTIFY to ourselves and then block
/// on a reply only our own message loop could produce, which wedges the process.
/// And an instance already in the disk table, which is what makes the overlap
/// between the subscription and the opening enumeration harmless -- but only
/// for an arrival, since re-reading a known disk is the whole of a rescan.
fn queueArrival(instance: u32, provider_endpoint: i32, force: bool, reply_to: i32, reply_id: i32) void {
    if (provider_endpoint >= 0 and provider_endpoint == endpoint()) {
        if (reply_to >= 0) rescanError(reply_to, reply_id, status.WASMOS_ERR_BLOCK_DEV_BAD_REQUEST);
        return;
    }
    if (!force and diskKnown(instance)) return;
    for (&g_pending) |*a| {
        // An arrival already queued for this disk covers a rescan of it too, but
        // only once it has been upgraded to one: a queued rescan must not be
        // downgraded by a plain arrival landing behind it, and a plain arrival
        // must not answer a request nobody made.
        if (a.in_use and a.instance == instance) {
            if (reply_to >= 0 and a.reply_to < 0) {
                a.force = true;
                a.reply_to = reply_to;
                a.reply_id = reply_id;
                return;
            }
            if (reply_to >= 0) break;
            return;
        }
    }
    for (&g_pending) |*a| {
        if (a.in_use) continue;
        a.* = .{
            .instance = instance,
            .endpoint = provider_endpoint,
            .in_use = true,
            .force = force,
            .reply_to = reply_to,
            .reply_id = reply_id,
        };
        if (g_root_parked) {
            g_root_parked = false;
            _ = g_wake_promise.resolve(0);
        }
        return;
    }
    driver.log("[partition-manager] arrival queue full; a block provider went unprobed");
    if (reply_to >= 0) rescanError(reply_to, reply_id, status.WASMOS_ERR_BLOCK_DEV_QUEUE_FULL);
}

/// The disk table index holding `instance`, if it is a disk we probed.
fn diskIndex(instance: u32) ?usize {
    for (g_disks[0..g_disk_count], 0..) |*d, i| {
        if (d.in_use and d.instance == instance) return i;
    }
    return null;
}

/// Probe every queued arrival. Runs on the root task, which is this process's
/// own thread of control: a probe is synchronous, and running it from the
/// message handler would block the loop that handler was dispatched from.
///
/// Forwarding stalls for the duration, since the whole process blocks in each
/// downstream call. That is bounded -- one IDENTIFY plus at most 34 sector reads
/// per disk -- and it is why the arrival is queued rather than probed inline.
/// TODO: drive the probe over the loop's IpcFuture instead, so a disk arriving
/// under load does not pause transfers on the disks already published. That is a
/// state machine over IDENTIFY -> GPT header -> entry sectors -> MBR, not a
/// rewrite of the parser.
fn drainArrivals() void {
    for (&g_pending) |*a| {
        if (!a.in_use) continue;
        const instance = a.instance;
        const provider_endpoint = a.endpoint;
        const force = a.force;
        const reply_to = a.reply_to;
        const reply_id = a.reply_id;
        // Cleared BEFORE the work: the drain walks the queue while the work it
        // does can add to it, and a slot still holding the entry being served
        // would be read as a second request for the same disk.
        a.* = .{};
        if (force) {
            runRescan(instance, reply_to, reply_id);
            continue;
        }
        if (diskKnown(instance)) continue;
        if (!probeProvider(instance, provider_endpoint)) continue;
        var line = driver.Line{};
        _ = line.str("[partition-manager] disk probed id=").str(diskId(instance));
        _ = line.str(" disks=").dec(g_disk_count).str(" partitions=").dec(g_part_count);
        line.end();
    }
}

/// Reconcile one disk against its table on the root task, and either answer the
/// caller now or leave the answer to the retirements still in flight.
fn runRescan(instance: u32, reply_to: i32, reply_id: i32) void {
    const disk_index = diskIndex(instance) orelse {
        // A disk this driver never took on. Not an error worth guessing about:
        // a caller that just wrote a table to a device we do not serve has
        // nothing here to reconcile.
        if (reply_to >= 0) rescanError(reply_to, reply_id, status.WASMOS_ERR_BLOCK_DEV_NO_SUCH_UNIT);
        return;
    };
    if (rescanInFlight(disk_index)) {
        if (reply_to >= 0) rescanError(reply_to, reply_id, status.WASMOS_ERR_BLOCK_DEV_QUEUE_FULL);
        return;
    }
    const slot = claimRescan() orelse {
        if (reply_to >= 0) rescanError(reply_to, reply_id, status.WASMOS_ERR_BLOCK_DEV_QUEUE_FULL);
        return;
    };
    // Claimed before the diff, because every retirement the diff schedules is
    // stamped with this index and answers back through it.
    g_rescans[slot] = .{
        .reply_to = reply_to,
        .reply_id = reply_id,
        .disk = disk_index,
        .in_use = true,
    };
    const scheduled = rescanDisk(disk_index, slot) orelse {
        // The table could not be read, so nothing was reconciled and nothing
        // retired. Reported rather than treated as an empty table, which would
        // retire every partition of a disk that is merely unreachable.
        g_rescans[slot] = .{};
        if (reply_to >= 0) rescanError(reply_to, reply_id, status.WASMOS_ERR_BLOCK_DEV_READ_FAILED);
        return;
    };
    var pending = &g_rescans[slot];
    if (!pending.in_use) return;
    pending.scheduled = scheduled;
    pending.diffed = true;
    if (pending.settled >= scheduled) completeRescan(slot);
}

/// Subscribe to the `block` class, then probe every disk already registered.
///
/// The subscription comes FIRST and the enumeration second, which is the order
/// that leaves no gap: a disk registering between the two fires an event that is
/// waiting when the loop starts, whereas subscribing afterwards would miss it
/// silently. The two overlapping is expected and costs nothing, because an
/// arrival for a disk already in the table is dropped.
///
/// Enumerating once was the whole defect: virtio-blk negotiates a PCI device
/// before it registers, so it publishes its disk well after this runs, and
/// without the subscription its partitions were never seen.
fn discoverDisks() void {
    resolveDevmgr();

    if (!driver.subscribeClass(g_proc_endpoint, endpoint(), "block", nextRequestId())) {
        driver.log("[partition-manager] block class subscribe failed; disks arriving later go unprobed");
    }

    var providers: [MAX_DISKS]driver.ClassEntry = undefined;
    const total = driver.lookupClass(g_proc_endpoint, "block", providers[0..], nextRequestId()) orelse {
        driver.log("[partition-manager] block class lookup failed; no disks probed");
        return;
    };
    const found: usize = @intCast(total);
    const seen: usize = if (found < providers.len) found else providers.len;
    if (found > providers.len) {
        var line = driver.Line{};
        _ = line.str("[partition-manager] ").dec(found);
        _ = line.str(" block providers present, probing the first ").dec(providers.len);
        line.end();
    }

    var i: usize = 0;
    while (i < seen) : (i += 1) {
        if (diskKnown(providers[i].instance)) continue;
        _ = probeProvider(providers[i].instance, @intCast(providers[i].endpoint));
    }
}
fn prepare(user: ?*anyopaque, arg0: i32, arg1: i32, arg2: i32, arg3: i32) callconv(.c) void {
    _ = user;
    _ = arg0;
    _ = arg1;
    _ = arg2;
    _ = arg3;

    loop().default_on_message = @ptrCast(&onMessage);
    g_proc_endpoint = driver.procEndpoint();

    g_block_phys = driver.blockBufferPhys() orelse {
        driver.log("[partition-manager] no block buffer; cannot read a partition table");
        return;
    };
    g_probe_bid = driver.bufferAcquire(@sizeOf(driver.BlockDescriptor)) orelse {
        driver.log("[partition-manager] descriptor buffer unavailable");
        return;
    };
    g_down_req_bid = driver.bufferAcquire(@sizeOf(driver.BlockRequest)) orelse {
        driver.log("[partition-manager] request buffer unavailable");
        return;
    };
    g_probe_req_bid = driver.bufferAcquire(@sizeOf(driver.BlockRequest)) orelse {
        driver.log("[partition-manager] probe request buffer unavailable");
        return;
    };
    g_publish_bid = driver.bufferAcquire(
        MAX_PARTITIONS * @sizeOf(driver.BlockDescriptor),
    ) orelse {
        driver.log("[partition-manager] publish buffer unavailable");
        return;
    };
    g_retire_bid = driver.bufferAcquire(MAX_PARTITIONS * driver.BLOCK_ID_MAX) orelse {
        driver.log("[partition-manager] retire buffer unavailable");
        return;
    };
    g_claim_bid = driver.bufferAcquire(@sizeOf(driver.VolumeDescriptor)) orelse {
        driver.log("[partition-manager] claim buffer unavailable");
        return;
    };

    // Bind the service NAME before anything is published.
    //
    // Every partition registers under this same name as a side effect of
    // claiming its class instance, so the name exists as soon as one partition
    // does -- and on a disk with no table, never. That is precisely the disk a
    // rescan is wanted for: `fdisk` writing a FIRST table onto an unpartitioned
    // device has nothing to tell afterwards, because the manager it must reach
    // is unreachable by name. Registering here makes the name a property of the
    // service rather than of its output. Re-registration by the owner re-points
    // an entry rather than being refused, so the per-partition registrations
    // below are unaffected.
    if (driver.registerService(g_proc_endpoint, endpoint(), "partition", "", 0, nextRequestId()) == null) {
        driver.log("[partition-manager] service name registration failed; rescans cannot be requested");
    }

    discoverDisks();

    var line = driver.Line{};
    _ = line.str("[partition-manager] ready disks=").dec(g_disk_count);
    _ = line.str(" partitions=").dec(g_part_count);
    line.end();

    driver.notifyReady(g_proc_endpoint);
}

/// The root task probes disks that arrive after bring-up, and otherwise parks.
///
/// Transfers need nothing from it -- they are driven by messages through
/// `onMessage`, which run on the loop -- so between arrivals it is parked, and
/// the runner's poll, which blocks on the loop's select set, is what makes an
/// idle partition manager sleep rather than spin.
///
/// A stackless task is resumed from the TOP, so draining before the await is
/// what makes this a loop: `handleClassEvent` queues an arrival and settles the
/// promise, the runtime resumes here, the drain runs, and the task parks again
/// on a fresh future.
fn rootTask(user: ?*anyopaque, out_value: *usize) callconv(.c) i32 {
    _ = user;
    _ = out_value;
    drainArrivals();
    g_wake_future.init(&g_wake_promise);
    g_root_parked = true;
    switch (g_wake_future.awaitValue()) {
        .pending => return co.TaskResult.yielded,
        else => {
            // Settled before the await, or the runtime refused it. Either way
            // nothing is parked, so loop rather than claim to be.
            g_root_parked = false;
            return co.TaskResult.yielded;
        },
    }
}
