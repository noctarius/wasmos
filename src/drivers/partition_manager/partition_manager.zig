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
//! the partition's window and `sector_count` clamped to it. The data destination
//! is passed through untouched: the backing device writes the CLIENT's pages
//! directly, so a partition costs one IPC hop and no copy.
const driver = @import("driver.zig");
const co = @import("coroutine.zig");
const op = @import("wasmos_opcodes.zig");
const status = @import("wasmos_status.zig");
const abi = @import("wasmos_constants.zig");
const pt = @import("partition_table.zig");

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

/// Backend endpoints this process has lent its probe buffers to.
///
/// Grants are per PROCESS, not per endpoint or per device, so one entry covers
/// every disk a backend serves and a second attempt would be refused as
/// ALREADY_BORROWED. Kept for the life of the process rather than rebuilt per
/// probe, because a late-arriving disk may be served by a backend already lent
/// to -- an ATA controller publishing its second drive, say.
var g_granted: [MAX_DISKS]i32 = [_]i32{-1} ** MAX_DISKS;
var g_granted_count: usize = 0;

/// Disks that have registered under the `block` class but not yet been probed.
///
/// An arrival is queued rather than probed where it is received: a probe is
/// synchronous and a message handler must not block the loop it runs on. The
/// root task drains this.
const Arrival = struct {
    instance: u32 = 0,
    endpoint: i32 = -1,
    in_use: bool = false,
};
/// One slot per disk this process can hold, which is the most that can ever be
/// waiting: an arrival beyond MAX_DISKS could not be recorded even if probed.
var g_pending: [MAX_DISKS]Arrival = [_]Arrival{.{}} ** MAX_DISKS;

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
        _ = line.str("[partmgr] class register failed id=").str(idSlice(&part.desc.canonical_id));
        line.end();
        return;
    }

    if (devmgr >= 0 and g_publish_bid >= 0 and slot < MAX_PARTITIONS) {
        const offset: u32 = @intCast(slot * @sizeOf(driver.BlockDescriptor));
        const raw: [*]const u8 = @ptrCast(&part.desc);
        if (driver.bufferWrite(g_publish_bid, raw[0..@sizeOf(driver.BlockDescriptor)], offset)) {
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
    }

    var line = driver.Line{};
    _ = line.str("[partmgr] partition id=").str(idSlice(&part.desc.canonical_id));
    _ = line.str(" instance=").dec(part.instance);
    _ = line.str(" lba=").dec(part.lba_start).str(" count=").dec(part.lba_count);
    line.end();
}

/// Turn one parsed table entry into a published partition.
fn addPartition(disk_index: usize, entry: pt.Partition, scheme: pt.Scheme, devmgr: i32) void {
    if (g_part_count >= MAX_PARTITIONS) return;
    const disk = &g_disks[disk_index];

    // A window that leaves the disk describes nothing on it. Refused rather than
    // clamped: a table saying something impossible is a table to distrust, and
    // silently shrinking it would publish a volume nobody wrote.
    if (entry.lba_start >= disk.sector_count or
        entry.lba_count == 0 or
        entry.lba_count > disk.sector_count - entry.lba_start)
    {
        var line = driver.Line{};
        _ = line.str("[partmgr] partition outside disk, skipped: ").str(idSlice(&disk.canonical_id));
        _ = line.str(" slot=").dec(entry.slot);
        line.end();
        return;
    }

    const slot = g_part_count;
    var part = &g_parts[slot];
    part.* = .{};
    part.disk = disk_index;
    part.lba_start = entry.lba_start;
    part.lba_count = entry.lba_count;

    var desc = driver.BlockDescriptor{};
    desc.backend = disk.backend;
    desc.unit = disk.unit;
    desc.partition = entry.slot;
    desc.scheme = switch (scheme) {
        .none => @intCast(abi.PARTITION_SCHEME_NONE),
        .mbr => @intCast(abi.PARTITION_SCHEME_MBR),
        .gpt => @intCast(abi.PARTITION_SCHEME_GPT),
    };
    desc.fs_type = @intCast(abi.FS_TYPE_UNKNOWN);
    desc.sector_bytes = SECTOR_BYTES;
    desc.flags = @intCast(abi.BLOCK_DESCRIPTOR_FLAG_PRESENT);
    desc.lba_start = entry.lba_start;
    desc.lba_count = entry.lba_count;
    desc.type_guid = entry.type_guid;
    desc.part_guid = entry.part_guid;
    desc.mbr_type = entry.mbr_type;
    desc.label = entry.label;
    if (!buildPartitionId(idSlice(&disk.canonical_id), entry.slot, &desc.canonical_id)) return;

    part.desc = desc;
    part.instance = driver.blockFingerprint(idSlice(&desc.canonical_id));
    if (part.instance == 0) return;
    part.in_use = true;
    g_part_count += 1;
    publishPartition(part, slot, devmgr);
}

// --- probing -----------------------------------------------------------------

/// Read a disk's table and publish what it holds.
///
/// GPT first, then MBR, then nothing. GPT is tried first because a GPT disk
/// carries a protective MBR that an MBR-first probe would read as one partition
/// spanning the device.
fn probeDisk(disk_index: usize, devmgr: i32) void {
    var disk = &g_disks[disk_index];
    g_table = .{};

    // GPT: the header lives at LBA 1.
    if (readSectors(disk, 1, 1, g_sector[0..])) {
        if (pt.parseGptHeader(g_sector[0..])) |header| {
            if (scanGptEntries(disk, header, &g_table)) {
                disk.scheme = .gpt;
                publishTable(disk_index, &g_table, devmgr);
                return;
            }
            // A header that parsed but whose entries did not is a damaged GPT,
            // not an MBR disk. Falling through to the legacy table would read
            // its protective entry as a real partition covering everything.
            var line = driver.Line{};
            _ = line.str("[partmgr] gpt entries invalid on ").str(idSlice(&disk.canonical_id));
            line.end();
            return;
        }
    }

    if (readSectors(disk, 0, 1, g_sector[0..])) {
        switch (pt.parseMbr(g_sector[0..], &g_table)) {
            .ok => {
                disk.scheme = .mbr;
                publishTable(disk_index, &g_table, devmgr);
                return;
            },
            .protective => {
                // The disk claims GPT but its header did not parse above.
                var line = driver.Line{};
                _ = line.str("[partmgr] protective mbr without a usable gpt on ");
                _ = line.str(idSlice(&disk.canonical_id));
                line.end();
                return;
            },
            .absent => {},
        }
    }

    // No table. Publish nothing and leave the disk alone: its own class instance
    // already serves it, which is what makes partition tables optional.
    var line = driver.Line{};
    _ = line.str("[partmgr] no partition table on ").str(idSlice(&disk.canonical_id));
    line.end();
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
/// The client's request is copied with two edits and nothing else: `target`
/// becomes the DISK's instance, and `lba` is rebased onto the partition's
/// window. The destination fields pass through untouched, so the backing device
/// still writes the client's own pages — a partition costs an IPC hop, not a
/// copy.
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

    const raw: [*]const u8 = @ptrCast(&req);
    if (g_down_req_bid < 0 or
        !driver.bufferWrite(g_down_req_bid, raw[0..@sizeOf(driver.BlockRequest)], 0))
    {
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
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NOT_READY);
        return;
    }

    slot.* = .{
        .client = msg.source,
        .client_request_id = msg.request_id,
        .down_request_id = down_id,
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

fn onMessage(user: ?*anyopaque, msg: *const co.IpcMessage) callconv(.c) void {
    _ = user;
    switch (msg.type) {
        op.BLOCK_IPC_IDENTIFY_REQ => handleIdentify(msg),
        op.BLOCK_IPC_READ_REQ, op.BLOCK_IPC_WRITE_REQ => handleTransfer(msg),
        op.BLOCK_IPC_READ_RESP, op.BLOCK_IPC_WRITE_RESP, op.BLOCK_IPC_ERROR => handleDownstreamReply(msg),
        op.SVC_IPC_CLASS_EVENT => handleClassEvent(msg),
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
        driver.log("[partmgr] devmgr inventory unavailable; partitions unpublished to it");
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
        driver.log("[partmgr] publish buffer grant failed; partitions unpublished to devmgr");
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
        driver.log("[partmgr] disk table full; a block provider went unprobed");
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

/// Record a `block` provider to be probed, and wake the root task to do it.
///
/// Two providers are dropped here rather than probed. Our OWN endpoint, because
/// every partition we publish registers under the same class and comes back to
/// us as an arrival: probing one would send IDENTIFY to ourselves and then block
/// on a reply only our own message loop could produce, which wedges the process.
/// And an instance already in the disk table, which is what makes the overlap
/// between the subscription and the opening enumeration harmless.
fn noteArrival(instance: u32, provider_endpoint: i32) void {
    if (provider_endpoint == endpoint()) return;
    if (diskKnown(instance)) return;
    for (&g_pending) |*a| {
        if (a.in_use and a.instance == instance) return;
    }
    for (&g_pending) |*a| {
        if (a.in_use) continue;
        a.* = .{ .instance = instance, .endpoint = provider_endpoint, .in_use = true };
        if (g_root_parked) {
            g_root_parked = false;
            _ = g_wake_promise.resolve(0);
        }
        return;
    }
    driver.log("[partmgr] arrival queue full; a block provider went unprobed");
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
        a.* = .{};
        if (diskKnown(instance)) continue;
        if (!probeProvider(instance, provider_endpoint)) continue;
        var line = driver.Line{};
        _ = line.str("[partmgr] late disk probed instance=").dec(instance);
        _ = line.str(" disks=").dec(g_disk_count).str(" partitions=").dec(g_part_count);
        line.end();
    }
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
        driver.log("[partmgr] block class subscribe failed; disks arriving later go unprobed");
    }

    var providers: [MAX_DISKS]driver.ClassEntry = undefined;
    const total = driver.lookupClass(g_proc_endpoint, "block", providers[0..], nextRequestId()) orelse {
        driver.log("[partmgr] block class lookup failed; no disks probed");
        return;
    };
    const found: usize = @intCast(total);
    const seen: usize = if (found < providers.len) found else providers.len;
    if (found > providers.len) {
        var line = driver.Line{};
        _ = line.str("[partmgr] ").dec(found);
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
        driver.log("[partmgr] no block buffer; cannot read a partition table");
        return;
    };
    g_probe_bid = driver.bufferAcquire(@sizeOf(driver.BlockDescriptor)) orelse {
        driver.log("[partmgr] descriptor buffer unavailable");
        return;
    };
    g_down_req_bid = driver.bufferAcquire(@sizeOf(driver.BlockRequest)) orelse {
        driver.log("[partmgr] request buffer unavailable");
        return;
    };
    g_probe_req_bid = driver.bufferAcquire(@sizeOf(driver.BlockRequest)) orelse {
        driver.log("[partmgr] probe request buffer unavailable");
        return;
    };
    g_publish_bid = driver.bufferAcquire(
        MAX_PARTITIONS * @sizeOf(driver.BlockDescriptor),
    ) orelse {
        driver.log("[partmgr] publish buffer unavailable");
        return;
    };

    discoverDisks();

    var line = driver.Line{};
    _ = line.str("[partmgr] ready disks=").dec(g_disk_count);
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
