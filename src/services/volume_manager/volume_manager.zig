//! volume_manager.zig — publishes every mountable thing as a `volume`.
//!
//! What it is
//! ----------
//! A `block` class CLIENT of every storage device, and the `volume` class
//! PROVIDER of every filesystem found on one. A volume is a thing with a
//! filesystem on it: a formatted raw disk is one, a formatted partition is one,
//! and above this layer partition-ness is not visible.
//!
//! Why the block class is not enough
//! ---------------------------------
//! `block` answers "what storage exists", which does not settle what can be
//! MOUNTED in either direction — a partition-table entry may hold no filesystem,
//! and a disk with no table may hold one. So a rule that wants a filesystem has
//! to name a disk and a unit, which is the thing mount policy is trying to stop
//! doing. See docs/architecture/37-volume-manager.md.
//!
//! What it does NOT do
//! -------------------
//! It does not mount, and it is not in the I/O path. The descriptor names the
//! `block` device underneath and a client reads THAT directly; nothing is
//! proxied here. The partition manager proxies because it must rebase every
//! transfer onto a window, and this has no such job — which is why it ships as a
//! service rather than a driver.
//!
//! A table suppresses a volume
//! ---------------------------
//! A device carrying a GPT or an MBR holds partitions, not a filesystem, and
//! publishes no volume of its own — its partitions are separate `block` devices
//! and get their own. Without that rule a partitioned disk and the partition on
//! it would both be mountable things covering the same sectors.
//!
//! "No superblock matched" is NOT that rule. An unrecognised format reads
//! exactly like a blank disk, and both are legitimate things to select with
//! `ATTR{fstype}=="unknown"`, so a device with no table and no recognised
//! filesystem still publishes. Only a TABLE suppresses.
//!
//! Discovery
//! ---------
//! Subscribe to `block` first, enumerate second — the order that leaves no gap,
//! copied from the partition manager along with the two arrivals it drops: our
//! own endpoint, and an instance already known. See
//! docs/architecture/36-partition-manager-and-block-identity.md §2.
const driver = @import("driver.zig");
const co = @import("coroutine.zig");
const op = @import("wasmos_opcodes.zig");
const status = @import("wasmos_status.zig");
const abi = @import("wasmos_constants.zig");
const rec = @import("recognise.zig");

/// Volumes published. One per mountable device; the bound is this service's own
/// state, not a property of the system.
const MAX_VOLUMES: usize = 16;
/// Backends lent the probe buffers. One entry per backend PROCESS, not per
/// device.
const MAX_BACKENDS: usize = 8;

const SECTOR_BYTES: u32 = 512;

const Volume = struct {
    /// This volume's own class instance: the fingerprint of its canonical id.
    instance: u32 = 0,
    desc: driver.VolumeDescriptor = .{},
    in_use: bool = false,
};

var g_volumes: [MAX_VOLUMES]Volume = [_]Volume{.{}} ** MAX_VOLUMES;
var g_volume_count: usize = 0;

var g_proc_endpoint: i32 = -1;
/// The device manager's inventory endpoint, resolved once and kept. Negative
/// when it could not be resolved, which downgrades publishing to class
/// registration alone -- and with it every `SUBSYSTEM=="volume"` rule, because
/// rules match the INVENTORY, not the class.
var g_devmgr: i32 = -1;

/// Backend endpoints this process has lent its probe buffers to. Grants are per
/// PROCESS, so one entry covers every device a backend serves and a second
/// attempt would be refused as ALREADY_BORROWED.
var g_granted: [MAX_BACKENDS]i32 = [_]i32{-1} ** MAX_BACKENDS;
var g_granted_count: usize = 0;

/// Devices seen under `block` but not yet probed. Queued rather than probed in
/// the handler: a probe is synchronous and a message handler must not block the
/// loop it runs on.
const Arrival = struct {
    instance: u32 = 0,
    endpoint: i32 = -1,
    in_use: bool = false,
};
var g_pending: [MAX_VOLUMES]Arrival = [_]Arrival{.{}} ** MAX_VOLUMES;

/// The root task parks on this and a handler settles it. Returning `yielded`
/// WITHOUT awaiting is not parking: the coroutine stays runnable, the runner
/// never reaches its poll, and the process spins at full CPU instead of
/// sleeping.
var g_wake_future: co.Future = .{};
var g_wake_promise: co.Promise = .{};
var g_root_parked: bool = false;
var g_next_request_id: i32 = 0x7400;

/// Buffer a backend writes a block descriptor into when we IDENTIFY a device.
var g_ident_bid: i32 = -1;
/// Buffer holding the read request we send downstream, reused for every probe.
var g_req_bid: i32 = -1;
/// Buffer the device manager reads published volume descriptors out of: one slot
/// per volume, lent READ for the life of the process.
///
/// Slotted rather than reused, and lent rather than merely written, because a
/// publish is fire-and-forget: the receiver reads the bytes at a moment of its
/// choosing, so a single slot rewritten by the next publish hands it whichever
/// descriptor arrived last, and a buffer it was never lent it cannot read at
/// all. The partition manager shipped without either and every publish was
/// refused at the read while class registration still succeeded -- so it looked
/// cosmetic while every rule addressed an empty registry.
var g_publish_bid: i32 = -1;
/// Physical address of this process's block buffer, where probe reads land.
var g_block_phys: u32 = 0;

/// The prefix every recogniser shares, read once per device. Static rather than
/// a stack local: the Zig shadow stack is 8 KiB (see cmake/WasmosZigApp.cmake)
/// and 4 KiB of it would leave no room for the descriptors beside it. Probing is
/// single-threaded and non-reentrant, so one copy is enough.
var g_prefix: [rec.PREFIX_BYTES]u8 = [_]u8{0} ** rec.PREFIX_BYTES;

fn nextRequestId() i32 {
    g_next_request_id += 1;
    if (g_next_request_id < 0) g_next_request_id = 0x7400;
    return g_next_request_id;
}

fn idSlice(id: []const u8) []const u8 {
    var n: usize = 0;
    while (n < id.len and id[n] != 0) : (n += 1) {}
    return id[0..n];
}

/// Build a volume's canonical id: `volume:` prefixed to the backing device's.
/// Returns false if it does not fit, which must fail the registration — a
/// truncated id fingerprints to a different value than anyone looking for it
/// would compute.
fn buildVolumeId(device_id: []const u8, out: *[driver.VOLUME_ID_MAX]u8) bool {
    const prefix = "volume:";
    for (out) |*b| b.* = 0;
    if (device_id.len == 0) return false;
    if (prefix.len + device_id.len + 1 > out.len) return false;
    var n: usize = 0;
    for (prefix) |c| {
        out[n] = c;
        n += 1;
    }
    for (device_id) |c| {
        out[n] = c;
        n += 1;
    }
    return true;
}

// --- talking to a device (synchronous, probe only) ---------------------------

/// IDENTIFY a block device into `out`.
fn identifyDevice(endpoint_id: i32, instance: u32, out: *driver.BlockDescriptor) bool {
    if (g_ident_bid < 0) return false;
    const reply = driver.call(
        endpoint_id,
        driver.privateReplyEndpoint(),
        op.BLOCK_IPC_IDENTIFY_REQ,
        nextRequestId(),
        @intCast(instance),
        g_ident_bid,
        0,
        0,
    ) orelse return false;
    if (reply.type != op.BLOCK_IPC_IDENTIFY_RESP or reply.arg0 != 0) return false;
    if (reply.arg1 < @as(i32, @intCast(@sizeOf(driver.BlockDescriptor)))) return false;
    return driver.bufferRead(g_ident_bid, out, 0);
}

/// Read the recogniser prefix from a device's OWN LBA 0 into `g_prefix`.
///
/// "Its own LBA 0" is load-bearing. The partition manager rebases every
/// forwarded transfer onto the partition's window, so a descriptor's `lba_start`
/// says where the volume SITS and is never an address a client sends. Seeking to
/// it here would read past the superblock this is looking for.
fn readPrefix(endpoint_id: i32, instance: u32, sectors_available: u64) bool {
    if (g_req_bid < 0 or g_block_phys == 0) return false;
    const want: u32 = @intCast(rec.PREFIX_BYTES / SECTOR_BYTES);
    const count: u32 = if (sectors_available < want) @intCast(sectors_available) else want;
    if (count == 0) return false;

    var req = driver.BlockRequest{};
    req.target = instance;
    req.lba = 0;
    req.sector_count = count;
    req.dst_kind = driver.BLOCK_DST_BLOCK_BUFFER;
    req.dst_phys = g_block_phys;

    const raw: [*]const u8 = @ptrCast(&req);
    if (!driver.bufferWrite(g_req_bid, raw[0..@sizeOf(driver.BlockRequest)], 0)) return false;

    const reply = driver.call(
        endpoint_id,
        driver.privateReplyEndpoint(),
        op.BLOCK_IPC_READ_REQ,
        nextRequestId(),
        g_req_bid,
        0,
        @intCast(@sizeOf(driver.BlockRequest)),
        0,
    ) orelse return false;
    if (reply.type != op.BLOCK_IPC_READ_RESP or reply.arg0 != 0) return false;

    for (&g_prefix) |*b| b.* = 0;
    return driver.blockBufferCopy(g_block_phys, g_prefix[0 .. count * SECTOR_BYTES], 0);
}

// --- publishing --------------------------------------------------------------

fn publishVolume(vol: *Volume, slot: usize) void {
    if (driver.registerService(
        g_proc_endpoint,
        endpoint(),
        "volume",
        "volume",
        vol.instance,
        nextRequestId(),
    ) == null) {
        var line = driver.Line{};
        _ = line.str("[volume-manager] class register failed id=");
        _ = line.str(idSlice(&vol.desc.canonical_id));
        line.end();
        return;
    }

    if (g_devmgr >= 0 and g_publish_bid >= 0 and slot < MAX_VOLUMES) {
        const offset: u32 = @intCast(slot * @sizeOf(driver.VolumeDescriptor));
        const raw: [*]const u8 = @ptrCast(&vol.desc);
        if (driver.bufferWrite(g_publish_bid, raw[0..@sizeOf(driver.VolumeDescriptor)], offset)) {
            _ = driver.send(
                g_devmgr,
                endpoint(),
                op.DEVMGR_PUBLISH_VOLUME,
                0,
                g_publish_bid,
                @intCast(offset),
                @intCast(@sizeOf(driver.VolumeDescriptor)),
                0,
            );
        }
    }

    var line = driver.Line{};
    _ = line.str("[volume-manager] volume id=").str(idSlice(&vol.desc.canonical_id));
    _ = line.str(" fstype=").str(fsTypeName(vol.desc.fs_type));
    if ((vol.desc.flags & @as(u32, @intCast(abi.VOLUME_DESCRIPTOR_FLAG_HAS_LABEL))) != 0) {
        _ = line.str(" label=").str(idSlice(&vol.desc.label));
    }
    // Reported in the spelling ATTR{uuid} takes, hyphenated at the canonical
    // GUID groups when the format's identity is that wide. A rule selects a
    // volume by this value, and nothing else on the system prints it: a FAT
    // serial otherwise has to be read out of the boot sector by hand, and a WFS
    // uuid recovered from whatever mkfs_wfs printed at format time.
    if ((vol.desc.flags & @as(u32, @intCast(abi.VOLUME_DESCRIPTOR_FLAG_HAS_UUID))) != 0) {
        _ = line.str(" uuid=");
        var i: usize = 0;
        while (i < vol.desc.uuid_len and i < vol.desc.uuid.len) : (i += 1) {
            if (vol.desc.uuid_len == 16 and (i == 4 or i == 6 or i == 8 or i == 10)) {
                _ = line.str("-");
            }
            const b = vol.desc.uuid[i];
            _ = line.str(HEX_LOWER[b >> 4 ..][0..1]).str(HEX_LOWER[b & 0x0f ..][0..1]);
        }
    }
    _ = line.str(" sectors=").dec(vol.desc.lba_count);
    _ = line.str(" instance=").dec(vol.instance);
    line.end();
}

/// Lower case deliberately: mkfs_wfs prints a uuid in lower case, and a rule's
/// value gets pasted from one report or the other. The rule parser accepts
/// either case, so this is about the two reports agreeing on sight, not about
/// what parses.
const HEX_LOWER = "0123456789abcdef";

fn fsTypeName(fs_type: u32) []const u8 {
    if (fs_type == @as(u32, @intCast(abi.FS_TYPE_FAT))) return "fat";
    if (fs_type == @as(u32, @intCast(abi.FS_TYPE_WFS))) return "wfs";
    return "unknown";
}

// --- probing -----------------------------------------------------------------

fn volumeKnown(backing: u32) bool {
    for (g_volumes[0..g_volume_count]) |v| {
        if (v.in_use and v.desc.backing_instance == backing) return true;
    }
    return false;
}

fn grantBackend(ep: i32) void {
    for (g_granted[0..g_granted_count]) |seen| {
        if (seen == ep) return;
    }
    if (g_granted_count >= g_granted.len) return;
    _ = driver.bufferBorrow(ep, g_ident_bid, driver.BUFFER_GRANT_WRITE);
    _ = driver.bufferBorrow(ep, g_req_bid, driver.BUFFER_GRANT_READ);
    g_granted[g_granted_count] = ep;
    g_granted_count += 1;
}

/// Probe one `block` device and publish a volume for it, unless it holds a
/// partition table.
fn probeDevice(instance: u32, provider_endpoint: i32) void {
    if (g_volume_count >= MAX_VOLUMES) {
        driver.log("[volume-manager] volume table full; a device went unprobed");
        return;
    }
    grantBackend(provider_endpoint);

    var dev = driver.BlockDescriptor{};
    if (!identifyDevice(provider_endpoint, instance, &dev)) return;
    if (dev.version != driver.BLOCK_DESCRIPTOR_VERSION) return;
    if (!readPrefix(provider_endpoint, instance, dev.lba_count)) {
        var line = driver.Line{};
        _ = line.str("[volume-manager] prefix read failed on ").str(idSlice(&dev.canonical_id));
        line.end();
        return;
    }

    // A table means partitions, and a partition is its own `block` device with
    // its own volume. Publishing here too would make the disk and the partition
    // on it two mountable things over the same sectors.
    const scheme = rec.detectScheme(g_prefix[0..]);
    if (scheme != .none) {
        var line = driver.Line{};
        _ = line.str("[volume-manager] ").str(idSlice(&dev.canonical_id));
        _ = line.str(" holds a ").str(if (scheme == .gpt) "gpt" else "mbr");
        _ = line.str("; its partitions are the volumes");
        line.end();
        return;
    }

    const verdict = rec.recognise(g_prefix[0..]);

    const slot = g_volume_count;
    var vol = &g_volumes[slot];
    vol.* = .{};

    var desc = driver.VolumeDescriptor{};
    desc.fs_type = verdict.fs_type;
    desc.backing_instance = instance;
    desc.flags = @intCast(abi.VOLUME_DESCRIPTOR_FLAG_PRESENT);
    desc.lba_start = 0;
    desc.lba_count = dev.lba_count;
    desc.sector_bytes = if (dev.sector_bytes != 0) dev.sector_bytes else SECTOR_BYTES;
    if (verdict.has_label) {
        desc.flags |= @as(u32, @intCast(abi.VOLUME_DESCRIPTOR_FLAG_HAS_LABEL));
        const n = if (verdict.label.len < desc.label.len) verdict.label.len else desc.label.len;
        var i: usize = 0;
        while (i < n) : (i += 1) desc.label[i] = verdict.label[i];
    }
    if (verdict.has_uuid) {
        desc.flags |= @as(u32, @intCast(abi.VOLUME_DESCRIPTOR_FLAG_HAS_UUID));
        desc.uuid = verdict.uuid;
        desc.uuid_len = verdict.uuid_len;
    }
    if ((dev.flags & @as(u32, @intCast(abi.BLOCK_DESCRIPTOR_FLAG_READ_ONLY))) != 0) {
        desc.flags |= @as(u32, @intCast(abi.VOLUME_DESCRIPTOR_FLAG_READ_ONLY));
    }
    if (!buildVolumeId(idSlice(&dev.canonical_id), &desc.canonical_id)) return;

    vol.desc = desc;
    vol.instance = driver.blockFingerprint(idSlice(&desc.canonical_id));
    if (vol.instance == 0) return;
    vol.in_use = true;
    g_volume_count += 1;
    publishVolume(vol, slot);
}

// --- discovery ---------------------------------------------------------------

fn noteArrival(instance: u32, provider_endpoint: i32) void {
    // Our own endpoint serves the `volume` class, not `block`, so it can never
    // appear here -- unlike the partition manager, which publishes INTO the class
    // it subscribes to. Checked anyway: the cost is one comparison and the
    // failure it prevents is IDENTIFYing ourselves and blocking forever.
    if (provider_endpoint == endpoint()) return;
    if (volumeKnown(instance)) return;
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
    driver.log("[volume-manager] arrival queue full; a device went unprobed");
}

/// Probe every queued arrival. Runs on the root task, which is this process's
/// own thread of control: a probe is synchronous, and running it from the
/// message handler would block the loop that handler was dispatched from.
fn drainArrivals() void {
    for (&g_pending) |*a| {
        if (!a.in_use) continue;
        const instance = a.instance;
        const provider_endpoint = a.endpoint;
        a.* = .{};
        if (volumeKnown(instance)) continue;
        probeDevice(instance, provider_endpoint);
        // A RUNNING TOTAL, because `ready volumes=` no longer carries one. This
        // service starts from the initfs, ahead of every disk driver, so its
        // startup sweep sees nothing and each device is probed as it registers.
        // Without this line the log would report zero volumes and never correct
        // itself. The partition manager reports its own totals the same way.
        var line = driver.Line{};
        _ = line.str("[volume-manager] device probed instance=").dec(instance);
        _ = line.str(" volumes=").dec(g_volume_count);
        line.end();
    }
}

/// Subscribe to the `block` class, then probe every device already registered.
/// Subscription FIRST: a device registering between the two fires an event that
/// is waiting when the loop starts, where subscribing afterwards would lose it.
/// Resolve the device manager's inventory endpoint and lend it the publish
/// buffer. Resolved once; the result, including a failure, stands for the life
/// of the process.
fn resolveDevmgr() void {
    const ep = driver.lookupService(g_proc_endpoint, "devmgr.inv", nextRequestId(), 64) orelse {
        driver.log("[volume-manager] devmgr inventory unavailable; volumes unpublished to it");
        return;
    };
    if (g_publish_bid < 0 or
        driver.bufferBorrow(ep, g_publish_bid, driver.BUFFER_GRANT_READ) == null)
    {
        driver.log("[volume-manager] publish buffer grant failed; volumes unpublished to devmgr");
        return;
    }
    g_devmgr = ep;
}

fn discoverVolumes() void {
    resolveDevmgr();

    if (!driver.subscribeClass(g_proc_endpoint, endpoint(), "block", nextRequestId())) {
        driver.log("[volume-manager] block class subscribe failed; later devices go unprobed");
    }

    var providers: [MAX_VOLUMES]driver.ClassEntry = undefined;
    const total = driver.lookupClass(g_proc_endpoint, "block", providers[0..], nextRequestId()) orelse {
        driver.log("[volume-manager] block class lookup failed; no devices probed");
        return;
    };
    const found: usize = @intCast(total);
    const seen: usize = if (found < providers.len) found else providers.len;
    if (found > providers.len) {
        var line = driver.Line{};
        _ = line.str("[volume-manager] ").dec(found);
        _ = line.str(" block providers present, probing the first ").dec(providers.len);
        line.end();
    }

    var i: usize = 0;
    while (i < seen) : (i += 1) {
        if (volumeKnown(providers[i].instance)) continue;
        probeDevice(providers[i].instance, @intCast(providers[i].endpoint));
    }
}

// --- serving -----------------------------------------------------------------

fn findVolume(instance: u32) ?*Volume {
    for (&g_volumes) |*v| {
        if (v.in_use and v.instance == instance) return v;
    }
    return null;
}

fn sendError(dest: i32, request_id: i32, code: i32) void {
    _ = driver.send(dest, endpoint(), op.VOLUME_IPC_ERROR, request_id, code, 0, 0, 0);
}

/// Answer IDENTIFY by writing a descriptor into the CLIENT's buffer, exactly as
/// a block backend answers for a disk.
fn handleIdentify(msg: *const co.IpcMessage) void {
    const vol = findVolume(@bitCast(msg.arg0)) orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NO_SUCH_UNIT);
        return;
    };
    if (msg.arg1 <= 0) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_BAD_REQUEST);
        return;
    }
    const raw: [*]const u8 = @ptrCast(&vol.desc);
    if (!driver.bufferWrite(msg.arg1, raw[0..@sizeOf(driver.VolumeDescriptor)], 0)) {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NO_DESCRIPTOR);
        return;
    }
    _ = driver.send(
        msg.source,
        endpoint(),
        op.VOLUME_IPC_IDENTIFY_RESP,
        msg.request_id,
        0,
        @intCast(@sizeOf(driver.VolumeDescriptor)),
        0,
        0,
    );
}

/// Record or release a mount claim. Advisory: this service is not in the I/O
/// path, so a tool that does not ask is not stopped. It is what `fsck` and
/// `mkfs` consult before touching a volume that may be mounted.
fn handleClaim(msg: *const co.IpcMessage) void {
    const vol = findVolume(@bitCast(msg.arg0)) orelse {
        sendError(msg.source, msg.request_id, status.WASMOS_ERR_BLOCK_DEV_NO_SUCH_UNIT);
        return;
    };
    const bit: u32 = @intCast(abi.VOLUME_DESCRIPTOR_FLAG_CLAIMED);
    if (msg.arg1 != 0) vol.desc.flags |= bit else vol.desc.flags &= ~bit;
    _ = driver.send(msg.source, endpoint(), op.VOLUME_IPC_RESP, msg.request_id, 0, 0, 0, 0);
}

/// An existence event for the `block` class: arg0 names the kind, arg1 the
/// instance, arg2 the provider's endpoint, arg3 its pid.
///
/// REMOVE is not acted on, for the same reason the partition manager does not:
/// retiring a volume means unregistering it and telling every consumer, which is
/// a teardown path with no caller today. Dropping the record alone would leave
/// the class advertising a volume nothing can reach.
/// TODO: retire a volume when its backing device goes away.
fn handleClassEvent(msg: *const co.IpcMessage) void {
    if (msg.arg0 != @as(i32, @intCast(abi.SVC_CLASS_EVENT_ADD))) return;
    noteArrival(@bitCast(msg.arg1), msg.arg2);
}

fn onMessage(user: ?*anyopaque, msg: *const co.IpcMessage) callconv(.c) void {
    _ = user;
    switch (msg.type) {
        op.VOLUME_IPC_IDENTIFY_REQ => handleIdentify(msg),
        op.VOLUME_IPC_CLAIM_REQ => handleClaim(msg),
        op.SVC_IPC_CLASS_EVENT => handleClassEvent(msg),
        else => {},
    }
}

// --- async service contract --------------------------------------------------

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

fn endpoint() i32 {
    return wasmos_async_service.reply_endpoint;
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
        driver.log("[volume-manager] no block buffer; cannot read a superblock");
        return;
    };
    g_ident_bid = driver.bufferAcquire(@sizeOf(driver.BlockDescriptor)) orelse {
        driver.log("[volume-manager] descriptor buffer unavailable");
        return;
    };
    g_req_bid = driver.bufferAcquire(@sizeOf(driver.BlockRequest)) orelse {
        driver.log("[volume-manager] request buffer unavailable");
        return;
    };
    g_publish_bid = driver.bufferAcquire(
        MAX_VOLUMES * @sizeOf(driver.VolumeDescriptor),
    ) orelse {
        driver.log("[volume-manager] publish buffer unavailable");
        return;
    };

    discoverVolumes();

    var line = driver.Line{};
    _ = line.str("[volume-manager] ready volumes=").dec(g_volume_count);
    line.end();

    driver.notifyReady(g_proc_endpoint);
}

/// Probes devices that arrive after bring-up, and otherwise parks. A stackless
/// task resumes from the TOP, so draining before the await is what makes this a
/// loop rather than a one-shot.
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
